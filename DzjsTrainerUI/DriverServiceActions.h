#pragma once

#include <Windows.h>

// Operates only on a service explicitly selected by the diagnostics page.
// The fallback is rejected for images under the Windows directory.
BOOL UnloadSelectedDriverService(LPCWSTR serviceName, BOOL* forced, DWORD* errorCode);
