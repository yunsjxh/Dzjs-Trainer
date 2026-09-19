#include "Driver64.h"
#include "Protection64.h"

static EX_PUSH_LOCK g_JdrvProtectionLock;
static PEPROCESS g_JdrvProtectedProcess;
static ULONG g_JdrvProtectedProcessId;
static PVOID g_JdrvProtectionRegistration;

static ACCESS_MASK
JdrvProtectionDeniedAccess(
    _In_ POBJECT_TYPE ObjectType
    )
{
    if (ObjectType == *PsProcessType) {
        // CreateProcess uses CSRSS to duplicate handles from the caller process.
        return JDRV_PROCESS_TERMINATE |
            JDRV_PROCESS_CREATE_THREAD |
            JDRV_PROCESS_VM_OPERATION |
            JDRV_PROCESS_VM_READ |
            JDRV_PROCESS_VM_WRITE |
            JDRV_PROCESS_SET_QUOTA |
            JDRV_PROCESS_SET_INFORMATION |
            JDRV_PROCESS_SUSPEND_RESUME |
            JDRV_PROCESS_SET_LIMITED_INFORMATION |
            WRITE_DAC |
            WRITE_OWNER;
    }

    if (ObjectType == *PsThreadType) {
        // CreateProcess requires system components to open this right on the caller thread.
        return JDRV_THREAD_TERMINATE |
            JDRV_THREAD_SUSPEND_RESUME |
            JDRV_THREAD_GET_CONTEXT |
            JDRV_THREAD_SET_CONTEXT |
            JDRV_THREAD_SET_INFORMATION |
            JDRV_THREAD_SET_LIMITED_INFORMATION |
            WRITE_DAC |
            WRITE_OWNER;
    }

    return 0UL;
}

static BOOLEAN
JdrvProtectionTargetsProtectedProcess(
    _In_ POB_PRE_OPERATION_INFORMATION OperationInformation
    )
{
    PEPROCESS targetProcess = NULL;
    BOOLEAN matches = FALSE;

    if (OperationInformation->ObjectType == *PsProcessType) {
        targetProcess = (PEPROCESS)OperationInformation->Object;
    }
    else if (OperationInformation->ObjectType == *PsThreadType) {
        targetProcess = PsGetThreadProcess((PETHREAD)OperationInformation->Object);
    }

    if (targetProcess == NULL) {
        return FALSE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_JdrvProtectionLock);
    matches = g_JdrvProtectedProcess != NULL && targetProcess == g_JdrvProtectedProcess;
    ExReleasePushLockShared(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();
    return matches;
}

static OB_PREOP_CALLBACK_STATUS
JdrvProtectionPreOperation(
    _In_ PVOID RegistrationContext,
    _Inout_ POB_PRE_OPERATION_INFORMATION OperationInformation
    )
{
    ACCESS_MASK* desiredAccess = NULL;
    BOOLEAN isProtectedCaller = FALSE;

    UNREFERENCED_PARAMETER(RegistrationContext);

    if (OperationInformation == NULL ||
        OperationInformation->KernelHandle ||
        OperationInformation->ObjectType == NULL ||
        (OperationInformation->ObjectType != *PsProcessType &&
         OperationInformation->ObjectType != *PsThreadType)) {
        return OB_PREOP_SUCCESS;
    }

    if (!JdrvProtectionTargetsProtectedProcess(OperationInformation)) {
        return OB_PREOP_SUCCESS;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_JdrvProtectionLock);
    isProtectedCaller = PsGetCurrentProcess() == g_JdrvProtectedProcess;
    ExReleasePushLockShared(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();
    if (isProtectedCaller) {
        return OB_PREOP_SUCCESS;
    }

    if (OperationInformation->Operation == OB_OPERATION_HANDLE_CREATE) {
        desiredAccess = &OperationInformation->Parameters->CreateHandleInformation.DesiredAccess;
    }
    else if (OperationInformation->Operation == OB_OPERATION_HANDLE_DUPLICATE) {
        desiredAccess = &OperationInformation->Parameters->DuplicateHandleInformation.DesiredAccess;
    }

    if (desiredAccess != NULL) {
        *desiredAccess &= ~JdrvProtectionDeniedAccess(OperationInformation->ObjectType);
    }
    return OB_PREOP_SUCCESS;
}

NTSTATUS
JdrvProtectionInitialize(
    VOID
    )
{
    OB_CALLBACK_REGISTRATION registration;
    OB_OPERATION_REGISTRATION operations[2];
    UNICODE_STRING altitude;

    ExInitializePushLock(&g_JdrvProtectionLock);
    g_JdrvProtectedProcess = NULL;
    g_JdrvProtectedProcessId = 0UL;
    g_JdrvProtectionRegistration = NULL;

    RtlZeroMemory(&registration, sizeof(registration));
    RtlZeroMemory(operations, sizeof(operations));
    RtlInitUnicodeString(&altitude, L"385201.5160");

    operations[0].ObjectType = PsProcessType;
    operations[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operations[0].PreOperation = JdrvProtectionPreOperation;
    operations[1].ObjectType = PsThreadType;
    operations[1].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operations[1].PreOperation = JdrvProtectionPreOperation;

    registration.Version = ObGetFilterVersion();
    registration.OperationRegistrationCount = RTL_NUMBER_OF(operations);
    registration.Altitude = altitude;
    registration.OperationRegistration = operations;
    return ObRegisterCallbacks(&registration, &g_JdrvProtectionRegistration);
}

VOID
JdrvProtectionUninitialize(
    VOID
    )
{
    PEPROCESS oldProcess = NULL;

    if (g_JdrvProtectionRegistration != NULL) {
        ObUnRegisterCallbacks(g_JdrvProtectionRegistration);
        g_JdrvProtectionRegistration = NULL;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvProtectionLock);
    oldProcess = g_JdrvProtectedProcess;
    g_JdrvProtectedProcess = NULL;
    g_JdrvProtectedProcessId = 0UL;
    ExReleasePushLockExclusive(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();

    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}

NTSTATUS
JdrvProtectionSetTarget(
    _In_ ULONG ProcessId
    )
{
    PEPROCESS newProcess = NULL;
    PEPROCESS oldProcess = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessId <= 4UL || g_JdrvProtectionRegistration == NULL) {
        return ProcessId <= 4UL ? STATUS_INVALID_PARAMETER : STATUS_DEVICE_NOT_READY;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &newProcess);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvProtectionLock);
    oldProcess = g_JdrvProtectedProcess;
    g_JdrvProtectedProcess = newProcess;
    g_JdrvProtectedProcessId = ProcessId;
    ExReleasePushLockExclusive(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();

    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
    return STATUS_SUCCESS;
}

VOID
JdrvProtectionClearIfProcess(
    _In_opt_ PEPROCESS Process
    )
{
    PEPROCESS oldProcess = NULL;

    if (Process == NULL) {
        return;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_JdrvProtectionLock);
    if (Process == g_JdrvProtectedProcess) {
        oldProcess = g_JdrvProtectedProcess;
        g_JdrvProtectedProcess = NULL;
        g_JdrvProtectedProcessId = 0UL;
    }
    ExReleasePushLockExclusive(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();

    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}

ULONG
JdrvProtectionGetTargetProcessId(
    VOID
    )
{
    ULONG processId = 0UL;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_JdrvProtectionLock);
    processId = g_JdrvProtectedProcessId;
    ExReleasePushLockShared(&g_JdrvProtectionLock);
    KeLeaveCriticalRegion();
    return processId;
}
