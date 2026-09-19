#include "DriverLoader.h"
#include "stdafx.h"
#include "DriverLoader.h"
#include "DzjsTrainer.h"
#include "KernelUtils.h"
#include "SysHlp.h"
#include "AppPublic.h"
#include "RegHlp.h"
#include "Logger.h"
#include <shellapi.h>
#include <Shlwapi.h>
#include "ActiveDefenseDemo.h"
#include "DriverSignaturePolicy.h"
#include "DriverGuard.h"
#include "AvIntegrated.h"
#include <winioctl.h>
#include <sddl.h>
#include "../DzjsTrainerDriver/IoCtl.h"
#include "../DzjsTrainerDriver/IoStructs.h"

extern JTApp * currentApp;
extern LoggerInternal * currentLogger;

HANDLE hKDrv = NULL;
BOOL selfProtectInitialized = FALSE;
static HANDLE hDriverEventStop = NULL;
static HANDLE hDriverEventThread = NULL;
static HANDLE hDriverHeartbeatStop = NULL;
static HANDLE hDriverHeartbeatThread = NULL;
static HANDLE hDriverWatchdogStop = NULL;
static HANDLE hDriverWatchdogProcess = NULL;
static HANDLE hDriverHeartbeatPulse = NULL;
static ULONG driverCapabilities = 0UL;
static volatile LONG driverHeartbeatSequence = 0L;

static DWORD WINAPI DriverEventMonitorThread(LPVOID)
{
	const DWORD batchHeaderSize = FIELD_OFFSET(JDRV_EVENT_BATCH, events);

	for (;;) {
		DWORD waitResult = WaitForSingleObject(hDriverEventStop, 250);
		if (waitResult == WAIT_OBJECT_0) break;
		if (waitResult != WAIT_TIMEOUT) {
			if (currentLogger)
				currentLogger->LogWarn(L"Driver event monitor wait failed: %d", GetLastError());
			break;
		}

		JDRV_EVENT_BATCH batch = {};
		DWORD returned = 0;
		if (!DeviceIoControl(
				hKDrv,
				CTL_READ_EVENTS,
				NULL,
				0,
				&batch,
				sizeof(batch),
				&returned,
				NULL)) {
			DWORD error = GetLastError();
			if (WaitForSingleObject(hDriverEventStop, 0) == WAIT_OBJECT_0)
				break;
			if (currentLogger)
				currentLogger->LogWarn(L"Driver event read stopped: %d", error);
			break;
		}

		DWORD requiredSize = batchHeaderSize +
			batch.eventCount * sizeof(JDRV_EVENT_RECORD);
		if (returned < batchHeaderSize ||
			batch.version != JDRV_EVENT_VERSION ||
			batch.eventCount > JDRV_EVENT_BATCH_CAPACITY ||
			batch.size != requiredSize ||
			returned < requiredSize) {
			if (currentLogger)
				currentLogger->LogWarn(
					L"Driver event batch rejected: bytes=%lu size=%lu version=%lu count=%lu",
					returned,
					batch.size,
					batch.version,
					batch.eventCount);
			continue;
		}

		if (batch.droppedEvents != 0UL && currentLogger) {
			currentLogger->LogWarn(
				L"Kernel event queue overflow: dropped %lu oldest events",
				batch.droppedEvents);
		}

		for (ULONG index = 0UL; index < batch.eventCount; ++index) {
			const JDRV_EVENT_RECORD& eventRecord = batch.events[index];
			if (eventRecord.size != sizeof(eventRecord) ||
				eventRecord.version != JDRV_EVENT_VERSION ||
				(eventRecord.reserved != 0UL &&
					eventRecord.type != JDRV_EVENT_TYPE_DRIVER_BLOCKED)) {
				continue;
			}

			if (eventRecord.type == JDRV_EVENT_TYPE_IMAGE_LOAD) {
				WCHAR imagePath[JDRV_EVENT_IMAGE_PATH_CHARS + 1UL] = {};
				ULONG pathLength = eventRecord.imagePathLength;
				if (pathLength > JDRV_EVENT_IMAGE_PATH_CHARS)
					pathLength = JDRV_EVENT_IMAGE_PATH_CHARS;
				if (pathLength != 0UL) {
					memcpy(
						imagePath,
						eventRecord.imagePath,
						pathLength * sizeof(WCHAR));
				}
				imagePath[pathLength] = L'\0';
				if (currentLogger) {
					BOOL highRisk = ActiveDefenseDemoScoreImage(
						eventRecord.processId,
						pathLength != 0UL ? imagePath : L"",
						(eventRecord.flags & JDRV_EVENT_FLAG_SYSTEM_IMAGE) != 0UL,
						(eventRecord.flags & JDRV_EVENT_FLAG_PROTECTED_TARGET) != 0UL);
					currentLogger->LogInfo(
						L"Kernel image event seq=%I64u pid=%lu system=%u target=%u base=0x%I64X size=%I64u path=%s",
						eventRecord.sequence,
						eventRecord.processId,
						(eventRecord.flags & JDRV_EVENT_FLAG_SYSTEM_IMAGE) != 0UL,
						(eventRecord.flags & JDRV_EVENT_FLAG_PROTECTED_TARGET) != 0UL,
						eventRecord.imageBase,
						eventRecord.imageSize,
						pathLength != 0UL ? imagePath : L"<unknown>");
					if (highRisk)
						currentLogger->LogWarn(L"[DEFENSE-DEMO] high-risk image requires operator review");
				}
			}
			else if (eventRecord.type == JDRV_EVENT_TYPE_DRIVER_BLOCKED) {
				WCHAR imagePath[JDRV_EVENT_IMAGE_PATH_CHARS + 1UL] = {};
				ULONG pathLength = eventRecord.imagePathLength;
				if (pathLength > JDRV_EVENT_IMAGE_PATH_CHARS)
					pathLength = JDRV_EVENT_IMAGE_PATH_CHARS;
				if (pathLength != 0UL) {
					memcpy(
						imagePath,
						eventRecord.imagePath,
						pathLength * sizeof(WCHAR));
				}
				imagePath[pathLength] = L'\0';
				const wchar_t* reason = L"unknown";
				if (eventRecord.reserved == JDRV_GUARD_BLOCK_REASON_NOT_WHITELISTED)
					reason = L"not-whitelisted";
				else if (eventRecord.reserved == JDRV_GUARD_BLOCK_REASON_HASH_MISMATCH)
					reason = L"hash-mismatch";
				else if (eventRecord.reserved == JDRV_GUARD_BLOCK_REASON_PATCH_FAILED)
					reason = L"patch-failed(load-not-prevented)";
				if (currentLogger) {
					currentLogger->LogWarn(
						L"[DRIVER-GUARD] blocked driver load seq=%I64u reason=%s pid=%lu base=0x%I64X size=%I64u path=%s",
						eventRecord.sequence,
						reason,
						eventRecord.processId,
						eventRecord.imageBase,
						eventRecord.imageSize,
						pathLength != 0UL ? imagePath : L"<unknown>");
				}
			}
			else if (eventRecord.type == JDRV_EVENT_TYPE_THREAD_CREATE ||
				eventRecord.type == JDRV_EVENT_TYPE_THREAD_EXIT) {
				if (currentLogger) {
					currentLogger->LogInfo(
						L"Kernel thread event seq=%I64u action=%s pid=%lu tid=%lu target=%u",
						eventRecord.sequence,
						eventRecord.type == JDRV_EVENT_TYPE_THREAD_CREATE
							? L"create"
							: L"exit",
						eventRecord.processId,
						eventRecord.threadId,
						(eventRecord.flags & JDRV_EVENT_FLAG_PROTECTED_TARGET) != 0UL);
				}
			}
		}
	}

	return 0;
}

static void StopDriverEventMonitor()
{
	if (hDriverEventStop) SetEvent(hDriverEventStop);
	if (hDriverEventThread) {
		WaitForSingleObject(hDriverEventThread, INFINITE);
		CloseHandle(hDriverEventThread);
		hDriverEventThread = NULL;
	}
	if (hDriverEventStop) {
		CloseHandle(hDriverEventStop);
		hDriverEventStop = NULL;
	}
}

static BOOL RestartMainAfterHeartbeatFailure()
{
	static volatile LONG restartAttempts = 0L;
	LONG attempt = InterlockedIncrement(&restartAttempts);
	if (attempt > 3L || !currentApp) return FALSE;

	WCHAR commandLine[MAX_PATH * 2] = {};
	WCHAR executable[MAX_PATH] = {};
	if (GetModuleFileNameW(NULL, executable, _countof(executable)) == 0)
		return FALSE;
	if (swprintf_s(commandLine, L"\"%s\" --heartbeat-recovery", executable) < 0)
		return FALSE;

	STARTUPINFOW startupInfo = { sizeof(startupInfo) };
	PROCESS_INFORMATION processInfo = {};
	BOOL started = CreateProcessW(
		NULL,
		commandLine,
		NULL,
		NULL,
		FALSE,
		CREATE_UNICODE_ENVIRONMENT,
		NULL,
		NULL,
		&startupInfo,
		&processInfo);
	if (started) {
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
	}
	return started;
}

static DWORD WINAPI DriverHeartbeatThread(LPVOID)
{
	for (;;) {
		DWORD waitResult = WaitForSingleObject(hDriverHeartbeatStop, 5000);
		if (waitResult == WAIT_OBJECT_0) break;
		if (waitResult != WAIT_TIMEOUT) break;

		JDRV_HEARTBEAT_REQUEST request = {};
		JDRV_HEARTBEAT_RESPONSE response = {};
		DWORD returned = 0;
		request.size = sizeof(request);
		request.version = JDRV_PROTOCOL_VERSION;
		request.processId = GetCurrentProcessId();
		request.sequence = static_cast<ULONG>(InterlockedIncrement(&driverHeartbeatSequence));
		BOOL succeeded = DeviceIoControl(
			hKDrv,
			CTL_HEARTBEAT,
			&request,
			sizeof(request),
			&response,
			sizeof(response),
			&returned,
			NULL);
		if (!succeeded || returned < sizeof(response) ||
			response.version != JDRV_PROTOCOL_VERSION ||
			response.accepted == 0UL ||
			response.protocolVersion != JDRV_PROTOCOL_VERSION) {
			DWORD error = succeeded ? ERROR_REVISION_MISMATCH : GetLastError();
			if (currentLogger)
				currentLogger->LogError(
					L"Driver heartbeat failed: error=%lu returned=%lu version=%lu protocol=%lu",
					error,
					returned,
					response.version,
					response.protocolVersion);
						// A transient driver communication failure must not terminate the UI.
			// Stop the separate watchdog so it does not create a duplicate recovery
			// instance, then leave driver-backed commands in their normal failed state.
			if (hDriverWatchdogStop != NULL)
				SetEvent(hDriverWatchdogStop);
			if (currentLogger)
				currentLogger->LogWarn(L"Driver heartbeat stopped; application remains available and driver features will retry on their next use");
			break;
		}
		if (hDriverHeartbeatPulse) SetEvent(hDriverHeartbeatPulse);
	}
	return 0;
}

static void StopDriverHeartbeat()
{
	if (hDriverHeartbeatStop) SetEvent(hDriverHeartbeatStop);
	if (hDriverHeartbeatThread) {
		WaitForSingleObject(hDriverHeartbeatThread, INFINITE);
		CloseHandle(hDriverHeartbeatThread);
		hDriverHeartbeatThread = NULL;
	}
	if (hDriverHeartbeatStop) {
		CloseHandle(hDriverHeartbeatStop);
		hDriverHeartbeatStop = NULL;
	}
	if (hDriverHeartbeatPulse) {
		CloseHandle(hDriverHeartbeatPulse);
		hDriverHeartbeatPulse = NULL;
	}
}

static BOOL StartDriverHeartbeat()
{
	WCHAR pulseEventName[128] = {};
	StopDriverHeartbeat();
	if ((driverCapabilities & JDRV_CAPABILITY_HEARTBEAT) == 0UL)
		return TRUE;
	if (swprintf_s(pulseEventName,
		L"Global\\DzjsTrainerHeartbeatPulse_%lu",
		GetCurrentProcessId()) < 0)
		return FALSE;
	hDriverHeartbeatPulse = CreateEventW(NULL, FALSE, FALSE, pulseEventName);
	if (!hDriverHeartbeatPulse) return FALSE;
	hDriverHeartbeatStop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!hDriverHeartbeatStop) {
		CloseHandle(hDriverHeartbeatPulse);
		hDriverHeartbeatPulse = NULL;
		return FALSE;
	}
	hDriverHeartbeatThread = CreateThread(
		NULL, 0, DriverHeartbeatThread, NULL, 0, NULL);
	if (!hDriverHeartbeatThread) {
		CloseHandle(hDriverHeartbeatStop);
		hDriverHeartbeatStop = NULL;
		CloseHandle(hDriverHeartbeatPulse);
		hDriverHeartbeatPulse = NULL;
		return FALSE;
	}
	return TRUE;
}

static BOOL StartDriverWatchdog()
{
	WCHAR executable[MAX_PATH] = {};
	WCHAR commandLine[MAX_PATH * 4] = {};
	WCHAR stopEventName[128] = {};
	WCHAR pulseEventName[128] = {};
	DWORD processId = GetCurrentProcessId();
	if (GetModuleFileNameW(NULL, executable, _countof(executable)) == 0)
		return FALSE;
	if (swprintf_s(stopEventName, L"Global\\DzjsTrainerHeartbeatStop_%lu", processId) < 0)
		return FALSE;
	if (swprintf_s(pulseEventName, L"Global\\DzjsTrainerHeartbeatPulse_%lu", processId) < 0)
		return FALSE;
	hDriverWatchdogStop = CreateEventW(NULL, TRUE, FALSE, stopEventName);
	if (!hDriverWatchdogStop) return FALSE;
	if (swprintf_s(commandLine, L"\"%s\" --heartbeat-watchdog %lu \"%s\" \"%s\"",
		executable, processId, stopEventName, pulseEventName) < 0) {
		CloseHandle(hDriverWatchdogStop);
		hDriverWatchdogStop = NULL;
		return FALSE;
	}
	STARTUPINFOW startupInfo = { sizeof(startupInfo) };
	PROCESS_INFORMATION processInfo = {};
	BOOL started = CreateProcessW(NULL, commandLine, NULL, NULL, FALSE,
		CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, NULL, NULL,
		&startupInfo, &processInfo);
	if (!started) {
		CloseHandle(hDriverWatchdogStop);
		hDriverWatchdogStop = NULL;
		return FALSE;
	}
	CloseHandle(processInfo.hThread);
	hDriverWatchdogProcess = processInfo.hProcess;
	return TRUE;
}

static void StopDriverWatchdog()
{
	if (hDriverWatchdogStop) SetEvent(hDriverWatchdogStop);
	if (hDriverWatchdogProcess) {
		WaitForSingleObject(hDriverWatchdogProcess, 3000);
		CloseHandle(hDriverWatchdogProcess);
		hDriverWatchdogProcess = NULL;
	}
	if (hDriverWatchdogStop) {
		CloseHandle(hDriverWatchdogStop);
		hDriverWatchdogStop = NULL;
	}
}

int RunHeartbeatWatchdogMode()
{
	int argumentCount = 0;
	LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
	if (!arguments || argumentCount < 5) {
		if (arguments) LocalFree(arguments);
		return 2;
	}
	DWORD parentProcessId = wcstoul(arguments[2], NULL, 10);
	HANDLE parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentProcessId);
	HANDLE stopEvent = OpenEventW(SYNCHRONIZE, FALSE, arguments[3]);
	HANDLE pulseEvent = OpenEventW(SYNCHRONIZE, FALSE, arguments[4]);
	if (!parentProcess || !stopEvent || !pulseEvent) {
		if (parentProcess) CloseHandle(parentProcess);
		if (stopEvent) CloseHandle(stopEvent);
		if (pulseEvent) CloseHandle(pulseEvent);
		LocalFree(arguments);
		return 3;
	}
	HANDLE waits[3] = { parentProcess, stopEvent, pulseEvent };
	DWORD waitResult;
	for (;;) {
		waitResult = WaitForMultipleObjects(3, waits, FALSE, 15000);
		if (waitResult == WAIT_OBJECT_0 + 2) continue;
		break;
	}
	// A clean shutdown signals stopEvent before the parent exits.
	if ((waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) &&
		WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
		Sleep(1000);
		WCHAR executable[MAX_PATH] = {};
		if (GetModuleFileNameW(NULL, executable, _countof(executable)) != 0) {
			WCHAR commandLine[MAX_PATH * 2] = {};
			if (swprintf_s(commandLine, L"\"%s\" --heartbeat-recovery", executable) >= 0) {
				STARTUPINFOW startupInfo = { sizeof(startupInfo) };
				PROCESS_INFORMATION processInfo = {};
				if (CreateProcessW(NULL, commandLine, NULL, NULL, FALSE,
					CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &startupInfo, &processInfo)) {
					CloseHandle(processInfo.hThread);
					CloseHandle(processInfo.hProcess);
				}
			}
		}
	}
	CloseHandle(parentProcess);
	CloseHandle(stopEvent);
	CloseHandle(pulseEvent);
	LocalFree(arguments);
	return 0;
}

static BOOL StartDriverEventMonitor()
{
	StopDriverEventMonitor();
	if ((driverCapabilities & JDRV_CAPABILITY_EVENT_STREAM) == 0UL)
		return TRUE;

	hDriverEventStop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!hDriverEventStop) return FALSE;

	hDriverEventThread = CreateThread(
		NULL,
		0,
		DriverEventMonitorThread,
		NULL,
		0,
		NULL);
	if (!hDriverEventThread) {
		CloseHandle(hDriverEventStop);
		hDriverEventStop = NULL;
		return FALSE;
	}
	return TRUE;
}

// DeleteService is asynchronous: the service object remains marked for
// deletion until every open SCM/service handle is closed.  This is retained
// for the legacy services which are intentionally removed by this module.
static BOOL WaitForServiceRemoval(const wchar_t* serviceName, DWORD timeoutMs)
{
	const DWORD intervalMs = 100;
	for (DWORD elapsed = 0; elapsed <= timeoutMs; elapsed += intervalMs) {
		SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (manager) {
			SC_HANDLE service = OpenServiceW(manager, serviceName, SERVICE_QUERY_STATUS);
			if (!service) {
				DWORD error = GetLastError();
				CloseServiceHandle(manager);
				if (error == ERROR_SERVICE_DOES_NOT_EXIST || error == ERROR_FILE_NOT_FOUND)
					return TRUE;
			} else {
				CloseServiceHandle(service);
				CloseServiceHandle(manager);
			}
		} else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
			return TRUE;
		}
		if (elapsed < timeoutMs) Sleep(intervalMs);
	}
	return FALSE;
}

static BOOL ServiceBinaryPathMatches(SC_HANDLE service, const wchar_t* expectedPath)
{
	if (!service || !expectedPath) return FALSE;
	DWORD required = 0;
	QueryServiceConfigW(service, nullptr, 0, &required);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return FALSE;
	std::vector<BYTE> buffer(required);
	QUERY_SERVICE_CONFIGW* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
	if (!QueryServiceConfigW(service, config, required, &required) || !config->lpBinaryPathName) return FALSE;
	std::wstring actual = config->lpBinaryPathName;
	std::wstring expected = expectedPath;
	if (actual.size() >= 4 && _wcsnicmp(actual.c_str(), L"\\??\\", 4) == 0) actual.erase(0, 4);
	if (actual.size() >= 2 && actual.front() == L'"' && actual.back() == L'"') actual = actual.substr(1, actual.size() - 2);
	return _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}

static BOOL GetServiceBinaryPath(SC_HANDLE service, std::wstring* path)
{
	if (!service || path == nullptr) return FALSE;
	DWORD required = 0;
	QueryServiceConfigW(service, nullptr, 0, &required);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0UL) return FALSE;
	std::vector<BYTE> buffer(required);
	QUERY_SERVICE_CONFIGW* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
	if (!QueryServiceConfigW(service, config, required, &required) || !config->lpBinaryPathName)
		return FALSE;

	std::wstring value = config->lpBinaryPathName;
	if (!value.empty() && value.front() == L'"') {
		size_t closingQuote = value.find(L'"', 1UL);
		if (closingQuote == std::wstring::npos) return FALSE;
		value = value.substr(1UL, closingQuote - 1UL);
	}
	if (value.rfind(L"\\??\\", 0UL) == 0UL) value.erase(0UL, 4UL);

	wchar_t windowsDirectory[MAX_PATH] = {};
	if (value.rfind(L"\\SystemRoot\\", 0UL) == 0UL ||
		value.rfind(L"System32\\", 0UL) == 0UL) {
		if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) == 0UL)
			return FALSE;
		value = std::wstring(windowsDirectory) + L"\\" +
			(value.front() == L'\\' ? value.substr(12UL) : value);
	}

	DWORD expandedLength = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0UL);
	if (expandedLength != 0UL) {
		std::vector<wchar_t> expanded(expandedLength);
		if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expandedLength) != 0UL)
			value.assign(expanded.data());
	}
	DWORD fullLength = GetFullPathNameW(value.c_str(), 0UL, nullptr, nullptr);
	if (fullLength == 0UL) return FALSE;
	std::vector<wchar_t> fullPath(fullLength + 1UL);
	if (GetFullPathNameW(value.c_str(), static_cast<DWORD>(fullPath.size()), fullPath.data(), nullptr) == 0UL)
		return FALSE;
	path->assign(fullPath.data());
	return TRUE;
}

static BOOL DriverSignaturePolicyAllows(LPCWSTR serviceName, LPCWSTR driverPath)
{
	wchar_t publisher[256] = {};
	LONG trustStatus = E_FAIL;
	if (VerifyAllowedDriverSignature(
			driverPath,
			publisher,
			_countof(publisher),
			&trustStatus)) {
		if (currentLogger) {
			currentLogger->LogInfo(
				L"Driver signature allowed: service=%s publisher=%s path=%s",
				serviceName,
				publisher,
				driverPath);
		}
		return TRUE;
	}
	if (currentLogger) {
		currentLogger->LogError(
			L"Driver signature rejected: service=%s trust=0x%08lX publisher=%s path=%s",
			serviceName,
			static_cast<unsigned long>(trustStatus),
			publisher[0] != L'\0' ? publisher : L"<unverified>",
			driverPath);
	}
	SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
	return FALSE;
}

// ---------------------------------------------------------------------------
// 主驱动服务名由 App 侧独立日期算法生成，不从 .sys 文件名派生。
// ---------------------------------------------------------------------------
LPCWSTR XGetDriverServiceName()
{
	return JTAppGetDriverServiceNameDirect();
}

// ---------------------------------------------------------------------------
// AV 内核驱动服务名由 App 侧独立日期算法生成（算法与主驱动不同）。
// ---------------------------------------------------------------------------
LPCWSTR XGetAvDriverServiceName()
{
	return JTAppGetAvServiceNameDirect();
}

static BOOL ServiceBinaryPathInsideDirectory(SC_HANDLE service, const std::wstring& directory)
{
	std::wstring binaryPath;
	if (!GetServiceBinaryPath(service, &binaryPath)) return FALSE;
	if (binaryPath.size() <= directory.size()) return FALSE;
	if (_wcsnicmp(binaryPath.c_str(), directory.c_str(), directory.size()) != 0) return FALSE;
	return binaryPath[directory.size()] == L'\\';
}

static BOOL IsTrainerDriverImagePath(SC_HANDLE service)
{
	std::wstring binaryPath;
	if (!GetServiceBinaryPath(service, &binaryPath)) return FALSE;
size_t separator = binaryPath.rfind(L'\\');
	std::wstring fileName = separator == std::wstring::npos
		? binaryPath : binaryPath.substr(separator + 1);
	if (fileName.size() < 4 || _wcsicmp(fileName.substr(fileName.size() - 4).c_str(), L".sys") != 0)
		return FALSE;
	fileName.resize(fileName.size() - 4);
	if (_wcsicmp(fileName.c_str(), L"DzjsTrainerDriver") == 0 ||
		_wcsicmp(fileName.c_str(), L"JiYuAvKernel") == 0)
		return TRUE;
	if (fileName.size() != 8 && fileName.size() != 9) return FALSE;
	for (wchar_t c : fileName) {
		if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
			(c >= L'2' && c <= L'9'))) return FALSE;
	}
	return TRUE;
}
static void StopAndDeleteForeignDriverService(SC_HANDLE service, const wchar_t* serviceName)
{
	SERVICE_STATUS status = {};
	if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (!QueryServiceStatus(service, &status)) break;
			if (status.dwCurrentState == SERVICE_STOPPED) break;
			Sleep(100);
		}
	}
	if (!DeleteService(service)) {
		if (currentLogger)
			currentLogger->LogWarn(L"清扫遗留驱动服务 %s 删除失败：%d", serviceName, GetLastError());
		return;
	}
	MRegForceDeleteServiceRegkey((LPWSTR)serviceName);
	if (!WaitForServiceRemoval(serviceName, 5000)) {
		if (currentLogger)
			currentLogger->LogWarn(L"遗留驱动服务 %s 仍处于待删除状态；重启后自动清除", serviceName);
	}
}

// 清扫镜像位于本程序目录、但不属于当前随机名（主驱动/AV 驱动）的内核驱动服务：
//  - 旧版固定名 DzjsTrainerDriver / JiYuAvKernel（含正在运行、需卸载授权的自保护实例）
//  - 跨天随机名变更后遗留的孤儿服务
static void XRemoveForeignTrainerDriverServices()
{
	LPCWSTR driverPath = currentApp ? currentApp->GetPartFullPath(PART_DRIVER) : nullptr;
	if (driverPath == nullptr || driverPath[0] == L'\0') return;
	std::wstring directory = driverPath;
	size_t lastSeparator = directory.rfind(L'\\');
	if (lastSeparator == std::wstring::npos || lastSeparator == 0) return;
	directory.erase(lastSeparator);

	SC_HANDLE serviceManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (serviceManager == NULL) return;

	DWORD bufferSize = 0;
	DWORD serviceCount = 0;
	DWORD resumeHandle = 0;
	if (!EnumServicesStatusExW(
			serviceManager,
			SC_ENUM_PROCESS_INFO,
			SERVICE_KERNEL_DRIVER,
			SERVICE_STATE_ALL,
			NULL,
			0,
			&bufferSize,
			&serviceCount,
			&resumeHandle,
			NULL) &&
		GetLastError() != ERROR_MORE_DATA) {
		CloseServiceHandle(serviceManager);
		return;
	}
	std::vector<BYTE> buffer(bufferSize);
	if (!EnumServicesStatusExW(
			serviceManager,
			SC_ENUM_PROCESS_INFO,
			SERVICE_KERNEL_DRIVER,
			SERVICE_STATE_ALL,
			buffer.data(),
			bufferSize,
			&bufferSize,
			&serviceCount,
			&resumeHandle,
			NULL)) {
		CloseServiceHandle(serviceManager);
		return;
	}

	ENUM_SERVICE_STATUS_PROCESSW* services =
		reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
	LPCWSTR currentName = XGetDriverServiceName();
	LPCWSTR currentAvName = XGetAvDriverServiceName();
	std::vector<std::wstring> foreignNames;
	BOOL foreignRunning = FALSE;
	for (DWORD index = 0; index < serviceCount; ++index) {
		const wchar_t* name = services[index].lpServiceName;
		if (name == nullptr || name[0] == L'\0') continue;
		if (currentName[0] != L'\0' && _wcsicmp(name, currentName) == 0) continue;
		if (currentAvName[0] != L'\0' && _wcsicmp(name, currentAvName) == 0) continue;
		SC_HANDLE service = OpenServiceW(serviceManager, name, SERVICE_ALL_ACCESS);
		if (service == NULL) continue;
		if (ServiceBinaryPathInsideDirectory(service, directory) && IsTrainerDriverImagePath(service)) {
			foreignNames.emplace_back(name);
			if (services[index].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING)
				foreignRunning = TRUE;
		}
		CloseServiceHandle(service);
	}
	if (foreignNames.empty()) {
		CloseServiceHandle(serviceManager);
		return;
	}

	// 正在运行的旧驱动带自保护，SCM 无法直接停止；先经设备句柄做卸载授权，
	// 再由下面的 ControlService 完成真正的卸载。
	if (foreignRunning && !XDriverLoaded() && XTestDriverCanUse()) {
		if (XOpenDriver(FALSE)) {
			KFBeforeUnInitDriver();
			XCloseDriverHandle();
		}
	}
	// 同理：仍在运行的旧 AV 内核驱动（昨日随机服务名或旧固定名）也需先经
	// 其设备句柄授权卸载；仅当今名 AV 服务不是运行中的实例时才授权。
	if (foreignRunning) AvIntegratedArmLegacyDriverForUnload();

	for (size_t index = 0; index < foreignNames.size(); ++index) {
		SC_HANDLE service = OpenServiceW(
			serviceManager, foreignNames[index].c_str(), SERVICE_ALL_ACCESS);
		if (service == NULL) continue;
		if (currentLogger)
			currentLogger->LogInfo(L"清扫遗留驱动服务：%s", foreignNames[index].c_str());
		StopAndDeleteForeignDriverService(service, foreignNames[index].c_str());
		CloseServiceHandle(service);
	}
	CloseServiceHandle(serviceManager);
}

//加载驱动
//    lpszDriverName：驱动的服务名
//    driverPath：驱动的完整路径
//    lpszDisplayName：nullptr
BOOL MLoadKernelDriver(const wchar_t* lpszDriverName, const wchar_t* driverPath, const wchar_t* lpszDisplayName)
{
	if (lpszDriverName == nullptr || lpszDriverName[0] == L'\0' ||
		driverPath == nullptr || driverPath[0] == L'\0') {
		if (currentLogger) currentLogger->LogError(L"拒绝加载驱动：服务名或镜像路径为空");
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const wchar_t* extension = PathFindExtensionW(driverPath);
	if (extension == nullptr || _wcsicmp(extension, L".sys") != 0) {
		if (currentLogger) currentLogger->LogError(
			L"拒绝加载非内核驱动镜像：service=%s path=%s",
			lpszDriverName,
			driverPath);
		SetLastError(ERROR_BAD_EXE_FORMAT);
		return FALSE;
	}
	currentLogger->LogInfo(L"准备加载内核驱动服务 %s，镜像路径 %s", lpszDriverName, driverPath);
	if (!DriverSignaturePolicyAllows(lpszDriverName, driverPath)) return FALSE;
	wchar_t sDriverName[32];
	wcscpy_s(sDriverName, lpszDriverName);

	DWORD dwRtn = 0;
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		currentLogger->LogError2(L"Load driver error in OpenSCManager : %d", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}

	hServiceDDK = CreateService(hServiceMgr, lpszDriverName, lpszDisplayName, SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, driverPath, NULL, NULL, NULL, NULL, NULL);


	if (hServiceDDK == NULL)
	{
		dwRtn = GetLastError();
		if (dwRtn == ERROR_SERVICE_MARKED_FOR_DELETE)
		{
			currentLogger->LogError(L"Service %s is marked for deletion by the SCM; reboot Windows before loading it again", sDriverName);
			bRet = FALSE;
			goto BeforeLeave;
		}
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_EXISTS)
		{
			currentLogger->LogError2(L"Load driver error in CreateService : %d", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		hServiceDDK = OpenService(hServiceMgr, lpszDriverName, SERVICE_ALL_ACCESS);
		if (hServiceDDK == NULL)
		{
			dwRtn = GetLastError();
			currentLogger->LogError2(L"Load driver error in OpenService : %d", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		SERVICE_STATUS currentStatus = {};
		if (QueryServiceStatus(hServiceDDK, &currentStatus) &&
			currentStatus.dwCurrentState != SERVICE_STOPPED) {
			std::wstring activePath;
			if (!GetServiceBinaryPath(hServiceDDK, &activePath) ||
				!DriverSignaturePolicyAllows(lpszDriverName, activePath.c_str())) {
				bRet = FALSE;
				goto BeforeLeave;
			}
		}
		if (!ServiceBinaryPathMatches(hServiceDDK, driverPath)) {
			if (!ChangeServiceConfig(
					hServiceDDK,
					SERVICE_KERNEL_DRIVER,
					SERVICE_DEMAND_START,
					SERVICE_ERROR_IGNORE,
					driverPath,
					NULL,
					NULL,
					NULL,
					NULL,
					NULL,
					lpszDisplayName))
			{
				currentLogger->LogError2(L"Load driver error in ChangeServiceConfig (path differs and update is denied) : %d", GetLastError());
				bRet = FALSE;
				goto BeforeLeave;
			}
		}
		else {
			currentLogger->LogInfo(L"Existing service image path matches; skipped ChangeServiceConfig");
		}
	}
	{
		PSECURITY_DESCRIPTOR serviceSecurity = NULL;
		const wchar_t* serviceSddl = L"D:P(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)";
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
			serviceSddl, SDDL_REVISION_1, &serviceSecurity, NULL)) {
			if (!SetServiceObjectSecurity(hServiceDDK, DACL_SECURITY_INFORMATION, serviceSecurity)) {
				DWORD securityError = GetLastError();
				if (securityError == ERROR_ACCESS_DENIED) {
					// An existing service may have a locked-down SCM ACL. The ACL is
					// not required to start the already registered kernel driver.
					currentLogger->LogWarn(L"Service ACL update skipped (access denied); continuing with existing service security");
				}
				else {
					currentLogger->LogError2(L"Load driver error in SetServiceObjectSecurity : %d", securityError);
					LocalFree(serviceSecurity);
					bRet = FALSE;
					goto BeforeLeave;
				}
			}
			LocalFree(serviceSecurity);
		}
		else {
			currentLogger->LogWarn(L"Service ACL descriptor creation failed; continuing with existing service security");
		}
	}
	bRet = StartService(hServiceDDK, NULL, NULL);
	if (!bRet)
	{
		DWORD dwRtn = GetLastError();
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_ALREADY_RUNNING)
		{
			currentLogger->LogError2(L"Load driver error in StartService : %d", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		else
		{
			bRet = TRUE;
		}
	}
	if (bRet) {
		SERVICE_STATUS status = {};
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (!QueryServiceStatus(hServiceDDK, &status)) {
				currentLogger->LogError2(L"QueryServiceStatus failed: %d", GetLastError());
				bRet = FALSE;
				break;
			}
			if (status.dwCurrentState == SERVICE_RUNNING) break;
			if (status.dwCurrentState == SERVICE_STOPPED) {
				currentLogger->LogError2(L"Driver service stopped during startup: %d", status.dwWin32ExitCode);
				bRet = FALSE;
				break;
			}
			Sleep(100);
		}
		if (bRet && status.dwCurrentState != SERVICE_RUNNING) {
			currentLogger->LogError(L"Driver service did not reach RUNNING state");
			bRet = FALSE;
		}
	}
	currentLogger->LogInfo(L"内核驱动服务加载结果: %s", bRet ? L"RUNNING" : L"FAILED");
	//离开前关闭句柄
BeforeLeave:
	if (hServiceDDK) CloseServiceHandle(hServiceDDK);
	if (hServiceMgr) CloseServiceHandle(hServiceMgr);
	return bRet;
}
//卸载驱动
//    szSvrName：服务名
BOOL MUnLoadKernelDriver(const wchar_t* szSvrName)
{
	if (hKDrv && szSvrName != nullptr && wcscmp(szSvrName, XGetDriverServiceName()) == 0) {
		XStopDriverGuard();
		StopDriverHeartbeat();
		StopDriverEventMonitor();
		CloseHandle(hKDrv);
		hKDrv = NULL;
		driverCapabilities = 0UL;
	}

	BOOL bDeleted = FALSE;
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	SERVICE_STATUS SvrSta;
	DWORD stopError = ERROR_SUCCESS;
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		currentLogger->LogError2(L"UnLoad driver error in OpenSCManager : %d", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}
	//打开驱动所对应的服务
	hServiceDDK = OpenService(hServiceMgr, szSvrName, SERVICE_ALL_ACCESS);
	if (hServiceDDK == NULL)
	{
		if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
			currentLogger->LogWarn(L"UnLoad driver error because driver not load.");
		else currentLogger->LogError2(L"UnLoad driver error in OpenService : %d", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}
	//停止驱动程序，如果停止失败，只有重新启动才能，再动态加载。 
	if (!ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &SvrSta)) {
		stopError = GetLastError();
		if (stopError == ERROR_SERVICE_NOT_ACTIVE) {
			stopError = ERROR_SUCCESS;
		}
		else {
			currentLogger->LogError2(L"UnLoad driver error in ControlService : %d", stopError);
		}
	}
	//动态卸载驱动程序。
	// Keep reusable driver services registered. DeleteService is asynchronous
	// and can leave the service name stuck in ERROR_SERVICE_MARKED_FOR_DELETE
	// while another handle is alive; the next load can safely reuse a stopped
	// kernel-driver service and update its binary path.
	if ((szSvrName != nullptr && wcscmp(szSvrName, XGetDriverServiceName()) == 0) ||
		(szSvrName != nullptr && wcscmp(szSvrName, XGetAvDriverServiceName()) == 0)) {
		if (stopError == ERROR_SERVICE_MARKED_FOR_DELETE) {
			currentLogger->LogWarn(L"Driver service %s is already marked for deletion; reboot is required to clear the stale SCM object", szSvrName);
			bRet = FALSE;
			goto BeforeLeave;
		}
		if (stopError != ERROR_SUCCESS) {
			currentLogger->LogError(L"Driver service %s did not accept stop control; verify the running driver matches this EXE and its unload protocol", szSvrName);
			bRet = FALSE;
			goto BeforeLeave;
		}
		SERVICE_STATUS status = {};
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (!QueryServiceStatus(hServiceDDK, &status)) {
				currentLogger->LogError2(L"QueryServiceStatus after stop failed: %d", GetLastError());
				bRet = FALSE;
				goto BeforeLeave;
			}
			if (status.dwCurrentState == SERVICE_STOPPED) break;
			Sleep(100);
		}
		if (status.dwCurrentState != SERVICE_STOPPED) {
			currentLogger->LogError(L"Driver service %s did not reach STOPPED state after authorized unload", szSvrName);
			bRet = FALSE;
			goto BeforeLeave;
		}
		bRet = TRUE;
		goto BeforeLeave;
	}
	if (!DeleteService(hServiceDDK)) {
		currentLogger->LogError2(L"UnLoad driver error in DeleteService : %d", GetLastError());
		bRet = FALSE;
	}
	else bDeleted = TRUE;

BeforeLeave:
	//离开前关闭打开的句柄
	if (hServiceDDK) CloseServiceHandle(hServiceDDK);
	if (hServiceMgr) CloseServiceHandle(hServiceMgr);

	if (bDeleted) bRet = MRegForceDeleteServiceRegkey((LPWSTR)szSvrName);
	if (bDeleted && !WaitForServiceRemoval(szSvrName, 5000)) {
		currentLogger->LogWarn(L"Service %s is still marked for deletion; retrying load will wait for SCM", szSvrName);
	}

	return bRet;
}
BOOL MUnLoadDriverServiceWithMessage(const wchar_t* szSvrName)
{
	BOOL bDeleted = FALSE;
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	SERVICE_STATUS SvrSta;
	DWORD lastErr = 0;

	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		lastErr = GetLastError();
		FAST_STR_BINDER(str, L"卸载驱动错误，打开驱动管理错误：%s\n请尝试以管理员身份运行软件。", 128, SysHlp::ConvertErrorCodeToString(lastErr));
		MessageBox(NULL, str, L"DzjsTrainer - 错误", MB_ICONERROR);
		bRet = FALSE;
		goto BeforeLeave;
	}
	//打开驱动所对应的服务
	hServiceDDK = OpenService(hServiceMgr, szSvrName, SERVICE_ALL_ACCESS);
	if (hServiceDDK == NULL)
	{
		lastErr = GetLastError();
		if (lastErr == ERROR_SERVICE_DOES_NOT_EXIST) 
			MessageBox(NULL, L"驱动已卸载并删除，请不要重复操作", L"DzjsTrainer - 提示", MB_ICONEXCLAMATION);
		else if ( lastErr == ERROR_SERVICE_MARKED_FOR_DELETE) 
			MessageBox(NULL, L"没有在这台计算机上找到找到驱动，可能是驱动已经被卸载了", L"DzjsTrainer - 提示", MB_ICONEXCLAMATION);
		else {
			FAST_STR_BINDER(str, L"卸载驱动错误，打开驱动错误：%s", 128, SysHlp::ConvertErrorCodeToString(lastErr));
			MessageBox(NULL, str, L"DzjsTrainer - 错误", MB_ICONERROR);
		}
		bRet = FALSE;
		goto BeforeLeave;
	}
	//停止驱动程序，如果停止失败，只有重新启动才能，再动态加载。 
	if (!ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &SvrSta)) {
		lastErr = GetLastError();
		if (lastErr == ERROR_SERVICE_MARKED_FOR_DELETE) {
			MessageBox(NULL, L"驱动已卸载并删除，请不要重复操作", L"DzjsTrainer - 提示", MB_ICONEXCLAMATION);
			bRet = FALSE;
			goto BeforeLeave;
		}
		else {
			FAST_STR_BINDER(str, L"卸载驱动错误，停止驱动失败：%s", 128, SysHlp::ConvertErrorCodeToString(lastErr));
			MessageBox(NULL, str, L"DzjsTrainer - 错误", MB_ICONERROR);
		}
	}
	//动态卸载驱动程序。 
	if (!DeleteService(hServiceDDK)) {
		lastErr = GetLastError();
		if (lastErr == ERROR_SERVICE_MARKED_FOR_DELETE) 
			MessageBox(NULL, L"驱动已卸载并删除，请不要重复操作", L"DzjsTrainer - 提示", MB_ICONEXCLAMATION);
		else {
			FAST_STR_BINDER(str, L"卸载驱动错误，删除驱动错误：%s", 128, SysHlp::ConvertErrorCodeToString(lastErr));
			MessageBox(NULL, str, L"DzjsTrainer - 错误", MB_ICONERROR);
			bRet = FALSE;
		}
	}
	else bDeleted = TRUE;
BeforeLeave:
	//离开前关闭打开的句柄
	if (hServiceDDK) CloseServiceHandle(hServiceDDK);
	if (hServiceMgr) CloseServiceHandle(hServiceMgr);

	if (bDeleted) bRet = MRegForceDeleteServiceRegkey((LPWSTR)szSvrName);

	return bRet;
}
//打开驱动
BOOL XOpenDriver(BOOL startGuard)
{
	HANDLE driverHandle = CreateFile(L"\\\\.\\JKRK",
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (!driverHandle || driverHandle == INVALID_HANDLE_VALUE)
	{
		currentLogger->LogError(L"Get Kernel driver handle (CreateFile) failed : %d . ", GetLastError());
		return FALSE;
	}

	JDRV_VERSION_RESPONSE version = { 0 };
	DWORD returned = 0;
	BOOL querySucceeded = DeviceIoControl(
		driverHandle,
		CTL_QUERY_VERSION,
		NULL,
			0,
		&version,
		sizeof(version),
		&returned,
		NULL);
	if (!querySucceeded)
	{
		currentLogger->LogError(L"Kernel driver version query failed : %d . ", GetLastError());
		CloseHandle(driverHandle);
		return FALSE;
	}
	if (returned < sizeof(version) ||
		version.size < sizeof(version) ||
		version.magic != JDRV_PROTOCOL_MAGIC ||
		version.version != JDRV_PROTOCOL_VERSION ||
		version.architecture != JDRV_ARCHITECTURE_X64)
	{
		currentLogger->LogError(
			L"Kernel driver protocol or architecture mismatch: bytes=%lu, magic=0x%08lX, version=%lu, architecture=%lu.",
			returned,
			version.magic,
			version.version,
			version.architecture);
		CloseHandle(driverHandle);
		return FALSE;
	}

	hKDrv = driverHandle;
	driverCapabilities = version.capabilities;
	currentLogger->LogInfo(
		L"Kernel driver protocol %d, capabilities 0x%08X",
		version.version,
		version.capabilities);
	if (startGuard) {
		if (!StartDriverEventMonitor())
			currentLogger->LogWarn(L"Kernel driver loaded, but event monitor thread could not be started");
		else if ((driverCapabilities & JDRV_CAPABILITY_EVENT_STREAM) != 0UL)
			currentLogger->LogInfo(L"Kernel callback event monitor started");
		if (!StartDriverHeartbeat())
			currentLogger->LogWarn(L"Kernel driver loaded, but heartbeat thread could not be started");
		else if ((driverCapabilities & JDRV_CAPABILITY_HEARTBEAT) != 0UL)
			currentLogger->LogInfo(L"Driver heartbeat started: interval=5s");
		if ((driverCapabilities & JDRV_CAPABILITY_HEARTBEAT) != 0UL && !StartDriverWatchdog())
			currentLogger->LogWarn(L"Heartbeat active, but watchdog process could not be started");
		if ((driverCapabilities & JDRV_CAPABILITY_DRIVER_GUARD) != 0UL) {
			if (!XStartDriverGuard())
				currentLogger->LogWarn(L"Driver guard whitelist thread could not be started");
		}
		else {
			currentLogger->LogWarn(L"Kernel driver does not report the driver guard capability; malicious-driver fallback is disabled");
		}
	}
	return TRUE;
}
BOOL XDriverLoaded() {
	return hKDrv != NULL && hKDrv != INVALID_HANDLE_VALUE;
}

BOOL XTryReuseInstalledDriver()
{
	if (XDriverLoaded()) return TRUE;
	if (!XTestDriverCanUse()) return FALSE;

	// 先清走指向本目录的遗留/外来驱动服务（旧固定名、随机名变更后的孤儿）
	// Disabled: never delete services discovered by heuristic.

	SC_HANDLE serviceManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (!serviceManager) return FALSE;
	SC_HANDLE service = OpenService(
		serviceManager,
		XGetDriverServiceName(),
		SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
	if (!service) {
		CloseServiceHandle(serviceManager);
		return FALSE;
	}
	std::wstring installedDriverPath;
	if (!GetServiceBinaryPath(service, &installedDriverPath) ||
		!DriverSignaturePolicyAllows(XGetDriverServiceName(), installedDriverPath.c_str())) {
		CloseServiceHandle(service);
		CloseServiceHandle(serviceManager);
		return FALSE;
	}

	SERVICE_STATUS status = {};
	BOOL ready = QueryServiceStatus(service, &status);
	if (ready && status.dwCurrentState == SERVICE_STOPPED)
		ready = StartService(service, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
	if (ready) {
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (!QueryServiceStatus(service, &status)) { ready = FALSE; break; }
			if (status.dwCurrentState == SERVICE_RUNNING) break;
			if (status.dwCurrentState == SERVICE_STOPPED) { ready = FALSE; break; }
			Sleep(100);
		}
		if (ready && status.dwCurrentState != SERVICE_RUNNING) ready = FALSE;
	}

	BOOL compatible = ready && XOpenDriver(TRUE);
	if (compatible) {
		bool isWin7 = SysHlp::GetSystemVersion() == SystemVersionWindows7OrLater;
		bool isXp = SysHlp::GetSystemVersion() == SystemVersionWindowsXP;
		ULONG sysBulidVersion = SysHlp::GetWindowsBulidVersion();
		KFSendDriverinitParam(isXp, isWin7, sysBulidVersion);
		currentLogger->LogInfo(L"Reused installed driver service; compatible protocol verified, skipped reinstall");
	}
	else if (ready && status.dwCurrentState == SERVICE_RUNNING) {
		ControlService(service, SERVICE_CONTROL_STOP, &status);
	}

	CloseServiceHandle(service);
	CloseServiceHandle(serviceManager);
	return compatible;
}
BOOL XTestDriverCanUse() {

	if (!SysHlp::Is64BitOS()) {
		currentLogger->LogWarn(L"64 位驱动不支持 32 位 Windows");
		return FALSE;
	}
	if (!SysHlp::IsRunasAdmin()) {
		currentLogger->LogWarn(L"要加载驱动，请以管理员身份运行本程序");
		return FALSE;
	}

	return TRUE;
}
BOOL XInitSelfProtect()
{
	if (selfProtectInitialized) return TRUE;
	selfProtectInitialized = KFInstallSelfProtect();
	return selfProtectInitialized;
}
BOOL XLoadDriver() {

	if (XDriverLoaded()) return TRUE;
	if(!XTestDriverCanUse()) return FALSE;
	if (MLoadKernelDriver(XGetDriverServiceName(), currentApp->GetPartFullPath(PART_DRIVER), NULL))
	{
		if (XOpenDriver(TRUE)) {

			bool isWin7 = SysHlp::GetSystemVersion() == SystemVersionWindows7OrLater;
			bool isXp = SysHlp::GetSystemVersion() == SystemVersionWindowsXP;
			ULONG sysBulidVersion = SysHlp::GetWindowsBulidVersion();

			currentLogger->Log(L"Windows Bulid version %d", sysBulidVersion);
			KFSendDriverinitParam(isXp, isWin7, sysBulidVersion);
			
			currentLogger->LogInfo(L"驱动加载成功");
			return TRUE;
		}
		else currentLogger->LogWarn2(L"驱动加载成功，但打开驱动失败");
	}

	return FALSE;
}
BOOL XCloseDriverHandle() {
	if (XDriverLoaded()) {
		XStopDriverGuard();
		StopDriverWatchdog();
		StopDriverHeartbeat();
		StopDriverEventMonitor();
		KFUnInstallSelfProtect();
		selfProtectInitialized = FALSE;
		CloseHandle(hKDrv);
		hKDrv = nullptr;
		driverCapabilities = 0UL;
		return TRUE;
	}
	return FALSE;
}
BOOL XUnLoadDriver()
{
	if (!XDriverLoaded()) {
		currentLogger->LogError(L"Driver unload authorization requires an open driver handle");
		return FALSE;
	}
	if (!KFBeforeUnInitDriver()) {
		currentLogger->LogError(L"Driver unload authorization failed; service stop was not attempted");
		return FALSE;
	}
	// The authorization IOCTL must be sent while the device handle is open.
	// Only close it after the driver has installed its unload routine.
	XCloseDriverHandle();
	return MUnLoadKernelDriver(XGetDriverServiceName());
}
