#include "Driver64.h"
#include "DriverGuard64.h"

#include <ntimage.h>
#include <bcrypt.h>

#define JDRV_GUARD_POOL_TAG 'grdJ'
#define JDRV_GUARD_FILE_CHUNK_SIZE (64UL * 1024UL)
#define JDRV_GUARD_PATCH_SIZE 8UL
#define JDRV_GUARD_MAX_NAME_MATCHES 8UL

// DriverEntry stub that makes the driver fail to initialize:
//   48 C7 C0 22 00 00 C0    mov rax, 0FFFFFFFFC0000022h   ; STATUS_ACCESS_DENIED
//   C3                      ret
static const UCHAR g_JdrvGuardEntryStub[JDRV_GUARD_PATCH_SIZE] = {
    0x48, 0xC7, 0xC0, 0x22, 0x00, 0x00, 0xC0, 0xC3
};

typedef struct _JDRV_GUARD_STORE_ENTRY {
    WCHAR Name[JDRV_GUARD_NAME_CHARS];
    UCHAR Sha256[JDRV_GUARD_HASH_SIZE];
    ULONGLONG FileSize;
    BOOLEAN HashValid;
} JDRV_GUARD_STORE_ENTRY, *PJDRV_GUARD_STORE_ENTRY;

typedef struct _JDRV_GUARD_HASH_CONTEXT {
    BCRYPT_ALG_HANDLE Algorithm;
    BCRYPT_HASH_HANDLE Hash;
    PUCHAR ObjectBuffer;
    ULONG ObjectLength;
} JDRV_GUARD_HASH_CONTEXT, *PJDRV_GUARD_HASH_CONTEXT;

static EX_PUSH_LOCK g_JdrvGuardLock;
static PJDRV_GUARD_STORE_ENTRY g_JdrvGuardEntries;
static ULONG g_JdrvGuardEntryCount;
static ULONG g_JdrvGuardEntryCapacity;
static BOOLEAN g_JdrvGuardArmed;
static PVOID g_JdrvGuardSelfImageBase;

static volatile LONG64 g_JdrvGuardEvaluated;
static volatile LONG64 g_JdrvGuardAllowed;
static volatile LONG64 g_JdrvGuardBlocked;
static volatile LONG64 g_JdrvGuardPatchFailures;

static NTSTATUS
JdrvGuardHashInitialize(
    _Out_ PJDRV_GUARD_HASH_CONTEXT Context
    )
{
    ULONG resultLength = 0UL;
    ULONG hashLength = 0UL;
    NTSTATUS status;

    RtlZeroMemory(Context, sizeof(*Context));
    status = BCryptOpenAlgorithmProvider(
        &Context->Algorithm,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BCryptGetProperty(
        Context->Algorithm,
        BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&Context->ObjectLength,
        sizeof(Context->ObjectLength),
        &resultLength,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BCryptGetProperty(
        Context->Algorithm,
        BCRYPT_HASH_LENGTH,
        (PUCHAR)&hashLength,
        sizeof(hashLength),
        &resultLength,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (hashLength != JDRV_GUARD_HASH_SIZE) {
        return STATUS_NOT_SUPPORTED;
    }

    Context->ObjectBuffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        Context->ObjectLength,
        JDRV_GUARD_POOL_TAG);
    if (Context->ObjectBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return BCryptCreateHash(
        Context->Algorithm,
        &Context->Hash,
        Context->ObjectBuffer,
        Context->ObjectLength,
        NULL,
        0UL,
        0UL);
}

static VOID
JdrvGuardHashUninitialize(
    _Inout_ PJDRV_GUARD_HASH_CONTEXT Context
    )
{
    if (Context->Hash != NULL) {
        (VOID)BCryptDestroyHash(Context->Hash);
    }
    if (Context->ObjectBuffer != NULL) {
        RtlSecureZeroMemory(Context->ObjectBuffer, Context->ObjectLength);
        ExFreePoolWithTag(Context->ObjectBuffer, JDRV_GUARD_POOL_TAG);
    }
    if (Context->Algorithm != NULL) {
        (VOID)BCryptCloseAlgorithmProvider(Context->Algorithm, 0UL);
    }
    RtlZeroMemory(Context, sizeof(*Context));
}

// Streams the file through SHA-256. The image notify callback runs at
// PASSIVE_LEVEL in the loading thread, so synchronous reads are safe here.
static NTSTATUS
JdrvGuardHashFile(
    _In_ PUNICODE_STRING FilePath,
    _Out_writes_bytes_(JDRV_GUARD_HASH_SIZE) UCHAR Hash[JDRV_GUARD_HASH_SIZE]
    )
{
    JDRV_GUARD_HASH_CONTEXT hashContext;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatus;
    HANDLE fileHandle = NULL;
    PUCHAR buffer = NULL;
    NTSTATUS status;

    InitializeObjectAttributes(
        &objectAttributes,
        FilePath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);
    status = ZwCreateFile(
        &fileHandle,
        FILE_READ_DATA,
        &objectAttributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        JDRV_GUARD_FILE_CHUNK_SIZE,
        JDRV_GUARD_POOL_TAG);
    if (buffer == NULL) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = JdrvGuardHashInitialize(&hashContext);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buffer, JDRV_GUARD_POOL_TAG);
        ZwClose(fileHandle);
        return status;
    }

    for (;;) {
        status = ZwReadFile(
            fileHandle,
            NULL,
            NULL,
            NULL,
            &ioStatus,
            buffer,
            JDRV_GUARD_FILE_CHUNK_SIZE,
            NULL,
            NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (ioStatus.Information == 0UL) {
            status = STATUS_SUCCESS;
            break;
        }
        status = BCryptHashData(
            hashContext.Hash,
            buffer,
            (ULONG)ioStatus.Information,
            0UL);
        if (!NT_SUCCESS(status)) {
            break;
        }
    }

    if (NT_SUCCESS(status)) {
        status = BCryptFinishHash(
            hashContext.Hash,
            Hash,
            JDRV_GUARD_HASH_SIZE,
            0UL);
    }

    JdrvGuardHashUninitialize(&hashContext);
    ExFreePoolWithTag(buffer, JDRV_GUARD_POOL_TAG);
    ZwClose(fileHandle);
    return status;
}

static VOID
JdrvGuardLowercaseName(
    _Out_writes_(DestinationChars) PWCH Destination,
    _In_reads_(SourceChars) const WCHAR* Source,
    _In_ ULONG SourceChars,
    _In_ ULONG DestinationChars
    )
{
    ULONG index;

    if (DestinationChars == 0UL) {
        return;
    }
    for (index = 0UL; index < SourceChars && index + 1UL < DestinationChars;
         ++index) {
        PWCH character = &Destination[index];
        *character = Source[index];
        if (*character >= L'A' && *character <= L'Z') {
            *character = *character + (L'a' - L'A');
        }
    }
    Destination[index] = L'\0';
}

static BOOLEAN
JdrvGuardNameEquals(
    _In_ PCWSTR Left,
    _In_ PCWSTR Right
    )
{
    ULONG index;

    for (index = 0UL; index < JDRV_GUARD_NAME_CHARS; ++index) {
        if (Left[index] != Right[index]) {
            return FALSE;
        }
        if (Left[index] == L'\0') {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
JdrvGuardQueueBlockedEvent(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ PIMAGE_INFO ImageInfo,
    _In_ ULONG Reason
    )
{
    JDRV_EVENT_RECORD eventRecord;
    LARGE_INTEGER systemTime;
    ULONG processId = HandleToULong(PsGetCurrentProcessId());
    ULONG pathLength;

    RtlZeroMemory(&eventRecord, sizeof(eventRecord));
    KeQuerySystemTime(&systemTime);
    eventRecord.size = sizeof(eventRecord);
    eventRecord.version = JDRV_EVENT_VERSION;
    eventRecord.type = JDRV_EVENT_TYPE_DRIVER_BLOCKED;
    eventRecord.flags = JDRV_EVENT_FLAG_SYSTEM_IMAGE | JDRV_EVENT_FLAG_DRIVER_GUARD;
    eventRecord.timestamp = (ULONGLONG)systemTime.QuadPart;
    eventRecord.processId = processId;
    eventRecord.reserved = Reason;
    eventRecord.imageBase = (ULONGLONG)(ULONG_PTR)ImageInfo->ImageBase;
    eventRecord.imageSize = (ULONGLONG)ImageInfo->ImageSize;
    if (FullImageName != NULL && FullImageName->Buffer != NULL) {
        pathLength = FullImageName->Length / sizeof(WCHAR);
        if (pathLength >= JDRV_EVENT_IMAGE_PATH_CHARS) {
            pathLength = JDRV_EVENT_IMAGE_PATH_CHARS - 1UL;
        }
        RtlCopyMemory(
            eventRecord.imagePath,
            FullImageName->Buffer,
            pathLength * sizeof(WCHAR));
        eventRecord.imagePathLength = pathLength;
    }
    JdrvQueueEvent(&eventRecord);
}

// Maps the target range through an MDL so the write lands on the same
// physical pages regardless of the read-only PTEs of the original mapping.
static BOOLEAN
JdrvGuardWriteReadOnly(
    _In_ PVOID Address,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    PMDL mdl = NULL;
    PVOID mapped = NULL;
    BOOLEAN locked = FALSE;
    BOOLEAN written = FALSE;

    mdl = IoAllocateMdl(Address, Length, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        return FALSE;
    }

    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
        locked = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        locked = FALSE;
    }

    if (!locked) {
        // Read-only kernel pages reject a write lock on some builds. Lock for
        // read instead and lift the write protection of the alias mapping.
        __try {
            MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
            locked = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            locked = FALSE;
        }
        if (locked) {
            (VOID)MmProtectMdlSystemAddress(mdl, PAGE_READWRITE);
        }
    }

    if (locked) {
        mapped = MmMapLockedPagesSpecifyCache(
            mdl,
            KernelMode,
            MmCached,
            NULL,
            FALSE,
            NormalPagePriority);
        if (mapped != NULL) {
            RtlCopyMemory(mapped, Data, Length);
            written = RtlEqualMemory(mapped, Data, Length);
            MmUnmapLockedPages(mapped, mdl);
        }
        MmUnlockPages(mdl);
    }

    IoFreeMdl(mdl);
    return written;
}

static NTSTATUS
JdrvGuardPatchDriverEntry(
    _In_ PVOID ImageBase,
    _In_ SIZE_T ImageSize
    )
{
    const IMAGE_DOS_HEADER* dosHeader = NULL;
    const IMAGE_NT_HEADERS64* ntHeaders = NULL;
    ULONG entryRva = 0UL;
    PUCHAR entryVa = NULL;
    BOOLEAN headersValid = FALSE;

    // Parse the PE headers of the freshly mapped image by hand: this WDK
    // no longer declares RtlImageNtHeader and the image is trusted to have
    // valid headers because the kernel loader has just accepted it.
    __try {
        if (ImageSize >= sizeof(*dosHeader)) {
            dosHeader = (const IMAGE_DOS_HEADER*)ImageBase;
            if (dosHeader->e_magic == IMAGE_DOS_SIGNATURE &&
                dosHeader->e_lfanew > 0 &&
                ImageSize - (SIZE_T)(ULONG)dosHeader->e_lfanew >=
                    sizeof(*ntHeaders)) {
                ntHeaders = (const IMAGE_NT_HEADERS64*)
                    ((const UCHAR*)ImageBase + (ULONG)dosHeader->e_lfanew);
                if (ntHeaders->Signature == IMAGE_NT_SIGNATURE &&
                    ntHeaders->OptionalHeader.Magic ==
                        IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                    headersValid = TRUE;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        headersValid = FALSE;
    }
    if (!headersValid) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    entryRva = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    if (entryRva == 0UL || ImageSize < JDRV_GUARD_PATCH_SIZE ||
        entryRva > ImageSize - JDRV_GUARD_PATCH_SIZE) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    entryVa = (PUCHAR)ImageBase + entryRva;
    if (!JdrvGuardWriteReadOnly(entryVa, g_JdrvGuardEntryStub, JDRV_GUARD_PATCH_SIZE)) {
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

// Looks up the image basename in the whitelist. Returns TRUE with the
// matching entries copied to Matches when the name is whitelisted.
static BOOLEAN
JdrvGuardFindMatches(
    _In_ PCWSTR Name,
    _Out_writes_(JDRV_GUARD_MAX_NAME_MATCHES) PJDRV_GUARD_STORE_ENTRY Matches,
    _Out_ PULONG MatchCount
    )
{
    ULONG index;
    ULONG count = 0UL;
    BOOLEAN found = FALSE;

    *MatchCount = 0UL;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_JdrvGuardLock);
    for (index = 0UL; index < g_JdrvGuardEntryCount; ++index) {
        if (JdrvGuardNameEquals(g_JdrvGuardEntries[index].Name, Name)) {
            found = TRUE;
            if (count < JDRV_GUARD_MAX_NAME_MATCHES) {
                Matches[count] = g_JdrvGuardEntries[index];
                ++count;
            }
        }
    }
    ExReleasePushLockShared(&g_JdrvGuardLock);
    KeLeaveCriticalRegion();
    *MatchCount = count;
    return found;
}

static VOID
JdrvGuardBlockImage(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ PIMAGE_INFO ImageInfo,
    _In_ ULONG Reason
    )
{
    NTSTATUS patchStatus;

    patchStatus = JdrvGuardPatchDriverEntry(
        ImageInfo->ImageBase,
        ImageInfo->ImageSize);
    if (!NT_SUCCESS(patchStatus)) {
        InterlockedIncrement64(&g_JdrvGuardPatchFailures);
        Reason = JDRV_GUARD_BLOCK_REASON_PATCH_FAILED;
    }
    InterlockedIncrement64(&g_JdrvGuardBlocked);
    JdrvGuardQueueBlockedEvent(FullImageName, ImageInfo, Reason);
}

VOID
JdrvDriverGuardInspectImage(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ PIMAGE_INFO ImageInfo
    )
{
    WCHAR basename[JDRV_GUARD_NAME_CHARS];
    JDRV_GUARD_STORE_ENTRY matches[JDRV_GUARD_MAX_NAME_MATCHES];
    ULONG matchCount = 0UL;
    ULONG index;
    UCHAR fileHash[JDRV_GUARD_HASH_SIZE];
    BOOLEAN hashVerified = FALSE;
    BOOLEAN hashMatches = FALSE;
    BOOLEAN hashRequired = FALSE;

    if (!g_JdrvGuardArmed || ImageInfo == NULL || !ImageInfo->SystemModeImage) {
        return;
    }
    if (ImageInfo->ImageBase == g_JdrvGuardSelfImageBase) {
        return;
    }
    if (FullImageName == NULL || FullImageName->Buffer == NULL ||
        FullImageName->Length == 0UL) {
        return;
    }

    InterlockedIncrement64(&g_JdrvGuardEvaluated);

    // Extract the basename: the whitelist is keyed by file name, hashes
    // protect against a different file reusing a whitelisted name.
    {
        const PWCH buffer = FullImageName->Buffer;
        ULONG length = FullImageName->Length / sizeof(WCHAR);
        ULONG end = length;
        ULONG begin = 0UL;

        while (end > 0UL && buffer[end - 1UL] == L'\\') {
            --end;
        }
        for (index = end; index > 0UL; --index) {
            if (buffer[index - 1UL] == L'\\') {
                begin = index;
                break;
            }
        }
        if (end <= begin || end - begin >= JDRV_GUARD_NAME_CHARS) {
            // No usable name: cannot judge, fail open to avoid breaking
            // internal system image loads.
            InterlockedIncrement64(&g_JdrvGuardAllowed);
            return;
        }
        JdrvGuardLowercaseName(basename, buffer + begin, end - begin, JDRV_GUARD_NAME_CHARS);
    }
    if (basename[0] == L'\0') {
        InterlockedIncrement64(&g_JdrvGuardAllowed);
        return;
    }

    if (!JdrvGuardFindMatches(basename, matches, &matchCount)) {
        JdrvGuardBlockImage(FullImageName, ImageInfo, JDRV_GUARD_BLOCK_REASON_NOT_WHITELISTED);
        return;
    }

    for (index = 0UL; index < matchCount; ++index) {
        if (matches[index].HashValid) {
            hashRequired = TRUE;
            break;
        }
    }
    if (!hashRequired) {
        InterlockedIncrement64(&g_JdrvGuardAllowed);
        return;
    }

    if (NT_SUCCESS(JdrvGuardHashFile(FullImageName, fileHash))) {
        hashVerified = TRUE;
        for (index = 0UL; index < matchCount; ++index) {
            if (matches[index].HashValid &&
                RtlEqualMemory(
                    matches[index].Sha256,
                    fileHash,
                    JDRV_GUARD_HASH_SIZE)) {
                hashMatches = TRUE;
                break;
            }
        }
    }

    if (hashVerified && !hashMatches) {
        JdrvGuardBlockImage(FullImageName, ImageInfo, JDRV_GUARD_BLOCK_REASON_HASH_MISMATCH);
        return;
    }

    // Hash matched, or the file could not be hashed (file already replaced,
    // access denied): keep the load allowed to stay available.
    InterlockedIncrement64(&g_JdrvGuardAllowed);
}

static NTSTATUS
JdrvGuardGrowWhitelist(
    _In_ ULONG RequiredCount
    )
{
    ULONG newCapacity;
    PJDRV_GUARD_STORE_ENTRY newEntries;

    if (RequiredCount <= g_JdrvGuardEntryCapacity) {
        return STATUS_SUCCESS;
    }
    if (RequiredCount > JDRV_GUARD_MAX_WHITELIST_ENTRIES) {
        return STATUS_QUOTA_EXCEEDED;
    }

    newCapacity = g_JdrvGuardEntryCapacity * 2UL;
    if (newCapacity < RequiredCount) {
        newCapacity = RequiredCount;
    }
    if (newCapacity > JDRV_GUARD_MAX_WHITELIST_ENTRIES) {
        newCapacity = JDRV_GUARD_MAX_WHITELIST_ENTRIES;
    }
    if (newCapacity < RequiredCount) {
        return STATUS_QUOTA_EXCEEDED;
    }

    newEntries = (PJDRV_GUARD_STORE_ENTRY)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        newCapacity * sizeof(JDRV_GUARD_STORE_ENTRY),
        JDRV_GUARD_POOL_TAG);
    if (newEntries == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (g_JdrvGuardEntryCount != 0UL) {
        RtlCopyMemory(
            newEntries,
            g_JdrvGuardEntries,
            g_JdrvGuardEntryCount * sizeof(JDRV_GUARD_STORE_ENTRY));
    }
    if (g_JdrvGuardEntries != NULL) {
        ExFreePoolWithTag(g_JdrvGuardEntries, JDRV_GUARD_POOL_TAG);
    }
    g_JdrvGuardEntries = newEntries;
    g_JdrvGuardEntryCapacity = newCapacity;
    return STATUS_SUCCESS;
}

static NTSTATUS
JdrvGuardAddEntries(
    _In_reads_(EntryCount) const JDRV_GUARD_ENTRY* Entries,
    _In_ ULONG EntryCount
    )
{
    ULONG index;
    ULONG insertIndex;
    NTSTATUS status;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvGuardLock);

    status = JdrvGuardGrowWhitelist(g_JdrvGuardEntryCount + EntryCount);
    if (!NT_SUCCESS(status)) {
        ExReleasePushLockExclusive(&g_JdrvGuardLock);
        KeLeaveCriticalRegion();
        return status;
    }

    insertIndex = g_JdrvGuardEntryCount;
    for (index = 0UL; index < EntryCount; ++index) {
        const JDRV_GUARD_ENTRY* source = &Entries[index];
        PJDRV_GUARD_STORE_ENTRY target = &g_JdrvGuardEntries[insertIndex + index];
        ULONG nameIndex;

        if (source->size < sizeof(*source) ||
            (source->flags & ~JDRV_GUARD_ENTRY_FLAG_HASH_VALID) != 0UL ||
            source->name[0] == L'\0') {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        for (nameIndex = 0UL; nameIndex < JDRV_GUARD_NAME_CHARS; ++nameIndex) {
            if (source->name[nameIndex] == L'\0') {
                break;
            }
        }
        if (nameIndex == JDRV_GUARD_NAME_CHARS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        RtlZeroMemory(target, sizeof(*target));
        JdrvGuardLowercaseName(
            target->Name,
            source->name,
            nameIndex,
            JDRV_GUARD_NAME_CHARS);
        target->FileSize = source->fileSize;
        if ((source->flags & JDRV_GUARD_ENTRY_FLAG_HASH_VALID) != 0UL) {
            target->HashValid = TRUE;
            RtlCopyMemory(target->Sha256, source->sha256, JDRV_GUARD_HASH_SIZE);
        }
    }

    if (NT_SUCCESS(status)) {
        g_JdrvGuardEntryCount += EntryCount;
    }
    ExReleasePushLockExclusive(&g_JdrvGuardLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS
JdrvDriverGuardConfigure(
    _In_reads_bytes_(InputLength) const JDRV_GUARD_CONFIG* Request,
    _In_ ULONG InputLength
    )
{
    const ULONG headerSize = FIELD_OFFSET(JDRV_GUARD_CONFIG, entries);
    ULONG requiredSize = 0UL;
    ULONG entryCount = 0UL;

    if (Request == NULL ||
        InputLength < headerSize ||
        Request->version != JDRV_PROTOCOL_VERSION ||
        Request->reserved != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Request->action != JDRV_GUARD_ACTION_CLEAR &&
        Request->action != JDRV_GUARD_ACTION_ADD &&
        Request->action != JDRV_GUARD_ACTION_ARM &&
        Request->action != JDRV_GUARD_ACTION_DISARM) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Request->action == JDRV_GUARD_ACTION_ADD) {
        entryCount = Request->entryCount;
        if (entryCount > JDRV_GUARD_MAX_ENTRIES_PER_REQUEST) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    else if (Request->entryCount != 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    requiredSize = headerSize + entryCount * sizeof(JDRV_GUARD_ENTRY);
    if (Request->size != requiredSize || InputLength < requiredSize) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Request->action == JDRV_GUARD_ACTION_ADD) {
        return JdrvGuardAddEntries(Request->entries, entryCount);
    }

    if (Request->action == JDRV_GUARD_ACTION_CLEAR) {
        KeEnterCriticalRegion();
        ExAcquirePushLockExclusive(&g_JdrvGuardLock);
        g_JdrvGuardEntryCount = 0UL;
        g_JdrvGuardArmed = FALSE;
        ExReleasePushLockExclusive(&g_JdrvGuardLock);
        KeLeaveCriticalRegion();
        return STATUS_SUCCESS;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvGuardLock);
    g_JdrvGuardArmed = (Request->action == JDRV_GUARD_ACTION_ARM);
    ExReleasePushLockExclusive(&g_JdrvGuardLock);
    KeLeaveCriticalRegion();
    return STATUS_SUCCESS;
}

NTSTATUS
JdrvDriverGuardQuery(
    _Out_writes_bytes_(OutputLength) JDRV_GUARD_STATUS* Status,
    _In_ ULONG OutputLength
    )
{
    if (Status == NULL || OutputLength < sizeof(*Status)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Status, sizeof(*Status));
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_JdrvGuardLock);
    Status->armed = g_JdrvGuardArmed ? 1UL : 0UL;
    Status->entryCount = g_JdrvGuardEntryCount;
    ExReleasePushLockShared(&g_JdrvGuardLock);
    KeLeaveCriticalRegion();
    Status->size = sizeof(*Status);
    Status->version = JDRV_PROTOCOL_VERSION;
    Status->evaluatedImages = (ULONGLONG)g_JdrvGuardEvaluated;
    Status->allowedImages = (ULONGLONG)g_JdrvGuardAllowed;
    Status->blockedImages = (ULONGLONG)g_JdrvGuardBlocked;
    Status->patchFailures = (ULONGLONG)g_JdrvGuardPatchFailures;
    return STATUS_SUCCESS;
}

NTSTATUS
JdrvDriverGuardInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    ExInitializePushLock(&g_JdrvGuardLock);
    g_JdrvGuardEntries = NULL;
    g_JdrvGuardEntryCount = 0UL;
    g_JdrvGuardEntryCapacity = 0UL;
    g_JdrvGuardArmed = FALSE;
    g_JdrvGuardSelfImageBase = DriverObject->DriverStart;
    g_JdrvGuardEvaluated = 0LL;
    g_JdrvGuardAllowed = 0LL;
    g_JdrvGuardBlocked = 0LL;
    g_JdrvGuardPatchFailures = 0LL;
    return STATUS_SUCCESS;
}

VOID
JdrvDriverGuardUninitialize(
    VOID
    )
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvGuardLock);
    g_JdrvGuardArmed = FALSE;
    g_JdrvGuardEntryCount = 0UL;
    g_JdrvGuardEntryCapacity = 0UL;
    if (g_JdrvGuardEntries != NULL) {
        ExFreePoolWithTag(g_JdrvGuardEntries, JDRV_GUARD_POOL_TAG);
        g_JdrvGuardEntries = NULL;
    }
    ExReleasePushLockExclusive(&g_JdrvGuardLock);
    KeLeaveCriticalRegion();
}
