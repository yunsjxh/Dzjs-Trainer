#include "SignatureStore.h"
#include "..\JiYuAvShared\PatternMatcher.h"

static EX_PUSH_LOCK g_SignatureLock;
static JIYU_AV_SIGNATURE_RECORD g_Signatures[JIYU_AV_MAX_SIGNATURES];
static ULONG g_SignatureCount;

static BOOLEAN
AvSignatureRecordIsValid(
    _In_ const JIYU_AV_SIGNATURE_RECORD* Record
    )
{
    ULONG index;
    ULONG significantNibbles = 0UL;

    if (Record == NULL ||
        Record->size != sizeof(*Record) ||
        Record->version != JIYU_AV_PROTOCOL_VERSION ||
        Record->signatureId == 0UL ||
        Record->name[0] == L'\0') {
        return FALSE;
    }

    if (Record->type == JIYU_AV_SIGNATURE_SHA256) {
        return Record->dataLength == JIYU_AV_SHA256_BYTES;
    }

    if (Record->type != JIYU_AV_SIGNATURE_PATTERN ||
        Record->dataLength < 4UL ||
        Record->dataLength > JIYU_AV_MAX_PATTERN_BYTES) {
        return FALSE;
    }

    for (index = 0; index < Record->dataLength; ++index) {
        switch (Record->mask[index]) {
        case 0x00U:
            break;
        case 0x0FU:
        case 0xF0U:
            ++significantNibbles;
            break;
        case 0xFFU:
            significantNibbles += 2UL;
            break;
        default:
            return FALSE;
        }
    }
    return significantNibbles >= 8UL;
}

VOID
AvSignatureStoreInitialize(
    VOID
    )
{
    ExInitializePushLock(&g_SignatureLock);
    RtlZeroMemory(g_Signatures, sizeof(g_Signatures));
    g_SignatureCount = 0UL;
}

VOID
AvSignatureStoreUninitialize(
    VOID
    )
{
    AvSignatureStoreClear();
}

NTSTATUS
AvSignatureStoreAdd(
    _In_ const JIYU_AV_SIGNATURE_RECORD* Record
    )
{
    ULONG index;
    ULONG destinationIndex;
    JIYU_AV_SIGNATURE_RECORD normalized;

    if (!AvSignatureRecordIsValid(Record)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(&normalized, Record, sizeof(normalized));
    normalized.reserved = 0UL;
    normalized.name[JIYU_AV_MAX_SIGNATURE_NAME - 1UL] = L'\0';
    if (normalized.type == JIYU_AV_SIGNATURE_PATTERN) {
        for (index = 0; index < normalized.dataLength; ++index) {
            normalized.data[index] &= normalized.mask[index];
        }
    }
    else {
        RtlZeroMemory(normalized.mask, sizeof(normalized.mask));
    }
    if (normalized.dataLength < JIYU_AV_MAX_PATTERN_BYTES) {
        RtlZeroMemory(
            normalized.data + normalized.dataLength,
            JIYU_AV_MAX_PATTERN_BYTES - normalized.dataLength);
        RtlZeroMemory(
            normalized.mask + normalized.dataLength,
            JIYU_AV_MAX_PATTERN_BYTES - normalized.dataLength);
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_SignatureLock);
    destinationIndex = g_SignatureCount;
    for (index = 0; index < g_SignatureCount; ++index) {
        if (g_Signatures[index].signatureId == Record->signatureId) {
            destinationIndex = index;
            break;
        }
    }

    if (destinationIndex == g_SignatureCount) {
        if (g_SignatureCount >= JIYU_AV_MAX_SIGNATURES) {
            ExReleasePushLockExclusive(&g_SignatureLock);
            KeLeaveCriticalRegion();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        ++g_SignatureCount;
    }

    RtlCopyMemory(&g_Signatures[destinationIndex], &normalized, sizeof(normalized));
    ExReleasePushLockExclusive(&g_SignatureLock);
    KeLeaveCriticalRegion();
    return STATUS_SUCCESS;
}

NTSTATUS
AvSignatureStoreRemove(
    _In_ ULONG SignatureId
    )
{
    ULONG index;

    if (SignatureId == 0UL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_SignatureLock);
    for (index = 0; index < g_SignatureCount; ++index) {
        if (g_Signatures[index].signatureId == SignatureId) {
            ULONG remaining = g_SignatureCount - index - 1UL;
            if (remaining != 0UL) {
                RtlMoveMemory(
                    &g_Signatures[index],
                    &g_Signatures[index + 1UL],
                    remaining * sizeof(g_Signatures[0]));
            }
            --g_SignatureCount;
            RtlSecureZeroMemory(
                &g_Signatures[g_SignatureCount],
                sizeof(g_Signatures[0]));
            ExReleasePushLockExclusive(&g_SignatureLock);
            KeLeaveCriticalRegion();
            return STATUS_SUCCESS;
        }
    }
    ExReleasePushLockExclusive(&g_SignatureLock);
    KeLeaveCriticalRegion();
    return STATUS_NOT_FOUND;
}

NTSTATUS
AvSignatureStoreQuery(
    _In_ ULONG Index,
    _Out_ JIYU_AV_SIGNATURE_RECORD* Record,
    _Out_ ULONG* TotalCount
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Record == NULL || TotalCount == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Record, sizeof(*Record));

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_SignatureLock);
    *TotalCount = g_SignatureCount;
    if (Index >= g_SignatureCount) {
        status = STATUS_NO_MORE_ENTRIES;
    }
    else {
        RtlCopyMemory(Record, &g_Signatures[Index], sizeof(*Record));
    }
    ExReleasePushLockShared(&g_SignatureLock);
    KeLeaveCriticalRegion();
    return status;
}

VOID
AvSignatureStoreClear(
    VOID
    )
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_SignatureLock);
    RtlSecureZeroMemory(g_Signatures, sizeof(g_Signatures));
    g_SignatureCount = 0UL;
    ExReleasePushLockExclusive(&g_SignatureLock);
    KeLeaveCriticalRegion();
}

ULONG
AvSignatureStoreGetCount(
    VOID
    )
{
    ULONG count;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_SignatureLock);
    count = g_SignatureCount;
    ExReleasePushLockShared(&g_SignatureLock);
    KeLeaveCriticalRegion();
    return count;
}

NTSTATUS
AvSignatureStoreSnapshot(
    _Outptr_ PJIYU_AV_SIGNATURE_SNAPSHOT* Snapshot
    )
{
    PJIYU_AV_SIGNATURE_SNAPSHOT snapshot;

    if (Snapshot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Snapshot = NULL;
    snapshot = (PJIYU_AV_SIGNATURE_SNAPSHOT)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*snapshot),
        JIYU_AV_POOL_TAG);
    if (snapshot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_SignatureLock);
    snapshot->count = g_SignatureCount;
    if (g_SignatureCount != 0UL) {
        RtlCopyMemory(
            snapshot->records,
            g_Signatures,
            g_SignatureCount * sizeof(g_Signatures[0]));
    }
    ExReleasePushLockShared(&g_SignatureLock);
    KeLeaveCriticalRegion();
    *Snapshot = snapshot;
    return STATUS_SUCCESS;
}

VOID
AvSignatureStoreFreeSnapshot(
    _In_opt_ PJIYU_AV_SIGNATURE_SNAPSHOT Snapshot
    )
{
    if (Snapshot != NULL) {
        ExFreePoolWithTag(Snapshot, JIYU_AV_POOL_TAG);
    }
}

static VOID
AvSetMatchResult(
    _In_ const JIYU_AV_SIGNATURE_RECORD* Record,
    _In_ ULONGLONG Offset,
    _Inout_ JIYU_AV_SCAN_RESULT* Result
    )
{
    Result->flags |= JIYU_AV_SCAN_FLAG_MATCHED;
    Result->signatureId = Record->signatureId;
    Result->signatureType = Record->type;
    Result->matchOffset = Offset;
    RtlCopyMemory(Result->signatureName, Record->name, sizeof(Result->signatureName));
    Result->signatureName[JIYU_AV_MAX_SIGNATURE_NAME - 1UL] = L'\0';
}

BOOLEAN
AvFindPatternMatch(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ SIZE_T Length,
    _In_ ULONGLONG BaseOffset,
    _In_ const JIYU_AV_SIGNATURE_SNAPSHOT* Snapshot,
    _Inout_ JIYU_AV_SCAN_RESULT* Result
    )
{
    ULONG signatureIndex;

    if (Data == NULL || Snapshot == NULL || Result == NULL) {
        return FALSE;
    }

    for (signatureIndex = 0; signatureIndex < Snapshot->count; ++signatureIndex) {
        const JIYU_AV_SIGNATURE_RECORD* record = &Snapshot->records[signatureIndex];
        SIZE_T offset;
        ULONG anchorIndex = 0UL;
        ULONG patternIndex;

        if (record->type != JIYU_AV_SIGNATURE_PATTERN ||
            Length < record->dataLength) {
            continue;
        }

        for (patternIndex = 0; patternIndex < record->dataLength; ++patternIndex) {
            if (record->mask[patternIndex] == 0xFFU ||
                (record->mask[anchorIndex] == 0U && record->mask[patternIndex] != 0U)) {
                anchorIndex = patternIndex;
            }
        }

        for (offset = 0; offset <= Length - record->dataLength; ++offset) {
            if ((Data[offset + anchorIndex] & record->mask[anchorIndex]) ==
                    record->data[anchorIndex] &&
                JiYuAvPatternMatches(
                    Data + offset,
                    record->data,
                    record->mask,
                    record->dataLength)) {
                AvSetMatchResult(record, BaseOffset + offset, Result);
                return TRUE;
            }
        }
    }
    return FALSE;
}

BOOLEAN
AvFindHashMatch(
    _In_reads_(JIYU_AV_SHA256_BYTES) const UCHAR* Hash,
    _In_ const JIYU_AV_SIGNATURE_SNAPSHOT* Snapshot,
    _Inout_ JIYU_AV_SCAN_RESULT* Result
    )
{
    ULONG index;

    if (Hash == NULL || Snapshot == NULL || Result == NULL) {
        return FALSE;
    }

    for (index = 0; index < Snapshot->count; ++index) {
        const JIYU_AV_SIGNATURE_RECORD* record = &Snapshot->records[index];
        if (record->type == JIYU_AV_SIGNATURE_SHA256 &&
            RtlCompareMemory(record->data, Hash, JIYU_AV_SHA256_BYTES) ==
                JIYU_AV_SHA256_BYTES) {
            AvSetMatchResult(record, 0ULL, Result);
            return TRUE;
        }
    }
    return FALSE;
}
