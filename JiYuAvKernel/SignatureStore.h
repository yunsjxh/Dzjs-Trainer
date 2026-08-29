#pragma once

#include "JiYuAvKernel.h"

typedef struct _JIYU_AV_SIGNATURE_SNAPSHOT {
    ULONG count;
    JIYU_AV_SIGNATURE_RECORD records[JIYU_AV_MAX_SIGNATURES];
} JIYU_AV_SIGNATURE_SNAPSHOT, *PJIYU_AV_SIGNATURE_SNAPSHOT;

VOID AvSignatureStoreInitialize(VOID);
VOID AvSignatureStoreUninitialize(VOID);
NTSTATUS AvSignatureStoreAdd(_In_ const JIYU_AV_SIGNATURE_RECORD* Record);
NTSTATUS AvSignatureStoreRemove(_In_ ULONG SignatureId);
NTSTATUS AvSignatureStoreQuery(
    _In_ ULONG Index,
    _Out_ JIYU_AV_SIGNATURE_RECORD* Record,
    _Out_ ULONG* TotalCount);
VOID AvSignatureStoreClear(VOID);
ULONG AvSignatureStoreGetCount(VOID);
NTSTATUS AvSignatureStoreSnapshot(_Outptr_ PJIYU_AV_SIGNATURE_SNAPSHOT* Snapshot);
VOID AvSignatureStoreFreeSnapshot(_In_opt_ PJIYU_AV_SIGNATURE_SNAPSHOT Snapshot);

BOOLEAN AvFindPatternMatch(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ SIZE_T Length,
    _In_ ULONGLONG BaseOffset,
    _In_ const JIYU_AV_SIGNATURE_SNAPSHOT* Snapshot,
    _Inout_ JIYU_AV_SCAN_RESULT* Result);

BOOLEAN AvFindHashMatch(
    _In_reads_(JIYU_AV_SHA256_BYTES) const UCHAR* Hash,
    _In_ const JIYU_AV_SIGNATURE_SNAPSHOT* Snapshot,
    _Inout_ JIYU_AV_SCAN_RESULT* Result);
