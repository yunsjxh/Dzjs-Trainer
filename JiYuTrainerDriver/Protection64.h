#pragma once

#include <ntifs.h>

NTSTATUS
JdrvProtectionInitialize(
    VOID
    );

VOID
JdrvProtectionUninitialize(
    VOID
    );

NTSTATUS
JdrvProtectionSetTarget(
    _In_ ULONG ProcessId
    );

VOID
JdrvProtectionClearIfProcess(
    _In_opt_ PEPROCESS Process
    );

ULONG
JdrvProtectionGetTargetProcessId(
    VOID
    );
