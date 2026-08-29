#pragma once

#include <ntifs.h>

NTSTATUS
JdrvDriverGuardInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    );

VOID
JdrvDriverGuardUninitialize(
    VOID
    );

NTSTATUS
JdrvDriverGuardConfigure(
    _In_reads_bytes_(InputLength) const JDRV_GUARD_CONFIG* Request,
    _In_ ULONG InputLength
    );

NTSTATUS
JdrvDriverGuardQuery(
    _Out_writes_bytes_(OutputLength) JDRV_GUARD_STATUS* Status,
    _In_ ULONG OutputLength
    );

// Called from the load-image notify callback. Runs at PASSIVE_LEVEL in the
// context of the thread that loads the image, before its DriverEntry runs.
// When the guard is armed and the image is a kernel-mode driver that is not
// covered by the whitelist, its DriverEntry is patched through an MDL alias
// so it returns STATUS_ACCESS_DENIED and the load fails.
VOID
JdrvDriverGuardInspectImage(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ PIMAGE_INFO ImageInfo
    );
