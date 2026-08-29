#pragma once

#include <ntifs.h>
#include "IoStructs.h"

NTSTATUS
JdrvValidateUnloadToken(
    _In_reads_bytes_(TokenLength) const JDRV_UNLOAD_TOKEN* Token,
    _In_ ULONG TokenLength
    );
