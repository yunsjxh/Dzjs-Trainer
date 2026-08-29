#pragma once

#include <ntifs.h>
#include <wdmsec.h>
#include <bcrypt.h>

#include "..\JiYuAvShared\AvProtocol.h"

#define JIYU_AV_DEVICE_NAME L"\\Device\\JiYuAvKernel"
#define JIYU_AV_SYMBOLIC_LINK L"\\DosDevices\\JiYuAv"
#define JIYU_AV_POOL_TAG 'vAyJ'

typedef struct _JIYU_AV_DEVICE_EXTENSION {
    IO_REMOVE_LOCK removeLock;
} JIYU_AV_DEVICE_EXTENSION, *PJIYU_AV_DEVICE_EXTENSION;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD JiYuAvDriverUnload;

_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH JiYuAvDispatchCreate;

_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH JiYuAvDispatchClose;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH JiYuAvDispatchDeviceControl;
