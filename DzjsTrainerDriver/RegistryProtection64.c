#include "RegistryProtection64.h"

static LARGE_INTEGER g_JdrvRegistryCookie;
static volatile LONG g_JdrvRegistryProtectionActive;

// Randomized service name: the protected service-key suffix
// "\Services\<name>" is derived at initialization from the last component of
// the DriverEntry RegistryPath instead of a hard-coded driver name.
static WCHAR g_JdrvProtectedSuffixBuffer[300];
static UNICODE_STRING g_JdrvProtectedSuffix;

static BOOLEAN
JdrvRegistryPathIsProtected(
    _In_ PCUNICODE_STRING ObjectName
    )
{
    USHORT characterCount;
    USHORT suffixCharacterCount;
    USHORT index;

    if (ObjectName == NULL || ObjectName->Buffer == NULL ||
        g_JdrvProtectedSuffix.Length == 0 ||
        g_JdrvProtectedSuffix.Buffer == NULL) {
        return FALSE;
    }

    characterCount = ObjectName->Length / sizeof(WCHAR);
    suffixCharacterCount = g_JdrvProtectedSuffix.Length / sizeof(WCHAR);
    if (characterCount < suffixCharacterCount) {
        return FALSE;
    }

    for (index = 0; index <= characterCount - suffixCharacterCount; ++index) {
        UNICODE_STRING candidate;
        USHORT followingIndex;

        if (ObjectName->Buffer[index] != L'\\') {
            continue;
        }

        candidate.Buffer = &ObjectName->Buffer[index];
        candidate.Length = g_JdrvProtectedSuffix.Length;
        candidate.MaximumLength = g_JdrvProtectedSuffix.Length;
        if (!RtlEqualUnicodeString(&candidate, &g_JdrvProtectedSuffix, TRUE)) {
            continue;
        }

        followingIndex = index + suffixCharacterCount;
        if (followingIndex == characterCount ||
            ObjectName->Buffer[followingIndex] == L'\\') {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
JdrvRegistryObjectIsProtected(
    _In_opt_ PVOID Object
    )
{
    ULONG_PTR objectId = 0UL;
    PCUNICODE_STRING objectName = NULL;
    NTSTATUS status;

    if (Object == NULL) {
        return FALSE;
    }

    status = CmCallbackGetKeyObjectID(
        &g_JdrvRegistryCookie,
        Object,
        &objectId,
        &objectName);
    UNREFERENCED_PARAMETER(objectId);
    return NT_SUCCESS(status) && JdrvRegistryPathIsProtected(objectName);
}

static NTSTATUS
JdrvRegistryCallback(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)Argument1;
    PVOID keyObject = NULL;

    UNREFERENCED_PARAMETER(CallbackContext);

    if (InterlockedCompareExchange(
            &g_JdrvRegistryProtectionActive,
            0L,
            0L) == 0L ||
        Argument2 == NULL) {
        return STATUS_SUCCESS;
    }

    switch (notifyClass) {
    case RegNtPreDeleteKey:
        keyObject = ((PREG_DELETE_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreRenameKey:
        keyObject = ((PREG_RENAME_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreSetValueKey:
        keyObject = ((PREG_SET_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreDeleteValueKey:
        keyObject = ((PREG_DELETE_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreSetInformationKey:
        keyObject = ((PREG_SET_INFORMATION_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreSetKeySecurity:
        keyObject = ((PREG_SET_KEY_SECURITY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreRestoreKey:
        keyObject = ((PREG_RESTORE_KEY_INFORMATION)Argument2)->Object;
        break;

    case RegNtPreReplaceKey:
        keyObject = ((PREG_REPLACE_KEY_INFORMATION)Argument2)->Object;
        break;

    default:
        return STATUS_SUCCESS;
    }

    return JdrvRegistryObjectIsProtected(keyObject)
        ? STATUS_ACCESS_DENIED
        : STATUS_SUCCESS;
}

NTSTATUS
JdrvRegistryProtectionInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING altitude;
    NTSTATUS status;

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(g_JdrvProtectedSuffixBuffer, sizeof(g_JdrvProtectedSuffixBuffer));
    RtlZeroMemory(&g_JdrvProtectedSuffix, sizeof(g_JdrvProtectedSuffix));

    // RegistryPath looks like
    // \REGISTRY\MACHINE\SYSTEM\CurrentControlSet\Services\<name>;
    // take the trailing service name and build "\Services\<name>".
    if (RegistryPath != NULL && RegistryPath->Buffer != NULL &&
        RegistryPath->Length >= sizeof(WCHAR)) {
        USHORT characterCount = RegistryPath->Length / sizeof(WCHAR);
        USHORT lastSeparator = characterCount;
        USHORT index;

        for (index = 0; index < characterCount; ++index) {
            if (RegistryPath->Buffer[index] == L'\\') {
                lastSeparator = index;
            }
        }
        if (lastSeparator + 1 < characterCount) {
            USHORT nameCharacters = characterCount - lastSeparator - 1;
            USHORT prefixCharacters = 10; /* L"\\Services\\" */
            USHORT totalCharacters = prefixCharacters + nameCharacters;

            if (totalCharacters + 1 <=
                sizeof(g_JdrvProtectedSuffixBuffer) / sizeof(WCHAR)) {
                RtlCopyMemory(
                    g_JdrvProtectedSuffixBuffer,
                    L"\\Services\\",
                    prefixCharacters * sizeof(WCHAR));
                RtlCopyMemory(
                    &g_JdrvProtectedSuffixBuffer[prefixCharacters],
                    &RegistryPath->Buffer[lastSeparator + 1],
                    nameCharacters * sizeof(WCHAR));
                g_JdrvProtectedSuffixBuffer[totalCharacters] = L'\0';
                g_JdrvProtectedSuffix.Buffer = g_JdrvProtectedSuffixBuffer;
                g_JdrvProtectedSuffix.Length = totalCharacters * sizeof(WCHAR);
                g_JdrvProtectedSuffix.MaximumLength =
                    sizeof(g_JdrvProtectedSuffixBuffer);
            }
        }
    }

    if (g_JdrvProtectedSuffix.Length == 0) {
        // Own service key could not be determined: no protection target,
        // so refuse to initialize the registry callback.
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&g_JdrvRegistryCookie, sizeof(g_JdrvRegistryCookie));
    InterlockedExchange(&g_JdrvRegistryProtectionActive, 0L);
    RtlInitUnicodeString(&altitude, L"385201.5170");
    status = CmRegisterCallbackEx(
        JdrvRegistryCallback,
        &altitude,
        DriverObject,
        NULL,
        &g_JdrvRegistryCookie,
        NULL);
    if (NT_SUCCESS(status)) {
        InterlockedExchange(&g_JdrvRegistryProtectionActive, 1L);
    }
    return status;
}

VOID
JdrvRegistryProtectionUninitialize(
    VOID
    )
{
    if (InterlockedExchange(&g_JdrvRegistryProtectionActive, 0L) != 0L) {
        (VOID)CmUnRegisterCallback(g_JdrvRegistryCookie);
    }
}

BOOLEAN
JdrvRegistryProtectionIsActive(
    VOID
    )
{
    return InterlockedCompareExchange(
        &g_JdrvRegistryProtectionActive,
        0L,
        0L) != 0L;
}
