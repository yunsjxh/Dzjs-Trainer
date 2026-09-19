#include "stdafx.h"
#include "DriverSignaturePolicy.h"
#include "DriverPublisherAllowlist.h"
#include "DriverSignerPin.h"

#include <Softpub.h>
#include <mscat.h>
#include <wintrust.h>

#include <bcrypt.h>
#include <string>
#include <vector>

namespace {

bool GetVerifiedPublisher(HANDLE stateData, std::wstring* publisher, std::wstring* thumbprint)
{
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(stateData);
    if (provider == nullptr) return false;
    CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    if (signer == nullptr || signer->csCertChain == 0UL) return false;
    CRYPT_PROVIDER_CERT* certificate = WTHelperGetProvCertFromChain(signer, 0);
    if (certificate == nullptr || certificate->pCert == nullptr) return false;

    DWORD required = CertGetNameStringW(
        certificate->pCert,
        CERT_NAME_SIMPLE_DISPLAY_TYPE,
        0UL,
        nullptr,
        nullptr,
        0UL);
    if (required <= 1UL) return false;
    std::vector<wchar_t> name(required);
    if (CertGetNameStringW(
            certificate->pCert,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0UL,
            nullptr,
            name.data(),
            required) != required) {
        return false;
    }
    publisher->assign(name.data());
    if (thumbprint != nullptr) {
        BYTE digest[64] = {};
        DWORD digestBytes = sizeof(digest);
        if (CertGetCertificateContextProperty(certificate->pCert, CERT_SHA1_HASH_PROP_ID,
                digest, &digestBytes) && digestBytes != 0UL) {
            static const wchar_t hex[] = L"0123456789ABCDEF";
            thumbprint->clear();
            for (DWORD index = 0; index < digestBytes; ++index) {
                thumbprint->push_back(hex[digest[index] >> 4]);
                thumbprint->push_back(hex[digest[index] & 0x0F]);
            }
        }
    }
    return !publisher->empty();
}

LONG VerifyTrustData(WINTRUST_DATA* trustData, std::wstring* publisher, std::wstring* thumbprint)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    trustData->dwStateAction = WTD_STATEACTION_VERIFY;
    LONG status = WinVerifyTrust(nullptr, &action, trustData);
    bool publisherAvailable = GetVerifiedPublisher(trustData->hWVTStateData, publisher, thumbprint);
    if (status == ERROR_SUCCESS && !publisherAvailable) {
        status = TRUST_E_SUBJECT_NOT_TRUSTED;
    }
    trustData->dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &action, trustData);
    return status;
}

LONG VerifyEmbeddedSignature(LPCWSTR driverPath, std::wstring* publisher, std::wstring* thumbprint)
{
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = driverPath;

    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;
    return VerifyTrustData(&trustData, publisher, thumbprint);
}

LONG VerifyCatalogSignature(LPCWSTR driverPath, std::wstring* publisher, std::wstring* thumbprint)
{
    HCATADMIN catalogAdmin = nullptr;
    HCATINFO catalogContext = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    LONG status = TRUST_E_NOSIGNATURE;
    DWORD hashLength = 0UL;
    BOOL hashSized = FALSE;

    typedef BOOL(WINAPI* AcquireContext2Fn)(
        HCATADMIN*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD);
    typedef BOOL(WINAPI* CalcHash2Fn)(HCATADMIN, HANDLE, DWORD*, BYTE*, DWORD);
    HMODULE wintrust = GetModuleHandleW(L"wintrust.dll");
    AcquireContext2Fn acquireContext2 = wintrust == nullptr ? nullptr :
        reinterpret_cast<AcquireContext2Fn>(GetProcAddress(wintrust, "CryptCATAdminAcquireContext2"));
    CalcHash2Fn calcHash2 = wintrust == nullptr ? nullptr :
        reinterpret_cast<CalcHash2Fn>(GetProcAddress(wintrust, "CryptCATAdminCalcHashFromFileHandle2"));
    bool useSha256 = acquireContext2 != nullptr && calcHash2 != nullptr &&
        acquireContext2(&catalogAdmin, nullptr, L"SHA256", nullptr, 0UL) != FALSE;
    if (!useSha256 && !CryptCATAdminAcquireContext(&catalogAdmin, nullptr, 0UL)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    file = CreateFileW(
        driverPath,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto Exit;
    }

    hashSized = useSha256
        ? calcHash2(catalogAdmin, file, &hashLength, nullptr, 0UL)
        : CryptCATAdminCalcHashFromFileHandle(file, &hashLength, nullptr, 0UL);
    if (!hashSized || hashLength == 0UL) {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto Exit;
    }
    {
        std::vector<BYTE> hash(hashLength);
        BOOL hashCalculated = useSha256
            ? calcHash2(catalogAdmin, file, &hashLength, hash.data(), 0UL)
            : CryptCATAdminCalcHashFromFileHandle(file, &hashLength, hash.data(), 0UL);
        if (!hashCalculated) {
            status = HRESULT_FROM_WIN32(GetLastError());
            goto Exit;
        }
        catalogContext = CryptCATAdminEnumCatalogFromHash(
            catalogAdmin,
            hash.data(),
            hashLength,
            0UL,
            nullptr);
        if (catalogContext == nullptr) goto Exit;

        CATALOG_INFO catalog = {};
        catalog.cbStruct = sizeof(catalog);
        if (!CryptCATCatalogInfoFromContext(catalogContext, &catalog, 0UL)) {
            status = HRESULT_FROM_WIN32(GetLastError());
            goto Exit;
        }

        static const wchar_t hex[] = L"0123456789ABCDEF";
        std::wstring memberTag(hashLength * 2UL, L'0');
        for (DWORD index = 0UL; index < hashLength; ++index) {
            memberTag[index * 2UL] = hex[hash[index] >> 4];
            memberTag[index * 2UL + 1UL] = hex[hash[index] & 0x0FU];
        }

        WINTRUST_CATALOG_INFO catalogTrust = {};
        catalogTrust.cbStruct = sizeof(catalogTrust);
        catalogTrust.pcwszCatalogFilePath = catalog.wszCatalogFile;
        catalogTrust.pcwszMemberTag = memberTag.c_str();
        catalogTrust.pcwszMemberFilePath = driverPath;
        catalogTrust.hMemberFile = file;
        catalogTrust.pbCalculatedFileHash = hash.data();
        catalogTrust.cbCalculatedFileHash = hashLength;
        catalogTrust.hCatAdmin = catalogAdmin;

        WINTRUST_DATA trustData = {};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_CATALOG;
        trustData.pCatalog = &catalogTrust;
        trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;
        status = VerifyTrustData(&trustData, publisher, thumbprint);
    }

Exit:
    if (catalogContext != nullptr) {
        (void)CryptCATAdminReleaseCatalogContext(catalogAdmin, catalogContext, 0UL);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (catalogAdmin != nullptr) (void)CryptCATAdminReleaseContext(catalogAdmin, 0UL);
    return status;
}

void CopyPublisher(const std::wstring& value, LPWSTR output, DWORD characters)
{
    if (output == nullptr || characters == 0UL) return;
    wcsncpy_s(output, characters, value.c_str(), _TRUNCATE);
}

bool IsPinnedJiYuTestSigner(const std::wstring& publisher, const std::wstring& thumbprint)
{
    return _wcsicmp(publisher.c_str(), kJiYuTestSignerSubject) == 0 &&
        _wcsicmp(thumbprint.c_str(), kJiYuTestSignerThumbprint) == 0;
}

bool IsAcceptedPublisherTrust(LONG trustStatus, const std::wstring& publisher, const std::wstring& thumbprint)
{
    if (trustStatus == ERROR_SUCCESS) return IsAllowedDriverPublisher(publisher.c_str());

    // Private test certificates remain acceptable only when both the exact
    // subject and the pinned leaf thumbprint match the release configuration.
    // Missing signatures, bad digests, expired certificates, and arbitrary
    // certificates with the same display name remain rejected.
    return trustStatus == CERT_E_UNTRUSTEDROOT &&
        (IsJiYuDriverPublisher(publisher.c_str()) || IsPinnedJiYuTestSigner(publisher, thumbprint));
}

std::wstring NormalizeHex(std::wstring value)
{
    std::wstring normalized;
    for (wchar_t c : value) {
        if (c == L' ' || c == L'\t' || c == L'-' || c == L':') continue;
        if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - (L'a' - L'A'));
        normalized.push_back(c);
    }
    return normalized;
}

bool RegistryListContains(LPCWSTR keyName, LPCWSTR candidate, bool normalizeHex)
{
    if (candidate == nullptr || candidate[0] == L'\0') return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyName, 0,
        KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;

    bool found = false;
    for (DWORD index = 0; !found; ++index) {
        wchar_t valueName[256] = {};
        wchar_t valueData[512] = {};
        DWORD valueNameLength = _countof(valueName);
        DWORD valueDataLength = sizeof(valueData);
        DWORD valueType = 0;
        LONG status = RegEnumValueW(key, index, valueName, &valueNameLength,
            nullptr, &valueType, reinterpret_cast<LPBYTE>(valueData), &valueDataLength);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || (valueType != REG_SZ && valueType != REG_EXPAND_SZ)) continue;
        std::wstring expected = candidate;
        std::wstring name = valueName;
        std::wstring data = valueData;
        if (normalizeHex) {
            expected = NormalizeHex(expected);
            name = NormalizeHex(name);
            data = NormalizeHex(data);
        }
        found = _wcsicmp(name.c_str(), expected.c_str()) == 0 ||
            _wcsicmp(data.c_str(), expected.c_str()) == 0;
    }
    RegCloseKey(key);
    return found;
}

bool IsWindowsProtectedDriverPath(LPCWSTR driverPath)
{
    if (driverPath == nullptr || driverPath[0] == L'\0') return false;
    wchar_t fullPath[MAX_PATH * 4] = {};
    if (GetFullPathNameW(driverPath, _countof(fullPath), fullPath, nullptr) == 0) return false;
    for (wchar_t& c : fullPath) {
        if (c == L'/') c = L'\\';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    wchar_t windowsDirectory[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) == 0) return false;
    std::wstring root = windowsDirectory;
    for (wchar_t& c : root) {
        if (c == L'/') c = L'\\';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    const std::wstring prefixes[] = {
        root + L"\\system32\\drivers\\",
        root + L"\\syswow64\\drivers\\",
        root + L"\\driverstore\\filerepository\\",
        root + L"\\winsxs\\"
    };
    for (const std::wstring& prefix : prefixes) {
        if (_wcsnicmp(fullPath, prefix.c_str(), prefix.size()) == 0) return true;
    }
    return false;
}

bool ComputeMd5Hex(LPCWSTR driverPath, std::wstring* output)
{
    if (output == nullptr) return false;
    output->clear();
    HANDLE file = CreateFileW(driverPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<BYTE> object;
    BYTE digest[16] = {};
    BYTE buffer[64 * 1024] = {};
    ULONG objectLength = 0, resultLength = 0;
    DWORD bytesRead = 0;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) != 0) goto Exit;
    object.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) goto Exit;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) goto Exit;
        if (bytesRead == 0) break;
        if (BCryptHashData(hash, buffer, bytesRead, 0) != 0) goto Exit;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) goto Exit;
    {
        static const wchar_t hex[] = L"0123456789ABCDEF";
        for (BYTE value : digest) {
            output->push_back(hex[value >> 4]);
            output->push_back(hex[value & 0x0F]);
        }
    }
    ok = true;
Exit:
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

} // namespace

BOOL QueryDriverSignatureDetails(LPCWSTR driverPath, std::wstring* publisher,
    std::wstring* thumbprint, LONG* trustStatus)
{
    if (publisher != nullptr) publisher->clear();
    if (thumbprint != nullptr) thumbprint->clear();
    if (trustStatus != nullptr) *trustStatus = E_INVALIDARG;
    if (driverPath == nullptr || driverPath[0] == L'\0') return FALSE;
    std::wstring embeddedPublisher;
    std::wstring embeddedThumbprint;
    LONG embeddedStatus = VerifyEmbeddedSignature(driverPath, &embeddedPublisher, &embeddedThumbprint);
    if (embeddedStatus == ERROR_SUCCESS) {
        if (publisher != nullptr) *publisher = embeddedPublisher;
        if (thumbprint != nullptr) *thumbprint = embeddedThumbprint;
        if (trustStatus != nullptr) *trustStatus = embeddedStatus;
        return TRUE;
    }
    std::wstring catalogPublisher;
    std::wstring catalogThumbprint;
    LONG catalogStatus = VerifyCatalogSignature(driverPath, &catalogPublisher, &catalogThumbprint);
    const std::wstring& selected = catalogStatus == ERROR_SUCCESS
        ? catalogPublisher
        : embeddedPublisher;
    const std::wstring& selectedThumbprint = catalogStatus == ERROR_SUCCESS
        ? catalogThumbprint
        : embeddedThumbprint;
    if (publisher != nullptr) *publisher = selected;
    if (thumbprint != nullptr) *thumbprint = selectedThumbprint;
    if (trustStatus != nullptr) *trustStatus = catalogStatus == ERROR_SUCCESS
        ? catalogStatus
        : embeddedStatus;
    return catalogStatus == ERROR_SUCCESS;
}

BOOL QueryDriverSignature(LPCWSTR driverPath, LPWSTR publisher,
    DWORD publisherCharacters, LONG* trustStatus)
{
    std::wstring verifiedPublisher;
    const BOOL verified = QueryDriverSignatureDetails(driverPath, &verifiedPublisher, nullptr, trustStatus);
    CopyPublisher(verifiedPublisher, publisher, publisherCharacters);
    return verified;
}

BOOL VerifyAllowedDriverSignature(
    LPCWSTR driverPath,
    LPWSTR publisher,
    DWORD publisherCharacters,
    LONG* trustStatus)
{
    if (publisher != nullptr && publisherCharacters != 0UL) publisher[0] = L'\0';
    if (trustStatus != nullptr) *trustStatus = E_INVALIDARG;
    if (driverPath == nullptr || driverPath[0] == L'\0') return FALSE;

    LONG status = E_FAIL;
    std::wstring selectedPublisher;
    std::wstring signerThumbprint;
    QueryDriverSignatureDetails(driverPath, &selectedPublisher, &signerThumbprint, &status);
    CopyPublisher(selectedPublisher, publisher, publisherCharacters);
    if (IsAcceptedPublisherTrust(status, selectedPublisher, signerThumbprint)) {
        if (trustStatus != nullptr) *trustStatus = ERROR_SUCCESS;
        return TRUE;
    }
    if (trustStatus != nullptr) *trustStatus = status;
    return FALSE;
}

BOOL EvaluateDriverTrust(LPCWSTR driverPath, DRIVER_TRUST_RESULT* result)
{
    if (result == nullptr) return FALSE;
    ZeroMemory(result, sizeof(*result));
    result->reason = DRIVER_TRUST_UNTRUSTED;
    result->trustStatus = E_INVALIDARG;
    if (driverPath == nullptr || driverPath[0] == L'\0') return FALSE;

    if (IsWindowsProtectedDriverPath(driverPath)) {
        result->reason = DRIVER_TRUST_SYSTEM_PROTECTED;
        result->trustStatus = ERROR_SUCCESS;
        return TRUE;
    }

    std::wstring signerPublisher;
    std::wstring signerThumbprint;
    QueryDriverSignatureDetails(driverPath, &signerPublisher, &signerThumbprint, &result->trustStatus);
    CopyPublisher(signerPublisher, result->publisher, _countof(result->publisher));
    if (IsAcceptedPublisherTrust(result->trustStatus, signerPublisher, signerThumbprint)) {
        result->reason = DRIVER_TRUST_STATIC_PUBLISHER;
        return TRUE;
    }
    if (result->trustStatus == ERROR_SUCCESS &&
        RegistryListContains(L"SOFTWARE\\DzjsTrainer\\DriverPublisherAllowlist",
            result->publisher, false)) {
        result->reason = DRIVER_TRUST_DYNAMIC_PUBLISHER;
        return TRUE;
    }
    std::wstring md5;
    if (ComputeMd5Hex(driverPath, &md5) &&
        RegistryListContains(L"SOFTWARE\\DzjsTrainer\\DriverMd5Allowlist",
            md5.c_str(), true)) {
        result->reason = DRIVER_TRUST_MD5;
        return TRUE;
    }
    return FALSE;
}
