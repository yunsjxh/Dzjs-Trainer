#include "JiYuAvKernel.h"
#include "Controller.h"
#include "Scanner.h"
#include "SelfProtect.h"
#include "SignatureStore.h"

C_ASSERT(sizeof(JIYU_AV_SIGNATURE_RECORD) == 280UL);
C_ASSERT(sizeof(JIYU_AV_SIGNATURE_QUERY_RESPONSE) == 296UL);
C_ASSERT(sizeof(JIYU_AV_SCAN_FILE_REQUEST) == 1056UL);
C_ASSERT(sizeof(JIYU_AV_SCAN_RESULT) == 200UL);
C_ASSERT(sizeof(JIYU_AV_PROCESS_SAMPLE_RESPONSE) == 88UL);

static const GUID g_DeviceClassGuid = {
    0x8bb86f51,
    0xf2f7,
    0x45dc,
    { 0xb8, 0x20, 0x56, 0x8d, 0x9b, 0xa6, 0x13, 0x91 }
};

static PDEVICE_OBJECT g_DeviceObject;
static BOOLEAN g_SymbolicLinkCreated;
static PDRIVER_OBJECT g_DriverObject;
static BOOLEAN g_SelfProtectInitialized;

static NTSTATUS
AvCompleteIrp(
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

NTSTATUS
JiYuAvDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return AvCompleteIrp(Irp, STATUS_SUCCESS, 0UL);
}

NTSTATUS
JiYuAvDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return AvCompleteIrp(Irp, STATUS_SUCCESS, 0UL);
}

NTSTATUS
JiYuAvDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PJIYU_AV_DEVICE_EXTENSION extension =
        (PJIYU_AV_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG_PTR information = 0UL;
    NTSTATUS status;

    status = IoAcquireRemoveLock(&extension->removeLock, Irp);
    if (!NT_SUCCESS(status)) {
        return AvCompleteIrp(Irp, status, 0UL);
    }

    switch (controlCode) {
    case JIYU_AV_IOCTL_QUERY_VERSION:
        if (buffer == NULL || outputLength < sizeof(JIYU_AV_VERSION_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            PJIYU_AV_VERSION_RESPONSE response =
                (PJIYU_AV_VERSION_RESPONSE)buffer;
            RtlZeroMemory(response, sizeof(*response));
            response->size = sizeof(*response);
            response->magic = JIYU_AV_PROTOCOL_MAGIC;
            response->version = JIYU_AV_PROTOCOL_VERSION;
            response->architecture = 64UL;
            response->maxSignatures = JIYU_AV_MAX_SIGNATURES;
            response->maxPatternBytes = JIYU_AV_MAX_PATTERN_BYTES;
            information = sizeof(*response);
            status = STATUS_SUCCESS;
        }
        break;

    case JIYU_AV_IOCTL_REGISTER_CONTROLLER:
        status = AvControllerRegisterCurrent();
        break;

    case JIYU_AV_IOCTL_SET_PROTECTED_PROCESS:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        if (buffer == NULL || inputLength < sizeof(JIYU_AV_PROCESS_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            const JIYU_AV_PROCESS_REQUEST* request =
                (const JIYU_AV_PROCESS_REQUEST*)buffer;
            if (request->size != sizeof(*request) ||
                request->version != JIYU_AV_PROTOCOL_VERSION) {
                status = STATUS_REVISION_MISMATCH;
            }
            else {
                status = AvSelfProtectSetProcess(request->processId);
            }
        }
        break;

    case JIYU_AV_IOCTL_ADD_SIGNATURE:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        if (buffer == NULL || inputLength < sizeof(JIYU_AV_SIGNATURE_RECORD)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = AvSignatureStoreAdd((const JIYU_AV_SIGNATURE_RECORD*)buffer);
        break;

    case JIYU_AV_IOCTL_CLEAR_SIGNATURES:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        AvSignatureStoreClear();
        status = STATUS_SUCCESS;
        break;

    case JIYU_AV_IOCTL_REMOVE_SIGNATURE:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        if (buffer == NULL || inputLength < sizeof(JIYU_AV_SIGNATURE_ID_REQUEST)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            const JIYU_AV_SIGNATURE_ID_REQUEST* request =
                (const JIYU_AV_SIGNATURE_ID_REQUEST*)buffer;
            if (request->size != sizeof(*request) ||
                request->version != JIYU_AV_PROTOCOL_VERSION) {
                status = STATUS_REVISION_MISMATCH;
            }
            else {
                status = AvSignatureStoreRemove(request->signatureId);
            }
        }
        break;

    case JIYU_AV_IOCTL_QUERY_SIGNATURE:
        if (buffer == NULL ||
            inputLength < sizeof(JIYU_AV_SIGNATURE_QUERY_REQUEST) ||
            outputLength < sizeof(JIYU_AV_SIGNATURE_QUERY_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            JIYU_AV_SIGNATURE_QUERY_REQUEST request;
            PJIYU_AV_SIGNATURE_QUERY_RESPONSE response =
                (PJIYU_AV_SIGNATURE_QUERY_RESPONSE)buffer;
            RtlCopyMemory(&request, buffer, sizeof(request));
            RtlZeroMemory(response, sizeof(*response));
            response->size = sizeof(*response);
            response->version = JIYU_AV_PROTOCOL_VERSION;
            response->index = request.index;
            if (request.size != sizeof(request) ||
                request.version != JIYU_AV_PROTOCOL_VERSION) {
                status = STATUS_REVISION_MISMATCH;
            }
            else {
                status = AvSignatureStoreQuery(
                    request.index,
                    &response->record,
                    &response->totalCount);
                if (NT_SUCCESS(status)) {
                    information = sizeof(*response);
                }
            }
        }
        break;

    case JIYU_AV_IOCTL_SCAN_BUFFER:
        if (buffer == NULL || inputLength == 0UL ||
            inputLength > JIYU_AV_MAX_BUFFER_SCAN ||
            outputLength < sizeof(JIYU_AV_SCAN_RESULT)) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        {
            PUCHAR inputCopy = (PUCHAR)ExAllocatePool2(
                POOL_FLAG_NON_PAGED,
                inputLength,
                JIYU_AV_POOL_TAG);
            if (inputCopy == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlCopyMemory(inputCopy, buffer, inputLength);
            status = AvScannerScanBuffer(
                inputCopy,
                inputLength,
                (PJIYU_AV_SCAN_RESULT)buffer);
            RtlSecureZeroMemory(inputCopy, inputLength);
            ExFreePoolWithTag(inputCopy, JIYU_AV_POOL_TAG);
            information = sizeof(JIYU_AV_SCAN_RESULT);
        }
        break;

    case JIYU_AV_IOCTL_SCAN_FILE:
        if (buffer == NULL ||
            inputLength < sizeof(JIYU_AV_SCAN_FILE_REQUEST) ||
            outputLength < sizeof(JIYU_AV_SCAN_RESULT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            JIYU_AV_SCAN_FILE_REQUEST request;
            RtlCopyMemory(&request, buffer, sizeof(request));
            status = AvScannerScanFile(
                &request,
                (PJIYU_AV_SCAN_RESULT)buffer);
            information = sizeof(JIYU_AV_SCAN_RESULT);
        }
        break;

    case JIYU_AV_IOCTL_SCAN_PROCESS:
        if (buffer == NULL ||
            inputLength < sizeof(JIYU_AV_PROCESS_REQUEST) ||
            outputLength < sizeof(JIYU_AV_SCAN_RESULT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            JIYU_AV_PROCESS_REQUEST request;
            RtlCopyMemory(&request, buffer, sizeof(request));
            if (request.size != sizeof(request) ||
                request.version != JIYU_AV_PROTOCOL_VERSION ||
                request.processId == 0UL) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = AvScannerScanProcess(
                request.processId,
                (PJIYU_AV_SCAN_RESULT)buffer);
            information = sizeof(JIYU_AV_SCAN_RESULT);
        }
        break;

    case JIYU_AV_IOCTL_SAMPLE_PROCESS_IMAGE:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        if (buffer == NULL ||
            inputLength < sizeof(JIYU_AV_PROCESS_REQUEST) ||
            outputLength < sizeof(JIYU_AV_PROCESS_SAMPLE_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            JIYU_AV_PROCESS_REQUEST request;
            RtlCopyMemory(&request, buffer, sizeof(request));
            if (request.size != sizeof(request) ||
                request.version != JIYU_AV_PROTOCOL_VERSION ||
                request.processId == 0UL) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = AvScannerSampleProcessImage(
                request.processId,
                (PJIYU_AV_PROCESS_SAMPLE_RESPONSE)buffer);
            information = sizeof(JIYU_AV_PROCESS_SAMPLE_RESPONSE);
        }
        break;

    case JIYU_AV_IOCTL_QUERY_STATS:
        if (buffer == NULL || outputLength < sizeof(JIYU_AV_STATS_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        AvScannerQueryStats((PJIYU_AV_STATS_RESPONSE)buffer);
        ((PJIYU_AV_STATS_RESPONSE)buffer)->protectedProcessId =
            AvSelfProtectGetProcessId();
        information = sizeof(JIYU_AV_STATS_RESPONSE);
        status = STATUS_SUCCESS;
        break;

    case JIYU_AV_IOCTL_ARM_UNLOAD:
        if (!AvControllerIsCurrent()) {
            status = STATUS_ACCESS_DENIED;
            break;
        }
        if (g_SelfProtectInitialized) {
            AvSelfProtectUninitialize();
            g_SelfProtectInitialized = FALSE;
        }
#pragma warning(suppress: 28175) // Unload is deliberately exposed only after authorization.
        g_DriverObject->DriverUnload = JiYuAvDriverUnload;
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    IoReleaseRemoveLock(&extension->removeLock, Irp);
    return AvCompleteIrp(Irp, status, information);
}

VOID
JiYuAvDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNICODE_STRING symbolicLink;

    if (g_SelfProtectInitialized) {
        AvSelfProtectUninitialize();
        g_SelfProtectInitialized = FALSE;
    }
    AvControllerUninitialize();
    AvSignatureStoreUninitialize();
    if (g_SymbolicLinkCreated) {
        RtlInitUnicodeString(&symbolicLink, JIYU_AV_SYMBOLIC_LINK);
        (VOID)IoDeleteSymbolicLink(&symbolicLink);
        g_SymbolicLinkCreated = FALSE;
    }
    if (g_DeviceObject != NULL) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
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
    PJIYU_AV_DEVICE_EXTENSION extension;
    NTSTATUS status;

    g_DeviceObject = NULL;
    g_SymbolicLinkCreated = FALSE;
    g_DriverObject = DriverObject;
    g_SelfProtectInitialized = FALSE;
    AvControllerInitialize();
    AvSignatureStoreInitialize();
    AvScannerInitialize();

    DriverObject->MajorFunction[IRP_MJ_CREATE] = JiYuAvDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = JiYuAvDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = JiYuAvDispatchDeviceControl;
    DriverObject->DriverUnload = NULL;

    RtlInitUnicodeString(&deviceName, JIYU_AV_DEVICE_NAME);
    RtlInitUnicodeString(
        &sddl,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = IoCreateDeviceSecure(
        DriverObject,
        sizeof(JIYU_AV_DEVICE_EXTENSION),
        &deviceName,
        JIYU_AV_DEVICE_TYPE,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        &g_DeviceClassGuid,
        &g_DeviceObject);
    if (!NT_SUCCESS(status)) {
        AvControllerUninitialize();
        AvSignatureStoreUninitialize();
        return status;
    }

    extension = (PJIYU_AV_DEVICE_EXTENSION)g_DeviceObject->DeviceExtension;
    IoInitializeRemoveLock(&extension->removeLock, JIYU_AV_POOL_TAG, 0UL, 0UL);
    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlInitUnicodeString(&symbolicLink, JIYU_AV_SYMBOLIC_LINK);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        JiYuAvDriverUnload(DriverObject);
        return status;
    }
    g_SymbolicLinkCreated = TRUE;

    status = AvSelfProtectInitialize(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status)) {
        JiYuAvDriverUnload(DriverObject);
        return status;
    }
    g_SelfProtectInitialized = TRUE;
    return STATUS_SUCCESS;
}
