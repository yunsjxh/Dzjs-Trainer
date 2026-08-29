#include "Controller.h"

static EX_PUSH_LOCK g_ControllerLock;
static PEPROCESS g_ControllerProcess;

VOID
AvControllerInitialize(
    VOID
    )
{
    ExInitializePushLock(&g_ControllerLock);
    g_ControllerProcess = NULL;
}

VOID
AvControllerUninitialize(
    VOID
    )
{
    PEPROCESS oldProcess;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ControllerLock);
    oldProcess = g_ControllerProcess;
    g_ControllerProcess = NULL;
    ExReleasePushLockExclusive(&g_ControllerLock);
    KeLeaveCriticalRegion();

    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}

NTSTATUS
AvControllerRegisterCurrent(
    VOID
    )
{
    PEPROCESS currentProcess = PsGetCurrentProcess();
    NTSTATUS status = STATUS_SUCCESS;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ControllerLock);
    if (g_ControllerProcess == NULL) {
        ObReferenceObject(currentProcess);
        g_ControllerProcess = currentProcess;
    }
    else if (g_ControllerProcess != currentProcess) {
        status = STATUS_DEVICE_BUSY;
    }
    ExReleasePushLockExclusive(&g_ControllerLock);
    KeLeaveCriticalRegion();
    return status;
}

BOOLEAN
AvControllerIsCurrent(
    VOID
    )
{
    BOOLEAN matches;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_ControllerLock);
    matches = g_ControllerProcess != NULL &&
        g_ControllerProcess == PsGetCurrentProcess();
    ExReleasePushLockShared(&g_ControllerLock);
    KeLeaveCriticalRegion();
    return matches;
}

VOID
AvControllerProcessExit(
    _In_ PEPROCESS Process
    )
{
    PEPROCESS oldProcess = NULL;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ControllerLock);
    if (g_ControllerProcess == Process) {
        oldProcess = g_ControllerProcess;
        g_ControllerProcess = NULL;
    }
    ExReleasePushLockExclusive(&g_ControllerLock);
    KeLeaveCriticalRegion();

    if (oldProcess != NULL) {
        ObDereferenceObject(oldProcess);
    }
}
