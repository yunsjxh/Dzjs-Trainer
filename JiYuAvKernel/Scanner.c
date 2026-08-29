#include "Scanner.h"
#include "SignatureStore.h"

#define AV_FILE_CHUNK_SIZE (64UL * 1024UL)
#define AV_PROCESS_CHUNK_SIZE (256UL * 1024UL)

// Exported by supported Windows kernels but omitted from the WDK's restricted
// headers. It returns the native PEB address for the target process.
extern PVOID PsGetProcessPeb(_In_ PEPROCESS Process);

typedef struct _AV_HASH_CONTEXT {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
    PUCHAR objectBuffer;
    ULONG objectLength;
} AV_HASH_CONTEXT, *PAV_HASH_CONTEXT;

// The PEB prefix is stable for native x64 user processes. Copy it only while
// attached to the target process; the fallback is used after a 32-bit caller
// cannot inspect a 64-bit process directly.
typedef struct _AV_PEB_PREFIX {
    UCHAR inheritedAddressSpace;
    UCHAR readImageFileExecOptions;
    UCHAR beingDebugged;
    UCHAR bitField;
    PVOID mutant;
    PVOID imageBaseAddress;
} AV_PEB_PREFIX, *PAV_PEB_PREFIX;

static volatile LONG64 g_FilesScanned;
static volatile LONG64 g_BuffersScanned;
static volatile LONG64 g_Detections;
static volatile LONG64 g_BytesScanned;

static NTSTATUS
AvHashInitialize(
    _Out_ PAV_HASH_CONTEXT Context
    )
{
    ULONG resultLength = 0UL;
    ULONG hashLength = 0UL;
    NTSTATUS status;

    RtlZeroMemory(Context, sizeof(*Context));
    status = BCryptOpenAlgorithmProvider(
        &Context->algorithm,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BCryptGetProperty(
        Context->algorithm,
        BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&Context->objectLength,
        sizeof(Context->objectLength),
        &resultLength,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = BCryptGetProperty(
        Context->algorithm,
        BCRYPT_HASH_LENGTH,
        (PUCHAR)&hashLength,
        sizeof(hashLength),
        &resultLength,
        0UL);
    if (!NT_SUCCESS(status) || hashLength != JIYU_AV_SHA256_BYTES) {
        return NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status;
    }

    Context->objectBuffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        Context->objectLength,
        JIYU_AV_POOL_TAG);
    if (Context->objectBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return BCryptCreateHash(
        Context->algorithm,
        &Context->hash,
        Context->objectBuffer,
        Context->objectLength,
        NULL,
        0UL,
        0UL);
}

static VOID
AvHashUninitialize(
    _Inout_ PAV_HASH_CONTEXT Context
    )
{
    if (Context->hash != NULL) {
        (VOID)BCryptDestroyHash(Context->hash);
    }
    if (Context->objectBuffer != NULL) {
        RtlSecureZeroMemory(Context->objectBuffer, Context->objectLength);
        ExFreePoolWithTag(Context->objectBuffer, JIYU_AV_POOL_TAG);
    }
    if (Context->algorithm != NULL) {
        (VOID)BCryptCloseAlgorithmProvider(Context->algorithm, 0UL);
    }
    RtlZeroMemory(Context, sizeof(*Context));
}

static NTSTATUS
AvHashBytes(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length,
    _Out_writes_(JIYU_AV_SHA256_BYTES) UCHAR* Hash
    )
{
    AV_HASH_CONTEXT context;
    NTSTATUS status;

    status = AvHashInitialize(&context);
    if (!NT_SUCCESS(status)) {
        AvHashUninitialize(&context);
        return status;
    }
    status = BCryptHashData(context.hash, (PUCHAR)Data, Length, 0UL);
    if (NT_SUCCESS(status)) {
        status = BCryptFinishHash(
            context.hash,
            Hash,
            JIYU_AV_SHA256_BYTES,
            0UL);
    }
    AvHashUninitialize(&context);
    return status;
}

static ULONG
AvGetMaximumPatternLength(
    _In_ const JIYU_AV_SIGNATURE_SNAPSHOT* Snapshot
    )
{
    ULONG index;
    ULONG maximum = 1UL;

    for (index = 0; index < Snapshot->count; ++index) {
        if (Snapshot->records[index].type == JIYU_AV_SIGNATURE_PATTERN &&
            Snapshot->records[index].dataLength > maximum) {
            maximum = Snapshot->records[index].dataLength;
        }
    }
    return maximum;
}

static VOID
AvInitializeScanResult(
    _Out_ JIYU_AV_SCAN_RESULT* Result
    )
{
    RtlZeroMemory(Result, sizeof(*Result));
    Result->size = sizeof(*Result);
    Result->status = STATUS_UNSUCCESSFUL;
}

VOID
AvScannerInitialize(
    VOID
    )
{
    InterlockedExchange64(&g_FilesScanned, 0LL);
    InterlockedExchange64(&g_BuffersScanned, 0LL);
    InterlockedExchange64(&g_Detections, 0LL);
    InterlockedExchange64(&g_BytesScanned, 0LL);
}

NTSTATUS
AvScannerScanBuffer(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length,
    _Out_ JIYU_AV_SCAN_RESULT* Result
    )
{
    PJIYU_AV_SIGNATURE_SNAPSHOT snapshot = NULL;
    NTSTATUS status;

    if (Data == NULL || Result == NULL || Length == 0UL ||
        Length > JIYU_AV_MAX_BUFFER_SCAN) {
        return STATUS_INVALID_PARAMETER;
    }
    AvInitializeScanResult(Result);
    status = AvSignatureStoreSnapshot(&snapshot);
    if (!NT_SUCCESS(status)) {
        Result->status = status;
        return status;
    }

    status = AvHashBytes(Data, Length, Result->sha256);
    if (NT_SUCCESS(status)) {
        Result->flags |= JIYU_AV_SCAN_FLAG_HASH_VALID;
        if (!AvFindHashMatch(Result->sha256, snapshot, Result)) {
            (VOID)AvFindPatternMatch(Data, Length, 0ULL, snapshot, Result);
        }
    }
    Result->status = status;
    Result->scannedBytes = Length;
    InterlockedIncrement64(&g_BuffersScanned);
    InterlockedAdd64(&g_BytesScanned, Length);
    if ((Result->flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0UL) {
        InterlockedIncrement64(&g_Detections);
    }
    AvSignatureStoreFreeSnapshot(snapshot);
    return status;
}

NTSTATUS
AvScannerScanFile(
    _In_ const JIYU_AV_SCAN_FILE_REQUEST* Request,
    _Out_ JIYU_AV_SCAN_RESULT* Result
    )
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING path;
    IO_STATUS_BLOCK ioStatus;
    FILE_STANDARD_INFORMATION standardInformation;
    PJIYU_AV_SIGNATURE_SNAPSHOT snapshot = NULL;
    AV_HASH_CONTEXT hashContext;
    JIYU_AV_SCAN_RESULT patternResult;
    HANDLE fileHandle = NULL;
    PUCHAR buffer = NULL;
    ULONG carry = 0UL;
    ULONG maximumPatternLength;
    ULONGLONG totalBytes = 0ULL;
    NTSTATUS status;

    if (Request == NULL || Result == NULL ||
        Request->size != sizeof(*Request) ||
        Request->version != JIYU_AV_PROTOCOL_VERSION ||
        Request->pathLength == 0UL ||
        Request->pathLength >= JIYU_AV_MAX_PATH_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    AvInitializeScanResult(Result);
    AvInitializeScanResult(&patternResult);
    RtlZeroMemory(&hashContext, sizeof(hashContext));
    path.Buffer = (PWCH)Request->path;
    path.Length = (USHORT)(Request->pathLength * sizeof(WCHAR));
    path.MaximumLength = path.Length;
    InitializeObjectAttributes(
        &attributes,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    status = ZwCreateFile(
        &fileHandle,
        FILE_GENERIC_READ | SYNCHRONIZE,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0UL);
    if (!NT_SUCCESS(status)) {
        Result->status = status;
        return status;
    }

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatus,
        &standardInformation,
        sizeof(standardInformation),
        FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    if (standardInformation.Directory) {
        status = STATUS_FILE_IS_A_DIRECTORY;
        goto Exit;
    }

    status = AvSignatureStoreSnapshot(&snapshot);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    maximumPatternLength = AvGetMaximumPatternLength(snapshot);
    buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        AV_FILE_CHUNK_SIZE + JIYU_AV_MAX_PATTERN_BYTES,
        JIYU_AV_POOL_TAG);
    if (buffer == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    status = AvHashInitialize(&hashContext);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    for (;;) {
        ULONG bytesRead;
        ULONG scanLength;
        ULONGLONG baseOffset;

        status = ZwReadFile(
            fileHandle,
            NULL,
            NULL,
            NULL,
            &ioStatus,
            buffer + carry,
            AV_FILE_CHUNK_SIZE,
            NULL,
            NULL);
        if (status == STATUS_END_OF_FILE) {
            status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status)) {
            break;
        }
        bytesRead = (ULONG)ioStatus.Information;
        if (bytesRead == 0UL) {
            break;
        }

        status = BCryptHashData(hashContext.hash, buffer + carry, bytesRead, 0UL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        scanLength = carry + bytesRead;
        baseOffset = totalBytes >= carry ? totalBytes - carry : 0ULL;
        if ((patternResult.flags & JIYU_AV_SCAN_FLAG_MATCHED) == 0UL) {
#pragma warning(suppress: 6385) // ZwReadFile initialized bytesRead bytes after the carry region.
            (VOID)AvFindPatternMatch(
                buffer,
                scanLength,
                baseOffset,
                snapshot,
                &patternResult);
        }
        totalBytes += bytesRead;
        carry = min(maximumPatternLength - 1UL, scanLength);
        if (carry != 0UL) {
            RtlMoveMemory(buffer, buffer + scanLength - carry, carry);
        }
    }

    if (NT_SUCCESS(status)) {
        status = BCryptFinishHash(
            hashContext.hash,
            Result->sha256,
            JIYU_AV_SHA256_BYTES,
            0UL);
    }
    if (NT_SUCCESS(status)) {
        Result->flags |= JIYU_AV_SCAN_FLAG_HASH_VALID;
        if (!AvFindHashMatch(Result->sha256, snapshot, Result) &&
            (patternResult.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0UL) {
            Result->flags |= JIYU_AV_SCAN_FLAG_MATCHED;
            Result->signatureId = patternResult.signatureId;
            Result->signatureType = patternResult.signatureType;
            Result->matchOffset = patternResult.matchOffset;
            RtlCopyMemory(
                Result->signatureName,
                patternResult.signatureName,
                sizeof(Result->signatureName));
        }
    }

Exit:
    Result->size = sizeof(*Result);
    Result->status = status;
    Result->scannedBytes = totalBytes;
    if (fileHandle != NULL) {
        ZwClose(fileHandle);
    }
    AvHashUninitialize(&hashContext);
    if (buffer != NULL) {
        ExFreePoolWithTag(buffer, JIYU_AV_POOL_TAG);
    }
    AvSignatureStoreFreeSnapshot(snapshot);
    InterlockedIncrement64(&g_FilesScanned);
    InterlockedAdd64(&g_BytesScanned, (LONG64)totalBytes);
    if ((Result->flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0UL) {
        InterlockedIncrement64(&g_Detections);
    }
    return status;
}

static BOOLEAN
AvIsExecutableProtection(
    _In_ ULONG Protection
    )
{
    ULONG baseProtection = Protection & 0xFFUL;

    if ((Protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0UL) {
        return FALSE;
    }
    return baseProtection == PAGE_EXECUTE ||
        baseProtection == PAGE_EXECUTE_READ ||
        baseProtection == PAGE_EXECUTE_READWRITE ||
        baseProtection == PAGE_EXECUTE_WRITECOPY;
}

static ULONG
AvSampleScore(
    _In_reads_bytes_(JIYU_AV_PROCESS_SAMPLE_BYTES) const UCHAR* Data
    )
{
    BOOLEAN seen[256] = { FALSE };
    ULONG distinct = 0UL;
    ULONG longestRun = 1UL;
    ULONG currentRun = 1UL;
    ULONG index;

    if (Data == NULL) {
        return 0UL;
    }
    for (index = 0UL; index < JIYU_AV_PROCESS_SAMPLE_BYTES; ++index) {
        if (!seen[Data[index]]) {
            seen[Data[index]] = TRUE;
            ++distinct;
        }
        if (index != 0UL && Data[index] == Data[index - 1UL]) {
            ++currentRun;
            if (currentRun > longestRun) {
                longestRun = currentRun;
            }
        }
        else {
            currentRun = 1UL;
        }
    }
    if (longestRun > 8UL || distinct < 8UL) {
        return 0UL;
    }
    return distinct * 16UL - longestRun;
}

NTSTATUS
AvScannerSampleProcessImage(
    _In_ ULONG ProcessId,
    _Out_ JIYU_AV_PROCESS_SAMPLE_RESPONSE* Response
    )
{
    PEPROCESS process = NULL;
    PVOID peb;
    AV_PEB_PREFIX pebPrefix;
    ULONG_PTR address;
    ULONG_PTR maximumAddress;
    ULONG bestScore = 0UL;
    ULONGLONG bestAddress = 0ULL;
    UCHAR bestData[JIYU_AV_PROCESS_SAMPLE_BYTES] = { 0 };
    NTSTATUS status;

    if (ProcessId == 0UL || Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->processId = ProcessId;
    Response->status = STATUS_UNSUCCESSFUL;

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &process);
    if (!NT_SUCCESS(status)) {
        Response->status = status;
        return status;
    }

    peb = PsGetProcessPeb(process);
    RtlZeroMemory(&pebPrefix, sizeof(pebPrefix));
    if (peb == NULL) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }
    {
        KAPC_STATE apcState;
        NTSTATUS copyStatus = STATUS_SUCCESS;
        KeStackAttachProcess(process, &apcState);
        __try {
            RtlCopyMemory(&pebPrefix, peb, sizeof(pebPrefix));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            copyStatus = GetExceptionCode();
        }
        KeUnstackDetachProcess(&apcState);
        if (!NT_SUCCESS(copyStatus)) {
            status = copyStatus;
            goto Exit;
        }
    }
    address = (ULONG_PTR)pebPrefix.imageBaseAddress;
    maximumAddress = address + (256ULL * 1024ULL * 1024ULL);
    if (address == 0UL || maximumAddress <= address ||
        maximumAddress > (ULONG_PTR)MmHighestUserAddress) {
        status = STATUS_NOT_FOUND;
        goto Exit;
    }

    status = STATUS_NOT_FOUND;
    while (address < maximumAddress) {
        MEMORY_BASIC_INFORMATION memory = { 0 };
        SIZE_T returnLength = 0U;
        KAPC_STATE apcState;
        ULONG_PTR nextAddress;
        NTSTATUS queryStatus;
        NTSTATUS copyStatus = STATUS_SUCCESS;
        ULONG regionBestScore = 0UL;
        ULONG regionOffset = 0UL;
        UCHAR candidate[JIYU_AV_PROCESS_SAMPLE_BYTES] = { 0 };
        UCHAR regionBestData[JIYU_AV_PROCESS_SAMPLE_BYTES] = { 0 };

        KeStackAttachProcess(process, &apcState);
        queryStatus = ZwQueryVirtualMemory(
            ZwCurrentProcess(),
            (PVOID)address,
            MemoryBasicInformation,
            &memory,
            sizeof(memory),
            &returnLength);
        KeUnstackDetachProcess(&apcState);
        if (!NT_SUCCESS(queryStatus)) {
            break;
        }
        nextAddress = (ULONG_PTR)memory.BaseAddress + memory.RegionSize;
        if (nextAddress <= address) {
            break;
        }

        if (memory.State == MEM_COMMIT &&
            memory.RegionSize >= JIYU_AV_PROCESS_SAMPLE_BYTES &&
            AvIsExecutableProtection(memory.Protect)) {
            KeStackAttachProcess(process, &apcState);
            for (regionOffset = 0UL;
                (SIZE_T)regionOffset + JIYU_AV_PROCESS_SAMPLE_BYTES <= memory.RegionSize &&
                regionOffset < 64UL * 1024UL;
                regionOffset += 16UL) {
                __try {
                    RtlCopyMemory(candidate,
                        (PUCHAR)memory.BaseAddress + regionOffset,
                        JIYU_AV_PROCESS_SAMPLE_BYTES);
                    copyStatus = STATUS_SUCCESS;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    copyStatus = GetExceptionCode();
                }
                if (NT_SUCCESS(copyStatus)) {
                    ULONG score = AvSampleScore(candidate);
                    if (score > regionBestScore) {
                        regionBestScore = score;
                        RtlCopyMemory(regionBestData, candidate, sizeof(regionBestData));
                    }
                }
            }
            KeUnstackDetachProcess(&apcState);
            if (regionBestScore > bestScore) {
                bestScore = regionBestScore;
                RtlCopyMemory(bestData, regionBestData, sizeof(bestData));
                bestAddress = (ULONGLONG)(ULONG_PTR)memory.BaseAddress + regionOffset - 16ULL;
            }
        }
        address = nextAddress;
    }

    if (bestScore >= 8UL * 16UL - 8UL && bestAddress != 0ULL) {
        RtlCopyMemory(Response->data, bestData, sizeof(bestData));
        Response->address = bestAddress;
        Response->dataLength = JIYU_AV_PROCESS_SAMPLE_BYTES;
        status = STATUS_SUCCESS;
    }

Exit:
    Response->status = status;
    ObDereferenceObject(process);
    return status;
}

NTSTATUS
AvScannerScanProcess(
    _In_ ULONG ProcessId,
    _Out_ JIYU_AV_SCAN_RESULT* Result
    )
{
    PJIYU_AV_SIGNATURE_SNAPSHOT snapshot = NULL;
    PEPROCESS process = NULL;
    PUCHAR buffer = NULL;
    ULONG_PTR address = 0x10000ULL;
    ULONG_PTR maximumAddress = (ULONG_PTR)MmHighestUserAddress;
    ULONG maximumPatternLength = 1UL;
    BOOLEAN processMatched = FALSE;
    NTSTATUS status;

    if (ProcessId == 0UL || Result == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    AvInitializeScanResult(Result);
    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &process);
    if (!NT_SUCCESS(status)) {
        Result->status = status;
        return status;
    }
    status = AvSignatureStoreSnapshot(&snapshot);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    maximumPatternLength = AvGetMaximumPatternLength(snapshot);
    buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        AV_PROCESS_CHUNK_SIZE + JIYU_AV_MAX_PATTERN_BYTES,
        JIYU_AV_POOL_TAG);
    if (buffer == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = STATUS_SUCCESS;
    while (address < maximumAddress) {
        MEMORY_BASIC_INFORMATION memory = { 0 };
        SIZE_T returnLength = 0U;
        KAPC_STATE apcState;
        ULONG_PTR nextAddress;

        KeStackAttachProcess(process, &apcState);
        status = ZwQueryVirtualMemory(
            ZwCurrentProcess(),
            (PVOID)address,
            MemoryBasicInformation,
            &memory,
            sizeof(memory),
            &returnLength);
        KeUnstackDetachProcess(&apcState);
        if (!NT_SUCCESS(status)) {
            status = STATUS_SUCCESS;
            break;
        }

        nextAddress = (ULONG_PTR)memory.BaseAddress + memory.RegionSize;
        if (nextAddress <= address) {
            break;
        }
        if (memory.State == MEM_COMMIT &&
            memory.RegionSize != 0U &&
            AvIsExecutableProtection(memory.Protect)) {
            SIZE_T regionOffset = 0U;
            ULONG carry = 0UL;

            while (regionOffset < memory.RegionSize) {
                SIZE_T remaining = memory.RegionSize - regionOffset;
                ULONG bytesToCopy = remaining > AV_PROCESS_CHUNK_SIZE
                    ? AV_PROCESS_CHUNK_SIZE
                    : (ULONG)remaining;
                ULONG scanLength;
                NTSTATUS copyStatus = STATUS_SUCCESS;

                KeStackAttachProcess(process, &apcState);
                __try {
                    RtlCopyMemory(
                        buffer + carry,
                        (PUCHAR)memory.BaseAddress + regionOffset,
                        bytesToCopy);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    copyStatus = GetExceptionCode();
                }
                KeUnstackDetachProcess(&apcState);
                if (!NT_SUCCESS(copyStatus)) {
                    break;
                }

                scanLength = carry + bytesToCopy;
                if (!processMatched && AvFindPatternMatch(
                        buffer,
                        scanLength,
                        (ULONGLONG)(ULONG_PTR)memory.BaseAddress +
                            regionOffset - carry,
                        snapshot,
                        Result)) {
                    // Keep walking this PID after the first hit. The user-mode
                    // enumerator then continues with the remaining PIDs.
                    processMatched = TRUE;
                }
                Result->scannedBytes += bytesToCopy;
                InterlockedIncrement64(&g_BuffersScanned);
                InterlockedAdd64(&g_BytesScanned, bytesToCopy);
                carry = min(maximumPatternLength - 1UL, scanLength);
                if (carry != 0UL) {
                    RtlMoveMemory(buffer, buffer + scanLength - carry, carry);
                }
                regionOffset += bytesToCopy;
            }
        }
        address = nextAddress;
    }

Exit:
    if ((Result->flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0UL) {
        InterlockedIncrement64(&g_Detections);
    }
    Result->size = sizeof(*Result);
    Result->status = status;
    if (buffer != NULL) {
        ExFreePoolWithTag(buffer, JIYU_AV_POOL_TAG);
    }
    AvSignatureStoreFreeSnapshot(snapshot);
    if (process != NULL) {
        ObDereferenceObject(process);
    }
    return status;
}

VOID
AvScannerQueryStats(
    _Out_ JIYU_AV_STATS_RESPONSE* Stats
    )
{
    RtlZeroMemory(Stats, sizeof(*Stats));
    Stats->size = sizeof(*Stats);
    Stats->version = JIYU_AV_PROTOCOL_VERSION;
    Stats->signatureCount = AvSignatureStoreGetCount();
    Stats->filesScanned = (ULONGLONG)InterlockedCompareExchange64(
        &g_FilesScanned, 0LL, 0LL);
    Stats->buffersScanned = (ULONGLONG)InterlockedCompareExchange64(
        &g_BuffersScanned, 0LL, 0LL);
    Stats->detections = (ULONGLONG)InterlockedCompareExchange64(
        &g_Detections, 0LL, 0LL);
    Stats->bytesScanned = (ULONGLONG)InterlockedCompareExchange64(
        &g_BytesScanned, 0LL, 0LL);
}
