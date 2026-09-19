#include "Driver64.h"
#include "Protection64.h"
#include "RegistryProtection64.h"
#include "UnloadToken64.h"
#include "DriverGuard64.h"

typedef NTSTATUS (NTAPI* JDRV_PS_PROCESS_CONTROL_ROUTINE)(
    _In_ PEPROCESS Process
    );

typedef enum _JDRV_SHUTDOWN_ACTION {
    JdrvShutdownNoReboot = 0,
    JdrvShutdownReboot = 1,
    JdrvShutdownPowerOff = 2
} JDRV_SHUTDOWN_ACTION;

typedef NTSTATUS (NTAPI* JDRV_ZW_SHUTDOWN_SYSTEM_ROUTINE)(
    _In_ JDRV_SHUTDOWN_ACTION Action
    );

typedef NTSTATUS (NTAPI* JDRV_PS_SET_LOAD_IMAGE_NOTIFY_ROUTINE_EX)(
    _In_ PLOAD_IMAGE_NOTIFY_ROUTINE NotifyRoutine,
    _In_ ULONG_PTR Flags
    );

typedef NTSTATUS (NTAPI* JDRV_PS_SET_CREATE_THREAD_NOTIFY_ROUTINE_EX)(
    _In_ PSCREATETHREADNOTIFYTYPE NotifyType,
    _In_ PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine
    );

static const GUID g_JdrvDeviceClassGuid = {
    0x2ac995e4,
    0x6797,
    0x4b26,
    { 0x8a, 0xe7, 0x5c, 0x41, 0x76, 0x50, 0xa2, 0x11 }
};

static PDEVICE_OBJECT g_JdrvDeviceObject;
static BOOLEAN g_JdrvSymbolicLinkCreated;
static BOOLEAN g_JdrvProcessNotifyRegistered;
static BOOLEAN g_JdrvImageNotifyRegistered;
static BOOLEAN g_JdrvThreadNotifyRegistered;
static PDRIVER_OBJECT g_JdrvDriverObject;
static JDRV_PS_PROCESS_CONTROL_ROUTINE g_JdrvSuspendProcess;
static JDRV_PS_PROCESS_CONTROL_ROUTINE g_JdrvResumeProcess;
static JDRV_ZW_SHUTDOWN_SYSTEM_ROUTINE g_JdrvShutdownSystem;
static JDRV_PS_SET_LOAD_IMAGE_NOTIFY_ROUTINE_EX g_JdrvSetLoadImageNotifyRoutineEx;
static JDRV_PS_SET_CREATE_THREAD_NOTIFY_ROUTINE_EX g_JdrvSetCreateThreadNotifyRoutineEx;

#define JDRV_EVENT_QUEUE_CAPACITY 128UL
#define JDRV_HEARTBEAT_TIMEOUT_100NS (15ULL * 10000000ULL)

static KSPIN_LOCK g_JdrvEventQueueLock;
static JDRV_EVENT_RECORD g_JdrvEventQueue[JDRV_EVENT_QUEUE_CAPACITY];
static ULONG g_JdrvEventQueueHead;
static ULONG g_JdrvEventQueueCount;
static ULONG g_JdrvEventQueueDropped;
static ULONGLONG g_JdrvEventSequence;
static KSPIN_LOCK g_JdrvHeartbeatLock;
static ULONG g_JdrvHeartbeatProcessId;
static ULONG g_JdrvHeartbeatProtocolVersion;
static ULONGLONG g_JdrvHeartbeatSequence;
static ULONGLONG g_JdrvHeartbeatTime;

static VOID
JdrvInitializeEventQueue(
    VOID
    )
{
    KeInitializeSpinLock(&g_JdrvEventQueueLock);
    RtlZeroMemory(g_JdrvEventQueue, sizeof(g_JdrvEventQueue));
    g_JdrvEventQueueHead = 0UL;
    g_JdrvEventQueueCount = 0UL;
    g_JdrvEventQueueDropped = 0UL;
    g_JdrvEventSequence = 0ULL;
    KeInitializeSpinLock(&g_JdrvHeartbeatLock);
    g_JdrvHeartbeatProcessId = 0UL;
    g_JdrvHeartbeatProtocolVersion = 0UL;
    g_JdrvHeartbeatSequence = 0ULL;
    g_JdrvHeartbeatTime = 0ULL;
}

static NTSTATUS
JdrvHeartbeat(
    _In_ const JDRV_HEARTBEAT_REQUEST* Request,
    _In_ ULONG InputLength,
    _Out_ JDRV_HEARTBEAT_RESPONSE* Response,
    _In_ ULONG OutputLength,
    _Out_ ULONG_PTR* Information
    )
{
    KIRQL oldIrql;
    ULONGLONG interruptTime;
    ULONGLONG currentTime;
    ULONG callerProcessId = HandleToULong(PsGetCurrentProcessId());

    if (Request == NULL || Response == NULL || Information == NULL ||
        InputLength < sizeof(*Request) || OutputLength < sizeof(*Response) ||
        Request->size < sizeof(*Request) || Request->processId != callerProcessId) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->version = JDRV_PROTOCOL_VERSION;
    Response->processId = callerProcessId;
    Response->protocolVersion = JDRV_PROTOCOL_VERSION;
    Response->sequence = Request->sequence;
    if (Request->version != JDRV_PROTOCOL_VERSION) {
        Response->accepted = 0UL;
        *Information = sizeof(*Response);
        return STATUS_REVISION_MISMATCH;
    }

    interruptTime = KeQueryInterruptTime();
    currentTime = interruptTime;
    KeAcquireSpinLock(&g_JdrvHeartbeatLock, &oldIrql);
    if (g_JdrvHeartbeatProcessId != 0UL &&
        g_JdrvHeartbeatProcessId != callerProcessId &&
        currentTime - g_JdrvHeartbeatTime < JDRV_HEARTBEAT_TIMEOUT_100NS) {
        Response->accepted = 0UL;
        Response->processId = g_JdrvHeartbeatProcessId;
        *Information = sizeof(*Response);
        KeReleaseSpinLock(&g_JdrvHeartbeatLock, oldIrql);
        return STATUS_DEVICE_BUSY;
    }
    g_JdrvHeartbeatProcessId = callerProcessId;
    g_JdrvHeartbeatProtocolVersion = Request->version;
    g_JdrvHeartbeatSequence = Request->sequence;
    g_JdrvHeartbeatTime = interruptTime;
    KeReleaseSpinLock(&g_JdrvHeartbeatLock, oldIrql);
    Response->accepted = 1UL;
    *Information = sizeof(*Response);
    return STATUS_SUCCESS;
}

static VOID
JdrvClearHeartbeatIfProcess(
    _In_ HANDLE ProcessId
    )
{
    KIRQL oldIrql;
    ULONG processId = HandleToULong(ProcessId);

    KeAcquireSpinLock(&g_JdrvHeartbeatLock, &oldIrql);
    if (g_JdrvHeartbeatProcessId == processId) {
        g_JdrvHeartbeatProcessId = 0UL;
        g_JdrvHeartbeatProtocolVersion = 0UL;
        g_JdrvHeartbeatSequence = 0ULL;
        g_JdrvHeartbeatTime = 0ULL;
    }
    KeReleaseSpinLock(&g_JdrvHeartbeatLock, oldIrql);
}

VOID
JdrvQueueEvent(
    _In_ const JDRV_EVENT_RECORD* Event
    )
{
    KIRQL oldIrql;
    ULONG tail;
    JDRV_EVENT_RECORD queuedEvent;

    if (Event == NULL) {
        return;
    }

    queuedEvent = *Event;
    KeAcquireSpinLock(&g_JdrvEventQueueLock, &oldIrql);
    queuedEvent.sequence = ++g_JdrvEventSequence;
    if (g_JdrvEventQueueCount == JDRV_EVENT_QUEUE_CAPACITY) {
        g_JdrvEventQueueHead =
            (g_JdrvEventQueueHead + 1UL) % JDRV_EVENT_QUEUE_CAPACITY;
        --g_JdrvEventQueueCount;
        ++g_JdrvEventQueueDropped;
    }
    tail = (g_JdrvEventQueueHead + g_JdrvEventQueueCount) %
        JDRV_EVENT_QUEUE_CAPACITY;
    g_JdrvEventQueue[tail] = queuedEvent;
    ++g_JdrvEventQueueCount;
    KeReleaseSpinLock(&g_JdrvEventQueueLock, oldIrql);
}

static NTSTATUS
JdrvReadEvents(
    _Out_writes_bytes_(OutputLength) JDRV_EVENT_BATCH* Batch,
    _In_ ULONG OutputLength,
    _Out_ ULONG_PTR* Information
    )
{
    const ULONG headerSize = FIELD_OFFSET(JDRV_EVENT_BATCH, events);
    ULONG maximumEvents;
    ULONG eventCount;
    ULONG index;
    KIRQL oldIrql;

    if (Batch == NULL || Information == NULL || OutputLength < headerSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    maximumEvents = (OutputLength - headerSize) / sizeof(JDRV_EVENT_RECORD);
    if (maximumEvents > JDRV_EVENT_BATCH_CAPACITY) {
        maximumEvents = JDRV_EVENT_BATCH_CAPACITY;
    }

    RtlZeroMemory(Batch, headerSize);
    Batch->version = JDRV_EVENT_VERSION;
    KeAcquireSpinLock(&g_JdrvEventQueueLock, &oldIrql);
    eventCount = g_JdrvEventQueueCount < maximumEvents
        ? g_JdrvEventQueueCount
        : maximumEvents;
    Batch->droppedEvents = g_JdrvEventQueueDropped;
    g_JdrvEventQueueDropped = 0UL;
    for (index = 0UL; index < eventCount; ++index) {
        Batch->events[index] = g_JdrvEventQueue[g_JdrvEventQueueHead];
        g_JdrvEventQueueHead =
            (g_JdrvEventQueueHead + 1UL) % JDRV_EVENT_QUEUE_CAPACITY;
    }
    g_JdrvEventQueueCount -= eventCount;
    KeReleaseSpinLock(&g_JdrvEventQueueLock, oldIrql);

    Batch->eventCount = eventCount;
    Batch->size = headerSize + eventCount * sizeof(JDRV_EVENT_RECORD);
    *Information = Batch->size;
    return STATUS_SUCCESS;
}

static NTSTATUS
JdrvAcquireRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PJDRV_DEVICE_EXTENSION extension = (PJDRV_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    return IoAcquireRemoveLock(&extension->RemoveLock, Irp);
}

static NTSTATUS
JdrvCompleteRequestWithoutLock(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
    )
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
JdrvCompleteRequest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
    )
{
    PJDRV_DEVICE_EXTENSION extension = (PJDRV_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoReleaseRemoveLock(&extension->RemoveLock, Irp);
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static PVOID
JdrvResolveRoutine(
    _In_ PCWSTR RoutineName
    )
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, RoutineName);
    return MmGetSystemRoutineAddress(&name);
}

static NTSTATUS
JdrvTerminateProcess(
    _In_ ULONG ProcessId
    )
{
    PEPROCESS process = NULL;
    HANDLE processHandle = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessId <= 4UL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &process);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (process == PsInitialSystemProcess) {
        status = STATUS_ACCESS_DENIED;
    }
    if (NT_SUCCESS(status)) {
        status = ObOpenObjectByPointer(
            process,
            OBJ_KERNEL_HANDLE,
            NULL,
            JDRV_PROCESS_TERMINATE,
            *PsProcessType,
            KernelMode,
            &processHandle);
    }
    if (NT_SUCCESS(status)) {
        status = ZwTerminateProcess(processHandle, STATUS_SUCCESS);
    }

    if (processHandle != NULL) {
        ZwClose(processHandle);
    }
    ObDereferenceObject(process);
    return status;
}

static NTSTATUS
JdrvSetProcessSuspended(
    _In_ ULONG ProcessId,
    _In_ BOOLEAN Suspend
    )
{
    PEPROCESS process = NULL;
    JDRV_PS_PROCESS_CONTROL_ROUTINE routine = Suspend
        ? g_JdrvSuspendProcess
        : g_JdrvResumeProcess;
    NTSTATUS status = STATUS_SUCCESS;

    if (ProcessId <= 4UL || routine == NULL) {
        return ProcessId <= 4UL ? STATUS_INVALID_PARAMETER : STATUS_NOT_SUPPORTED;
    }

    status = PsLookupProcessByProcessId(ULongToHandle(ProcessId), &process);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (process == PsInitialSystemProcess) {
        status = STATUS_ACCESS_DENIED;
    }
    else {
        status = routine(process);
    }
    ObDereferenceObject(process);
    return status;
}

static BOOLEAN
JdrvValidateProcessRequest(
    _In_reads_bytes_(InputLength) const JDRV_PROCESS_REQUEST* Request,
    _In_ ULONG InputLength
    )
{
    return Request != NULL &&
        InputLength >= sizeof(*Request) &&
        Request->size >= sizeof(*Request) &&
        Request->version == JDRV_PROTOCOL_VERSION &&
        Request->reserved == 0UL &&
        Request->processId > 4UL;
}

static NTSTATUS
JdrvWriteOperationResponse(
    _Out_writes_bytes_(OutputLength) JDRV_OPERATION_RESPONSE* Response,
    _In_ ULONG OutputLength,
    _In_ NTSTATUS OperationStatus,
    _Out_ ULONG_PTR* Information
    )
{
    if (Response == NULL || OutputLength < sizeof(*Response) || Information == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->status = OperationStatus;
    *Information = sizeof(*Response);
    return STATUS_SUCCESS;
}

static VOID
JdrvProcessNotify(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _In_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    UNREFERENCED_PARAMETER(ProcessId);

    if (CreateInfo == NULL) {
        JdrvProtectionClearIfProcess(Process);
        JdrvClearHeartbeatIfProcess(ProcessId);
    }
}

static VOID
JdrvImageLoadNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo
    )
{
    JDRV_EVENT_RECORD eventRecord;
    LARGE_INTEGER systemTime;
    ULONG processId;
    ULONG targetProcessId;
    ULONG pathLength;

    if (ImageInfo == NULL) {
        return;
    }

    processId = HandleToULong(ProcessId);
    targetProcessId = JdrvProtectionGetTargetProcessId();
    if (!ImageInfo->SystemModeImage &&
        (targetProcessId == 0UL || targetProcessId != processId)) {
        return;
    }

    // Kernel-mode image loads run through the driver guard before the
    // image's DriverEntry is invoked by the loader.
    if (ImageInfo->SystemModeImage) {
        JdrvDriverGuardInspectImage(FullImageName, ImageInfo);
    }

    RtlZeroMemory(&eventRecord, sizeof(eventRecord));
    KeQuerySystemTime(&systemTime);
    eventRecord.size = sizeof(eventRecord);
    eventRecord.version = JDRV_EVENT_VERSION;
    eventRecord.type = JDRV_EVENT_TYPE_IMAGE_LOAD;
    eventRecord.timestamp = (ULONGLONG)systemTime.QuadPart;
    eventRecord.processId = processId;
    eventRecord.imageBase = (ULONGLONG)(ULONG_PTR)ImageInfo->ImageBase;
    eventRecord.imageSize = (ULONGLONG)ImageInfo->ImageSize;
    if (ImageInfo->SystemModeImage) {
        eventRecord.flags |= JDRV_EVENT_FLAG_SYSTEM_IMAGE;
    }
    if (targetProcessId != 0UL && targetProcessId == processId) {
        eventRecord.flags |= JDRV_EVENT_FLAG_PROTECTED_TARGET;
    }
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

static VOID
JdrvThreadNotify(
    _In_ HANDLE ProcessId,
    _In_ HANDLE ThreadId,
    _In_ BOOLEAN Create
    )
{
    JDRV_EVENT_RECORD eventRecord;
    LARGE_INTEGER systemTime;
    ULONG targetProcessId = JdrvProtectionGetTargetProcessId();
    ULONG processId = HandleToULong(ProcessId);
    ULONG threadId = HandleToULong(ThreadId);

    if (targetProcessId != 0UL && targetProcessId == processId) {
        RtlZeroMemory(&eventRecord, sizeof(eventRecord));
        KeQuerySystemTime(&systemTime);
        eventRecord.size = sizeof(eventRecord);
        eventRecord.version = JDRV_EVENT_VERSION;
        eventRecord.type = Create
            ? JDRV_EVENT_TYPE_THREAD_CREATE
            : JDRV_EVENT_TYPE_THREAD_EXIT;
        eventRecord.flags = JDRV_EVENT_FLAG_PROTECTED_TARGET;
        eventRecord.timestamp = (ULONGLONG)systemTime.QuadPart;
        eventRecord.processId = processId;
        eventRecord.threadId = threadId;
        JdrvQueueEvent(&eventRecord);
    }
}

NTSTATUS
JdrvDispatchUnsupported(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    NTSTATUS status = JdrvAcquireRequest(DeviceObject, Irp);
    if (!NT_SUCCESS(status)) {
        return JdrvCompleteRequestWithoutLock(Irp, status, 0UL);
    }
    return JdrvCompleteRequest(DeviceObject, Irp, STATUS_INVALID_DEVICE_REQUEST, 0UL);
}

NTSTATUS
JdrvDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    NTSTATUS status = JdrvAcquireRequest(DeviceObject, Irp);
    if (!NT_SUCCESS(status)) {
        return JdrvCompleteRequestWithoutLock(Irp, status, 0UL);
    }
    return JdrvCompleteRequest(DeviceObject, Irp, STATUS_SUCCESS, 0UL);
}

NTSTATUS
JdrvDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG_PTR information = 0UL;
    NTSTATUS operationStatus = STATUS_SUCCESS;
    NTSTATUS status = STATUS_SUCCESS;
    ACCESS_MASK requiredAccess =
        (code == CTL_QUERY_VERSION || code == CTL_READ_EVENTS ||
         code == CTL_DRIVER_GUARD_QUERY)
        ? FILE_READ_ACCESS
        : FILE_WRITE_ACCESS;

    status = JdrvAcquireRequest(DeviceObject, Irp);
    if (!NT_SUCCESS(status)) {
        return JdrvCompleteRequestWithoutLock(Irp, status, 0UL);
    }

    status = IoValidateDeviceIoControlAccess(Irp, requiredAccess);
    if (!NT_SUCCESS(status)) {
        return JdrvCompleteRequest(DeviceObject, Irp, status, 0UL);
    }

    switch (code) {
    case CTL_QUERY_VERSION:
        if (buffer == NULL || outputLength < sizeof(JDRV_VERSION_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            JDRV_VERSION_RESPONSE* response = (JDRV_VERSION_RESPONSE*)buffer;
            RtlZeroMemory(response, sizeof(*response));
            response->size = sizeof(*response);
            response->magic = JDRV_PROTOCOL_MAGIC;
            response->version = JDRV_PROTOCOL_VERSION;
            response->architecture = JDRV_ARCHITECTURE_X64;
            response->capabilities = JDRV_CAPABILITY_TERMINATE_PROCESS |
                JDRV_CAPABILITY_SELF_PROTECT |
                JDRV_CAPABILITY_TOKEN_AUTHORIZED_UNLOAD |
                JDRV_CAPABILITY_EVENT_STREAM |
                JDRV_CAPABILITY_HEARTBEAT |
                JDRV_CAPABILITY_DRIVER_GUARD;
            if (g_JdrvImageNotifyRegistered) {
                response->capabilities |= JDRV_CAPABILITY_IMAGE_LOAD_NOTIFY;
            }
            if (g_JdrvThreadNotifyRegistered) {
                response->capabilities |= JDRV_CAPABILITY_THREAD_NOTIFY;
            }
            if (JdrvRegistryProtectionIsActive()) {
                response->capabilities |= JDRV_CAPABILITY_REGISTRY_PROTECTION;
            }
            if (g_JdrvSuspendProcess != NULL && g_JdrvResumeProcess != NULL) {
                response->capabilities |= JDRV_CAPABILITY_SUSPEND_PROCESS;
            }
            if (g_JdrvShutdownSystem != NULL) {
                response->capabilities |= JDRV_CAPABILITY_POWER_CONTROL;
            }
            information = sizeof(*response);
        }
        break;

    case CTL_READ_EVENTS:
        status = JdrvReadEvents(
            (JDRV_EVENT_BATCH*)buffer,
            outputLength,
            &information);
        break;

    case CTL_HEARTBEAT:
        status = JdrvHeartbeat(
            (const JDRV_HEARTBEAT_REQUEST*)buffer,
            inputLength,
            (JDRV_HEARTBEAT_RESPONSE*)buffer,
            outputLength,
            &information);
        break;

    case CTL_DRIVER_GUARD_CONFIG:
        {
            const JDRV_GUARD_CONFIG* request = (const JDRV_GUARD_CONFIG*)buffer;
            operationStatus = JdrvDriverGuardConfigure(request, inputLength);
            status = JdrvWriteOperationResponse(
                (JDRV_OPERATION_RESPONSE*)buffer,
                outputLength,
                operationStatus,
                &information);
        }
        break;

    case CTL_DRIVER_GUARD_QUERY:
        status = JdrvDriverGuardQuery(
            (JDRV_GUARD_STATUS*)buffer,
            outputLength);
        if (NT_SUCCESS(status)) {
            information = sizeof(JDRV_GUARD_STATUS);
        }
        break;

    case CTL_INITPARAM:
        if (buffer == NULL || inputLength < sizeof(JDRV_INITPARAM)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            const JDRV_INITPARAM* request = (const JDRV_INITPARAM*)buffer;
            if (request->size < sizeof(*request) || request->version != JDRV_PROTOCOL_VERSION) {
                status = STATUS_INVALID_PARAMETER;
            }
        }
        break;

    case CTL_INITSELFPROTECT:
    case CTL_KILL_PROCESS:
    case CTL_SUSPEND_PROCESS:
    case CTL_RESUME_PROCESS:
        if (!JdrvValidateProcessRequest(
                (const JDRV_PROCESS_REQUEST*)buffer,
                inputLength)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        {
            const JDRV_PROCESS_REQUEST request = *(const JDRV_PROCESS_REQUEST*)buffer;
            if (code == CTL_INITSELFPROTECT) {
                operationStatus = JdrvProtectionSetTarget(request.processId);
            }
            else if (code == CTL_KILL_PROCESS) {
                operationStatus = JdrvTerminateProcess(request.processId);
            }
            else {
                operationStatus = JdrvSetProcessSuspended(
                    request.processId,
                    code == CTL_SUSPEND_PROCESS);
            }
            status = JdrvWriteOperationResponse(
                (JDRV_OPERATION_RESPONSE*)buffer,
                outputLength,
                operationStatus,
                &information);
        }
        break;

    case CTL_SHUTDOWN:
    case CTL_REBOOT:
        if (g_JdrvShutdownSystem == NULL) {
            status = STATUS_NOT_SUPPORTED;
        }
        else {
            status = g_JdrvShutdownSystem(
                code == CTL_REBOOT ? JdrvShutdownReboot : JdrvShutdownPowerOff);
        }
        break;

    case CTL_CLIENT_QUIT:
        JdrvProtectionClearIfProcess(PsGetCurrentProcess());
        break;

    case CTL_UNINIT:
        status = JdrvValidateUnloadToken(
            (const JDRV_UNLOAD_TOKEN*)buffer,
            inputLength);
        if (NT_SUCCESS(status)) {
            JdrvRegistryProtectionUninitialize();
            g_JdrvDriverObject->DriverUnload = JdrvDriverUnload;
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return JdrvCompleteRequest(DeviceObject, Irp, status, information);
}

VOID
JdrvDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNICODE_STRING symbolicLink;

    if (g_JdrvDeviceObject != NULL) {
        PJDRV_DEVICE_EXTENSION extension =
            (PJDRV_DEVICE_EXTENSION)g_JdrvDeviceObject->DeviceExtension;
        IoReleaseRemoveLockAndWait(&extension->RemoveLock, NULL);
    }

    DriverObject->DriverUnload = NULL;
    g_JdrvDriverObject = NULL;

    JdrvRegistryProtectionUninitialize();
    if (g_JdrvProcessNotifyRegistered) {
        (VOID)PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, TRUE);
        g_JdrvProcessNotifyRegistered = FALSE;
    }
    if (g_JdrvImageNotifyRegistered) {
        (VOID)PsRemoveLoadImageNotifyRoutine(JdrvImageLoadNotify);
        g_JdrvImageNotifyRegistered = FALSE;
    }
    if (g_JdrvThreadNotifyRegistered) {
        (VOID)PsRemoveCreateThreadNotifyRoutine(JdrvThreadNotify);
        g_JdrvThreadNotifyRegistered = FALSE;
    }
    JdrvDriverGuardUninitialize();
    JdrvProtectionUninitialize();

    if (g_JdrvSymbolicLinkCreated) {
        RtlInitUnicodeString(&symbolicLink, JDRV_SYMBOLIC_LINK_NAME);
        IoDeleteSymbolicLink(&symbolicLink);
        g_JdrvSymbolicLinkCreated = FALSE;
    }
    if (g_JdrvDeviceObject != NULL) {
        IoDeleteDevice(g_JdrvDeviceObject);
        g_JdrvDeviceObject = NULL;
    }

    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLink;
    UNICODE_STRING sddl;
    ULONG index = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    g_JdrvDeviceObject = NULL;
    g_JdrvSymbolicLinkCreated = FALSE;
    g_JdrvProcessNotifyRegistered = FALSE;
    g_JdrvImageNotifyRegistered = FALSE;
    g_JdrvThreadNotifyRegistered = FALSE;
    g_JdrvDriverObject = DriverObject;
    g_JdrvSuspendProcess = (JDRV_PS_PROCESS_CONTROL_ROUTINE)JdrvResolveRoutine(L"PsSuspendProcess");
    g_JdrvResumeProcess = (JDRV_PS_PROCESS_CONTROL_ROUTINE)JdrvResolveRoutine(L"PsResumeProcess");
    g_JdrvShutdownSystem = (JDRV_ZW_SHUTDOWN_SYSTEM_ROUTINE)JdrvResolveRoutine(L"ZwShutdownSystem");
    if (g_JdrvShutdownSystem == NULL) {
        g_JdrvShutdownSystem = (JDRV_ZW_SHUTDOWN_SYSTEM_ROUTINE)JdrvResolveRoutine(L"NtShutdownSystem");
    }
    g_JdrvSetLoadImageNotifyRoutineEx =
        (JDRV_PS_SET_LOAD_IMAGE_NOTIFY_ROUTINE_EX)JdrvResolveRoutine(
            L"PsSetLoadImageNotifyRoutineEx");
    g_JdrvSetCreateThreadNotifyRoutineEx =
        (JDRV_PS_SET_CREATE_THREAD_NOTIFY_ROUTINE_EX)JdrvResolveRoutine(
            L"PsSetCreateThreadNotifyRoutineEx");
    JdrvInitializeEventQueue();

    for (index = 0UL; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index) {
        DriverObject->MajorFunction[index] = JdrvDispatchUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = JdrvDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = JdrvDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = JdrvDispatchDeviceControl;
    DriverObject->DriverUnload = NULL;

    status = JdrvProtectionInitialize();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = JdrvDriverGuardInitialize(DriverObject);
    if (!NT_SUCCESS(status)) {
        JdrvProtectionUninitialize();
        return status;
    }

    status = PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        JdrvDriverGuardUninitialize();
        JdrvProtectionUninitialize();
        return status;
    }
    g_JdrvProcessNotifyRegistered = TRUE;

    if (g_JdrvSetLoadImageNotifyRoutineEx != NULL) {
        status = g_JdrvSetLoadImageNotifyRoutineEx(
            JdrvImageLoadNotify,
            PS_IMAGE_NOTIFY_CONFLICTING_ARCHITECTURE);
    }
    else {
        status = PsSetLoadImageNotifyRoutine(JdrvImageLoadNotify);
    }
    if (!NT_SUCCESS(status)) {
        (VOID)PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, TRUE);
        g_JdrvProcessNotifyRegistered = FALSE;
        JdrvDriverGuardUninitialize();
        JdrvProtectionUninitialize();
        return status;
    }
    g_JdrvImageNotifyRegistered = TRUE;

    if (g_JdrvSetCreateThreadNotifyRoutineEx != NULL) {
        status = g_JdrvSetCreateThreadNotifyRoutineEx(
            PsCreateThreadNotifyNonSystem,
            JdrvThreadNotify);
    }
    else {
        status = PsSetCreateThreadNotifyRoutine(JdrvThreadNotify);
    }
    if (!NT_SUCCESS(status)) {
        (VOID)PsRemoveLoadImageNotifyRoutine(JdrvImageLoadNotify);
        g_JdrvImageNotifyRegistered = FALSE;
        (VOID)PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, TRUE);
        g_JdrvProcessNotifyRegistered = FALSE;
        JdrvDriverGuardUninitialize();
        JdrvProtectionUninitialize();
        return status;
    }
    g_JdrvThreadNotifyRegistered = TRUE;

    status = JdrvRegistryProtectionInitialize(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status)) {
        (VOID)PsRemoveCreateThreadNotifyRoutine(JdrvThreadNotify);
        g_JdrvThreadNotifyRegistered = FALSE;
        (VOID)PsRemoveLoadImageNotifyRoutine(JdrvImageLoadNotify);
        g_JdrvImageNotifyRegistered = FALSE;
        (VOID)PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, TRUE);
        g_JdrvProcessNotifyRegistered = FALSE;
        JdrvDriverGuardUninitialize();
        JdrvProtectionUninitialize();
        return status;
    }

    RtlInitUnicodeString(&deviceName, JDRV_DEVICE_NAME);
    sddl = SDDL_DEVOBJ_SYS_ALL_ADM_ALL;
    status = IoCreateDeviceSecure(
        DriverObject,
        sizeof(JDRV_DEVICE_EXTENSION),
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        &g_JdrvDeviceClassGuid,
        &g_JdrvDeviceObject);
    if (!NT_SUCCESS(status)) {
        JdrvRegistryProtectionUninitialize();
        (VOID)PsRemoveCreateThreadNotifyRoutine(JdrvThreadNotify);
        g_JdrvThreadNotifyRegistered = FALSE;
        (VOID)PsRemoveLoadImageNotifyRoutine(JdrvImageLoadNotify);
        g_JdrvImageNotifyRegistered = FALSE;
        (VOID)PsSetCreateProcessNotifyRoutineEx(JdrvProcessNotify, TRUE);
        g_JdrvProcessNotifyRegistered = FALSE;
        JdrvDriverGuardUninitialize();
        JdrvProtectionUninitialize();
        return status;
    }

    IoInitializeRemoveLock(
        &((PJDRV_DEVICE_EXTENSION)g_JdrvDeviceObject->DeviceExtension)->RemoveLock,
        'vdrJ',
        0UL,
        0UL);
    g_JdrvDeviceObject->Flags |= DO_BUFFERED_IO;
    RtlInitUnicodeString(&symbolicLink, JDRV_SYMBOLIC_LINK_NAME);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        JdrvDriverUnload(DriverObject);
        return status;
    }
    g_JdrvSymbolicLinkCreated = TRUE;
    g_JdrvDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
