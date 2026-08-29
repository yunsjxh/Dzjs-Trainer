#pragma once

#include "JiYuAvKernel.h"

VOID AvControllerInitialize(VOID);
VOID AvControllerUninitialize(VOID);
NTSTATUS AvControllerRegisterCurrent(VOID);
BOOLEAN AvControllerIsCurrent(VOID);
VOID AvControllerProcessExit(_In_ PEPROCESS Process);
