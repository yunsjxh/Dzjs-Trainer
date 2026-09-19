#include "stdafx.h"
#include "DriverServiceActions.h"
#include "../DzjsTrainer/DriverLoader.h"
#include "../DzjsTrainer/AvIntegrated.h"

#include <winternl.h>
#include <string>
#include <vector>

namespace {

bool QueryImagePath(SC_HANDLE service, std::wstring* output)
{
	if (!service || !output) return false;
	DWORD required = 0;
	QueryServiceConfigW(service, nullptr, 0, &required);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
	std::vector<BYTE> buffer(required);
	QUERY_SERVICE_CONFIGW* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
	if (!QueryServiceConfigW(service, config, required, &required) || !config->lpBinaryPathName)
		return false;
	std::wstring path = config->lpBinaryPathName;
	if (!path.empty() && path.front() == L'"') {
		const size_t end = path.find(L'"', 1);
		if (end == std::wstring::npos) return false;
		path = path.substr(1, end - 1);
	}
	if (path.rfind(L"\\??\\", 0) == 0) path.erase(0, 4);
	if (path.rfind(L"\\SystemRoot\\", 0) == 0) {
		wchar_t windowsDirectory[MAX_PATH] = {};
		if (!GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory))) return false;
		path = std::wstring(windowsDirectory) + path.substr(11);
	}
	DWORD expanded = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
	if (expanded != 0) {
		std::vector<wchar_t> expandedPath(expanded);
		if (ExpandEnvironmentStringsW(path.c_str(), expandedPath.data(), expanded) != 0)
			path.assign(expandedPath.data());
	}
	output->assign(path);
	return !output->empty();
}

bool IsKernelDriverService(SC_HANDLE service)
{
	if (!service) return false;
	DWORD required = 0;
	QueryServiceConfigW(service, nullptr, 0, &required);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
	std::vector<BYTE> buffer(required);
	auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
	if (!QueryServiceConfigW(service, config, required, &required)) return false;
	return (config->dwServiceType & SERVICE_DRIVER) != 0;
}

bool IsWindowsImage(const std::wstring& path)
{
	wchar_t windowsDirectory[MAX_PATH] = {};
	if (!GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory))) return true;
	std::wstring root = windowsDirectory;
	std::wstring normalized = path;
	for (wchar_t& c : root) {
		if (c == L'/') c = L'\\';
		if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
	}
	for (wchar_t& c : normalized) {
		if (c == L'/') c = L'\\';
		if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
	}
	return normalized.rfind(root + L"\\", 0) == 0;
}

bool StopAndWait(SC_HANDLE service, DWORD* errorCode)
{
	SERVICE_STATUS status = {};
	if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
		const DWORD error = GetLastError();
		if (error != ERROR_SERVICE_NOT_ACTIVE) {
			if (errorCode) *errorCode = error;
			return false;
		}
	}
	for (int attempt = 0; attempt < 50; ++attempt) {
		if (!QueryServiceStatus(service, &status)) {
			if (errorCode) *errorCode = GetLastError();
			return false;
		}
		if (status.dwCurrentState == SERVICE_STOPPED) return true;
		Sleep(100);
	}
	if (errorCode) *errorCode = ERROR_SERVICE_REQUEST_TIMEOUT;
	return false;
}

bool NativeUnload(LPCWSTR serviceName)
{
	using NtUnloadDriverFn = LONG(NTAPI*)(PUNICODE_STRING);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) return false;
	const auto unload = reinterpret_cast<NtUnloadDriverFn>(GetProcAddress(ntdll, "NtUnloadDriver"));
	if (!unload) return false;
	std::wstring path = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
	path += serviceName;
	if (path.size() > 0x7FFF / sizeof(wchar_t)) return false;
	UNICODE_STRING name = {};
	name.Buffer = const_cast<PWSTR>(path.c_str());
	name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
	name.MaximumLength = name.Length;
	return unload(&name) == 0;
}

}

BOOL UnloadSelectedDriverService(LPCWSTR serviceName, BOOL* forced, DWORD* errorCode)
{
	if (forced) *forced = FALSE;
	if (errorCode) *errorCode = ERROR_SUCCESS;
	if (!serviceName || !serviceName[0]) {
		if (errorCode) *errorCode = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager) {
		if (errorCode) *errorCode = GetLastError();
		return FALSE;
	}
	SC_HANDLE service = OpenServiceW(manager, serviceName,
		SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
	if (!service) {
		if (errorCode) *errorCode = GetLastError();
		CloseServiceHandle(manager);
		return FALSE;
	}
	if (!IsKernelDriverService(service)) {
		if (errorCode) *errorCode = ERROR_INVALID_PARAMETER;
		CloseServiceHandle(service);
		CloseServiceHandle(manager);
		return FALSE;
	}
	std::wstring imagePath;
	const bool pathKnown = QueryImagePath(service, &imagePath);
	DWORD normalError = ERROR_SUCCESS;
	bool stopped = false;
	const bool isPrimary = _wcsicmp(serviceName, XGetDriverServiceName()) == 0;
	const bool isAv = _wcsicmp(serviceName, XGetAvDriverServiceName()) == 0;
	if (isPrimary && XDriverLoaded()) {
		stopped = XUnLoadDriver() != FALSE;
		normalError = stopped ? ERROR_SUCCESS : GetLastError();
	}
	else if (isAv && AvIntegratedIsLoaded()) {
		stopped = AvIntegratedUnload() != FALSE;
		normalError = stopped ? ERROR_SUCCESS : GetLastError();
	}
	else {
		stopped = StopAndWait(service, &normalError);
	}
	DWORD deleteError = ERROR_SUCCESS;
	BOOL deleted = FALSE;
	if (stopped) {
		deleted = DeleteService(service);
		if (!deleted) deleteError = GetLastError();
	}
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	if (deleted || (stopped && deleteError == ERROR_SERVICE_MARKED_FOR_DELETE)) return TRUE;
	if (errorCode && *errorCode == ERROR_SUCCESS && normalError != ERROR_SUCCESS)
		*errorCode = normalError;
	if (errorCode && *errorCode == ERROR_SUCCESS && deleteError != ERROR_SUCCESS)
		*errorCode = deleteError;

	// Never apply the fallback to an image under the Windows directory.
	if (!pathKnown || IsWindowsImage(imagePath)) {
		if (errorCode && *errorCode == ERROR_SUCCESS) *errorCode = ERROR_ACCESS_DENIED;
		return FALSE;
	}
	if (!NativeUnload(serviceName)) {
		if (errorCode) *errorCode = ERROR_GEN_FAILURE;
		return FALSE;
	}
	SC_HANDLE retryManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
	if (!retryManager) {
		if (errorCode) *errorCode = GetLastError();
		return FALSE;
	}
	SC_HANDLE retryService = OpenServiceW(retryManager, serviceName, DELETE);
	const BOOL removed = retryService != nullptr && DeleteService(retryService);
	const DWORD removalError = removed ? ERROR_SUCCESS : GetLastError();
	if (retryService) CloseServiceHandle(retryService);
	CloseServiceHandle(retryManager);
	if (!removed && removalError != ERROR_SERVICE_MARKED_FOR_DELETE) {
		if (errorCode) *errorCode = removalError;
		return FALSE;
	}
	if (forced) *forced = TRUE;
	return TRUE;
}
