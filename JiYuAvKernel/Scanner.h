#pragma once

#include "JiYuAvKernel.h"

VOID AvScannerInitialize(VOID);
NTSTATUS AvScannerScanFile(
    _In_ const JIYU_AV_SCAN_FILE_REQUEST* Request,
    _Out_ JIYU_AV_SCAN_RESULT* Result);
NTSTATUS AvScannerScanBuffer(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length,
    _Out_ JIYU_AV_SCAN_RESULT* Result);
NTSTATUS AvScannerScanProcess(
    _In_ ULONG ProcessId,
    _Out_ JIYU_AV_SCAN_RESULT* Result);
NTSTATUS AvScannerSampleProcessImage(
    _In_ ULONG ProcessId,
    _Out_ JIYU_AV_PROCESS_SAMPLE_RESPONSE* Response);
VOID AvScannerQueryStats(_Out_ JIYU_AV_STATS_RESPONSE* Stats);
