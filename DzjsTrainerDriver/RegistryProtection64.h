#pragma once

#include <ntifs.h>

NTSTATUS
JdrvRegistryProtectionInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath
    );

VOID
JdrvRegistryProtectionUninitialize(
    VOID
    );

BOOLEAN
JdrvRegistryProtectionIsActive(
    VOID
    );
