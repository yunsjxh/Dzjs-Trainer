#pragma once

#include "JiYuAvKernel.h"

NTSTATUS AvSelfProtectInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath);
VOID AvSelfProtectUninitialize(VOID);
NTSTATUS AvSelfProtectSetProcess(_In_ ULONG ProcessId);
ULONG AvSelfProtectGetProcessId(VOID);
VOID AvSelfProtectProcessExit(_In_ PEPROCESS Process);
