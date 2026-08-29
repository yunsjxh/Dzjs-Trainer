#include "SelfProtect.h"
#include "Controller.h"

#define AV_PROCESS_DENIED_ACCESS (0x0001UL | 0x0002UL | 0x0008UL | 0x0010UL | \
    0x0020UL | 0x0040UL | 0x0100UL | 0x0200UL | 0x0800UL | 0x2000UL | \
    WRITE_DAC | WRITE_OWNER)
#define AV_THREAD_DENIED_ACCESS (0x0001UL | 0x0002UL | 0x0008UL | 0x0010UL | \
    0x0020UL | 0x0200UL | 0x0400UL | WRITE_DAC | WRITE_OWNER)

static EX_PUSH_LOCK g_ProtectionLock;
static PEPROCESS g_ProtectedProcess;
static ULONG g_ProtectedProcessId;
static PVOID g_ObjectCallbackHandle;
static LARGE_INTEGER g_RegistryCookie;
static BOOLEAN g_RegistryCallbackRegistered;
static BOOLEAN g_ProcessCallbackRegistered;

/* The service name is randomized by the loader, so the protected service-key
 * suffix "\Services\<name>" is derived at initialization from the last
 * component of the DriverEntry RegistryPath instead of a hard-coded name. */
static WCHAR g_ProtectedSuffixBuffer[300];
static UNICODE_STRING g_ProtectedSuffix;

static BOOLEAN
AvIsProtectedTarget(
    _In_ POB_PRE_OPERATION_INFORMATION OperationInformation
    )
{
    PEPROCESS targetProcess = NULL;
    BOOLEAN matches;

    if (OperationInformation->ObjectType == *PsProcessType) {
        targetProcess = (PEPROCESS)OperationInformation->Object;
    }
    else if (OperationInformation->ObjectType == *PsThreadType) {
        targetProcess = PsGetThreadProcess((PETHREAD)OperationInformation->Object);
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_ProtectionLock);
    matches = targetProcess != NULL && targetProcess == g_ProtectedProcess;
    ExReleasePushLockShared(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    return matches;
}

static BOOLEAN
AvProtectedProcessIsCurrent(
    VOID
    )
{
    BOOLEAN matches;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_ProtectionLock);
    matches = g_ProtectedProcess != NULL &&
        g_ProtectedProcess == PsGetCurrentProcess();
    ExReleasePushLockShared(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    return matches;
}

static OB_PREOP_CALLBACK_STATUS
AvObjectPreOperation(
    _In_ PVOID RegistrationContext,
    _Inout_ POB_PRE_OPERATION_INFORMATION OperationInformation
    )
{
    ACCESS_MASK* desiredAccess = NULL;
    ACCESS_MASK deniedAccess;

    UNREFERENCED_PARAMETER(RegistrationContext);
    if (OperationInformation == NULL ||
        OperationInformation->KernelHandle ||
        (OperationInformation->ObjectType != *PsProcessType &&
         OperationInformation->ObjectType != *PsThreadType) ||
        !AvIsProtectedTarget(OperationInformation) ||
        AvControllerIsCurrent() ||
        AvProtectedProcessIsCurrent()) {
        return OB_PREOP_SUCCESS;
    }

    if (OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE) {
        desiredAccess = &OperationInformation->Parameters->CreateHandleInformation.DesiredAccess;
    }
    else if (OperationInformation->Operation == OB_OPERATION_HANDLE_DUPLICATE) {
        desiredAccess = &OperationInformation->Parameters->DuplicateHandleInformation.DesiredAccess;
    }

    deniedAccess = OperationInformation->ObjectType == *PsProcessType
        ? AV_PROCESS_DENIED_ACCESS
        : AV_THREAD_DENIED_ACCESS;
    if (desiredAccess != NULL) {
        *desiredAccess &= ~deniedAccess;
    }
    return OB_PREOP_SUCCESS;
}

static BOOLEAN
AvRegistryPathIsProtected(
    _In_opt_ PCUNICODE_STRING Name
    )
{
    USHORT characters;
    USHORT suffixCharacters;
    USHORT index;

    if (Name == NULL || Name->Buffer == NULL ||
        g_ProtectedSuffix.Length == 0 ||
        g_ProtectedSuffix.Buffer == NULL) {
        return FALSE;
    }
    characters = Name->Length / sizeof(WCHAR);
    suffixCharacters = g_ProtectedSuffix.Length / sizeof(WCHAR);
    if (characters < suffixCharacters) {
        return FALSE;
    }

    for (index = 0; index <= characters - suffixCharacters; ++index) {
        UNICODE_STRING candidate;
        if (Name->Buffer[index] != L'\\') {
            continue;
        }
        candidate.Buffer = &Name->Buffer[index];
        candidate.Length = g_ProtectedSuffix.Length;
        candidate.MaximumLength = g_ProtectedSuffix.Length;
        if (RtlEqualUnicodeString(&candidate, &g_ProtectedSuffix, TRUE) &&
            (index + suffixCharacters == characters ||
             Name->Buffer[index + suffixCharacters] == L'\\')) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN
AvBuildProtectedSuffixFromRegistryPath(
    _In_opt_ PCUNICODE_STRING RegistryPath
    )
{
    USHORT characters;
    USHORT lastSeparator;
    USHORT index;
    USHORT nameCharacters;
    USHORT prefixCharacters;
    USHORT totalCharacters;

    RtlZeroMemory(g_ProtectedSuffixBuffer, sizeof(g_ProtectedSuffixBuffer));
    RtlZeroMemory(&g_ProtectedSuffix, sizeof(g_ProtectedSuffix));

    if (RegistryPath == NULL || RegistryPath->Buffer == NULL ||
        RegistryPath->Length < sizeof(WCHAR)) {
        return FALSE;
    }

    /* RegistryPath looks like
     * \REGISTRY\MACHINE\SYSTEM\CurrentControlSet\Services\<name>;
     * take the trailing service name and build "\Services\<name>". */
    characters = RegistryPath->Length / sizeof(WCHAR);
    lastSeparator = characters;
    for (index = 0; index < characters; ++index) {
        if (RegistryPath->Buffer[index] == L'\\') {
            lastSeparator = index;
        }
    }
    if (lastSeparator + 1 >= characters) {
        return FALSE;
    }

    nameCharacters = characters - lastSeparator - 1;
    prefixCharacters = 10; /* L"\\Services\\" */
    totalCharacters = prefixCharacters + nameCharacters;
    if (totalCharacters + 1 >
        sizeof(g_ProtectedSuffixBuffer) / sizeof(WCHAR)) {
        return FALSE;
    }

    RtlCopyMemory(
        g_ProtectedSuffixBuffer,
        L"\\Services\\",
        prefixCharacters * sizeof(WCHAR));
    RtlCopyMemory(
        &g_ProtectedSuffixBuffer[prefixCharacters],
        &RegistryPath->Buffer[lastSeparator + 1],
        nameCharacters * sizeof(WCHAR));
    g_ProtectedSuffixBuffer[totalCharacters] = L'\0';
    g_ProtectedSuffix.Buffer = g_ProtectedSuffixBuffer;
    g_ProtectedSuffix.Length = totalCharacters * sizeof(WCHAR);
    g_ProtectedSuffix.MaximumLength = sizeof(g_ProtectedSuffixBuffer);
    return TRUE;
}

static BOOLEAN
AvRegistryObjectIsProtected(
    _In_opt_ PVOID Object
    )
{
    ULONG_PTR objectId = 0UL;
    PCUNICODE_STRING name = NULL;
    NTSTATUS status;

    if (Object == NULL) {
        return FALSE;
    }
    status = CmCallbackGetKeyObjectID(&g_RegistryCookie, Object, &objectId, &name);
    UNREFERENCED_PARAMETER(objectId);
    return NT_SUCCESS(status) && AvRegistryPathIsProtected(name);
}

static NTSTATUS
AvRegistryCallback(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)Argument1;
    PVOID keyObject = NULL;

    UNREFERENCED_PARAMETER(CallbackContext);
    if (Argument2 == NULL || AvControllerIsCurrent()) {
        return STATUS_SUCCESS;
    }

    switch (notifyClass) {
    case RegNtPreDeleteKey:
        keyObject = ((PREG_DELETE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreRenameKey:
        keyObject = ((PREG_RENAME_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreSetValueKey:
        keyObject = ((PREG_SET_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreDeleteValueKey:
        keyObject = ((PREG_DELETE_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreSetInformationKey:
        keyObject = ((PREG_SET_INFORMATION_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreSetKeySecurity:
        keyObject = ((PREG_SET_KEY_SECURITY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreRestoreKey:
        keyObject = ((PREG_RESTORE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreReplaceKey:
        keyObject = ((PREG_REPLACE_KEY_INFORMATION)Argument2)->Object;
        break;
    default:
        return STATUS_SUCCESS;
    }

    return AvRegistryObjectIsProtected(keyObject)
        ? STATUS_ACCESS_DENIED
        : STATUS_SUCCESS;
}

static VOID
AvProcessNotify(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    UNREFERENCED_PARAMETER(ProcessId);
    if (CreateInfo == NULL) {
        AvControllerProcessExit(Process);
        AvSelfProtectProcessExit(Process);
    }
}

NTSTATUS
AvSelfProtectInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath
    )
{
    OB_CALLBACK_REGISTRATION registration;
    OB_OPERATION_REGISTRATION operations[2];
    UNICODE_STRING objectAltitude;
    UNICODE_STRING registryAltitude;
    NTSTATUS status;

    ExInitializePushLock(&g_ProtectionLock);
    g_ProtectedProcess = NULL;
    g_ProtectedProcessId = 0UL;
    g_ObjectCallbackHandle = NULL;
    g_RegistryCallbackRegistered = FALSE;
    g_ProcessCallbackRegistered = FALSE;
    RtlZeroMemory(&g_RegistryCookie, sizeof(g_RegistryCookie));

    if (!AvBuildProtectedSuffixFromRegistryPath(RegistryPath)) {
        /* Own service key could not be determined: no protection target,
         * so refuse to initialize the registry callback. */
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&registration, sizeof(registration));
    RtlZeroMemory(operations, sizeof(operations));
    RtlInitUnicodeString(&objectAltitude, L"385201.6200");
    operations[0].ObjectType = PsProcessType;
    operations[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operations[0].PreOperation = AvObjectPreOperation;
    operations[1].ObjectType = PsThreadType;
    operations[1].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operations[1].PreOperation = AvObjectPreOperation;
    registration.Version = ObGetFilterVersion();
    registration.OperationRegistrationCount = RTL_NUMBER_OF(operations);
    registration.Altitude = objectAltitude;
    registration.OperationRegistration = operations;
    status = ObRegisterCallbacks(&registration, &g_ObjectCallbackHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&registryAltitude, L"385201.6210");
    status = CmRegisterCallbackEx(
        AvRegistryCallback,
        &registryAltitude,
        DriverObject,
        NULL,
        &g_RegistryCookie,
        NULL);
    if (!NT_SUCCESS(status)) {
        AvSelfProtectUninitialize();
        return status;
    }
    g_RegistryCallbackRegistered = TRUE;

    status = PsSetCreateProcessNotifyRoutineEx(AvProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        AvSelfProtectUninitialize();
        return status;
    }
    g_ProcessCallbackRegistered = TRUE;
    return STATUS_SUCCESS;
}

VOID
AvSelfProtectUninitialize(
    VOID
    )
{
    PEPROCESS oldProcess;

    if (g_ProcessCallbackRegistered) {
        (VOID)PsSetCreateProcessNotifyRoutineEx(AvProcessNotify, TRUE);
        g_ProcessCallbackRegistered = FALSE;
    }
    if (g_RegistryCallbackRegistered) {
        (VOID)CmUnRegisterCallback(g_RegistryCookie);
        g_RegistryCallbackRegistered = FALSE;
    }
    if (g_ObjectCallbackHandle != NULL) {
        ObUnRegisterCallbacks(g_ObjectCallbackHandle);
        g_ObjectCallbackHandle = NULL;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ProtectionLock);
    oldProcess = g_ProtectedProcess;
    g_ProtectedProcess = NULL;
    g_ProtectedProcessId = 0UL;
    ExReleasePushLockExclusive(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}

NTSTATUS
AvSelfProtectSetProcess(
    _In_ ULONG ProcessId
    )
{
    PEPROCESS newProcess;
    PEPROCESS oldProcess;
    NTSTATUS status;

    if (ProcessId <= 4UL || g_ObjectCallbackHandle == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &newProcess);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ProtectionLock);
    oldProcess = g_ProtectedProcess;
    g_ProtectedProcess = newProcess;
    g_ProtectedProcessId = ProcessId;
    ExReleasePushLockExclusive(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
    return STATUS_SUCCESS;
}

ULONG
AvSelfProtectGetProcessId(
    VOID
    )
{
    ULONG processId;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_ProtectionLock);
    processId = g_ProtectedProcessId;
    ExReleasePushLockShared(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    return processId;
}

VOID
AvSelfProtectProcessExit(
    _In_ PEPROCESS Process
    )
{
    PEPROCESS oldProcess = NULL;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ProtectionLock);
    if (g_ProtectedProcess == Process) {
        oldProcess = g_ProtectedProcess;
        g_ProtectedProcess = NULL;
        g_ProtectedProcessId = 0UL;
    }
    ExReleasePushLockExclusive(&g_ProtectionLock);
    KeLeaveCriticalRegion();
    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}
