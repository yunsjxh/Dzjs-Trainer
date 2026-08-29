#pragma once

#include <Windows.h>

enum DRIVER_TRUST_REASON {
    DRIVER_TRUST_UNTRUSTED = 0,
    DRIVER_TRUST_SYSTEM_PROTECTED = 1,
    DRIVER_TRUST_STATIC_PUBLISHER = 2,
    DRIVER_TRUST_DYNAMIC_PUBLISHER = 3,
    DRIVER_TRUST_MD5 = 4
};

struct DRIVER_TRUST_RESULT {
    DRIVER_TRUST_REASON reason;
    LONG trustStatus;
    WCHAR publisher[256];
};

BOOL QueryDriverSignature(
    LPCWSTR driverPath,
    LPWSTR publisher,
    DWORD publisherCharacters,
    LONG* trustStatus);

BOOL VerifyAllowedDriverSignature(
    LPCWSTR driverPath,
    LPWSTR publisher,
    DWORD publisherCharacters,
    LONG* trustStatus);

BOOL EvaluateDriverTrust(
    LPCWSTR driverPath,
    DRIVER_TRUST_RESULT* result);
