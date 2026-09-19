#include "UnloadToken64.h"
#include <bcrypt.h>
#include "UnlockPublicKey.h"

static NTSTATUS
JdrvSha256(
    _In_reads_bytes_(InputLength) const UCHAR* Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_(32) UCHAR Output[32]
    )
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        0UL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BCryptHash(
        algorithm,
        NULL,
        0UL,
        (PUCHAR)Input,
        InputLength,
        Output,
        32UL);
    BCryptCloseAlgorithmProvider(algorithm, 0UL);
    return status;
}

static NTSTATUS
JdrvVerifyTokenSignature(
    _In_ const JDRV_UNLOAD_TOKEN* Token
    )
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    UCHAR digest[32];
    NTSTATUS status;

    status = JdrvSha256(
        (const UCHAR*)Token,
        FIELD_OFFSET(JDRV_UNLOAD_TOKEN, signature),
        digest);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_ECDSA_P256_ALGORITHM,
        NULL,
        0UL);
    if (NT_SUCCESS(status)) {
        status = BCryptImportKeyPair(
            algorithm,
            NULL,
            BCRYPT_ECCPUBLIC_BLOB,
            &key,
            (PUCHAR)g_JdrvUnlockPublicKey,
            sizeof(g_JdrvUnlockPublicKey),
            0UL);
    }
    if (NT_SUCCESS(status)) {
        status = BCryptVerifySignature(
            key,
            NULL,
            digest,
            sizeof(digest),
            (PUCHAR)Token->signature,
            sizeof(Token->signature),
            0UL);
    }

    if (key != NULL) {
        BCryptDestroyKey(key);
    }
    if (algorithm != NULL) {
        BCryptCloseAlgorithmProvider(algorithm, 0UL);
    }
    RtlSecureZeroMemory(digest, sizeof(digest));
    return status;
}

NTSTATUS
JdrvValidateUnloadToken(
    _In_reads_bytes_(TokenLength) const JDRV_UNLOAD_TOKEN* Token,
    _In_ ULONG TokenLength
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        Token == NULL ||
        TokenLength != sizeof(*Token) ||
        Token->size != sizeof(*Token) ||
        Token->magic != JDRV_UNLOAD_TOKEN_MAGIC ||
        Token->version != JDRV_UNLOAD_TOKEN_VERSION ||
        Token->action != JDRV_UNLOAD_TOKEN_ACTION_UNLOAD) {
        return STATUS_INVALID_PARAMETER;
    }

    return JdrvVerifyTokenSignature(Token);
}
