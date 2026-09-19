#include "stdafx.h"
#include "DriverGuard.h"
#include "DriverSignaturePolicy.h"
#include "AppPublic.h"
#include "Logger.h"
#include <bcrypt.h>
#include <winioctl.h>

#include <new>
#include <set>
#include <string>
#include <vector>

#include "../DzjsTrainerDriver/IoCtl.h"
#include "../DzjsTrainerDriver/IoStructs.h"

extern JTApp* currentApp;
extern LoggerInternal* currentLogger;
extern HANDLE hKDrv;

namespace {

// 最大白名单条目数，内核上限是 4096，留一点余量。
const size_t kMaxWhitelistEntries = 4000;
// DriverStore 递归枚举的文件数上限，防止超长后台扫描。
const size_t kMaxDriverStoreFiles = 2048;
// 单次 IOCTL 下发的批大小。
const ULONG kEntriesPerBatch = JDRV_GUARD_MAX_ENTRIES_PER_REQUEST;

struct GuardTrustCounters {
	ULONG systemProtected = 0;
	ULONG staticPublisher = 0;
	ULONG dynamicPublisher = 0;
	ULONG md5 = 0;
	ULONG rejected = 0;
};

HANDLE s_stopEvent = NULL;
HANDLE s_thread = NULL;
volatile LONG s_running = 0;

std::wstring Lowercase(std::wstring value)
{
	for (size_t index = 0; index < value.size(); ++index) {
		wchar_t c = value[index];
		if (c >= L'A' && c <= L'Z') value[index] = static_cast<wchar_t>(c + (L'a' - L'A'));
	}
	return value;
}

std::wstring BasenameOf(const std::wstring& path)
{
	size_t slash = path.find_last_of(L'\\');
	return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool ComputeSha256(LPCWSTR path, unsigned char* digest)
{
	HANDLE file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;

	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	unsigned char* object = NULL;
	DWORD objectLength = 0;
	DWORD resultLength = 0;
	bool ok = false;

	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
			&resultLength, 0) != 0) {
		goto Exit;
	}
	object = new (std::nothrow) unsigned char[objectLength];
	if (object == NULL) goto Exit;
	if (BCryptCreateHash(algorithm, &hash, object, objectLength, NULL, 0, 0) != 0)
		goto Exit;

	{
		std::vector<BYTE> buffer(64 * 1024);
		for (;;) {
			DWORD bytesRead = 0;
			if (!ReadFile(file, buffer.data(),
				static_cast<DWORD>(buffer.size()), &bytesRead, NULL)) {
				goto Exit;
			}
			if (bytesRead == 0) break;
			if (BCryptHashData(hash, buffer.data(), bytesRead, 0) != 0)
				goto Exit;
		}
	}
	ok = BCryptFinishHash(hash, digest, JDRV_GUARD_HASH_SIZE, 0) == 0;

Exit:
	if (hash != NULL) BCryptDestroyHash(hash);
	delete[] object;
	if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return ok;
}

bool SendGuardConfig(ULONG action, const JDRV_GUARD_ENTRY* entries, ULONG count)
{
	const ULONG headerSize = FIELD_OFFSET(JDRV_GUARD_CONFIG, entries);
	std::vector<BYTE> storage(headerSize + count * sizeof(JDRV_GUARD_ENTRY));
	JDRV_GUARD_CONFIG* request = reinterpret_cast<JDRV_GUARD_CONFIG*>(storage.data());
	request->size = headerSize + count * sizeof(JDRV_GUARD_ENTRY);
	request->version = JDRV_PROTOCOL_VERSION;
	request->action = action;
	request->entryCount = count;
	request->reserved = 0UL;
	if (count != 0UL && entries != NULL) {
		memcpy(request->entries, entries, count * sizeof(JDRV_GUARD_ENTRY));
	}

	JDRV_OPERATION_RESPONSE response = {};
	DWORD returned = 0;
	HANDLE driver = hKDrv;
	if (driver == NULL || driver == INVALID_HANDLE_VALUE) return false;
	if (!DeviceIoControl(driver, CTL_DRIVER_GUARD_CONFIG,
			request, static_cast<DWORD>(storage.size()),
			&response, sizeof(response), &returned, NULL)) {
		if (currentLogger)
			currentLogger->LogError(
				L"Driver guard config failed: action=%lu error=%lu",
				action, GetLastError());
		return false;
	}
	if (returned < sizeof(response) || response.size < sizeof(response) ||
		response.status != 0) {
		if (currentLogger)
			currentLogger->LogError(
				L"Driver guard config rejected: action=%lu status=0x%08lX",
				action, static_cast<unsigned long>(response.status));
		return false;
	}
	return true;
}

// 验证签名并计算 SHA-256，合格后加入条目向量。
bool TryBuildEntry(
	const std::wstring& path,
	std::vector<JDRV_GUARD_ENTRY>* entries,
	std::set<std::wstring>* seen,
	bool requireSignature,
	DRIVER_TRUST_REASON* trustReason,
	GuardTrustCounters* counters)
{
	if (trustReason != nullptr) *trustReason = DRIVER_TRUST_UNTRUSTED;
	std::wstring basename = BasenameOf(path);
	if (basename.empty() || basename.size() >= JDRV_GUARD_NAME_CHARS) return false;
	std::wstring lower = Lowercase(basename);
	if (lower.size() < 4 ||
		lower.compare(lower.size() - 4, 4, L".sys") != 0) return false;

	std::wstring dedupKey = lower + L"|" + Lowercase(path);
	if (!seen->insert(dedupKey).second) return false;

	if (requireSignature) {
		DRIVER_TRUST_RESULT trust = {};
		if (!EvaluateDriverTrust(path.c_str(), &trust)) {
			if (counters != nullptr) ++counters->rejected;
			return false;
		}
		if (trustReason != nullptr) *trustReason = trust.reason;
		if (counters != nullptr) {
			switch (trust.reason) {
			case DRIVER_TRUST_SYSTEM_PROTECTED: ++counters->systemProtected; break;
			case DRIVER_TRUST_STATIC_PUBLISHER: ++counters->staticPublisher; break;
			case DRIVER_TRUST_DYNAMIC_PUBLISHER: ++counters->dynamicPublisher; break;
			case DRIVER_TRUST_MD5: ++counters->md5; break;
			default: ++counters->rejected; break;
			}
		}
	}

	JDRV_GUARD_ENTRY entry = {};
	entry.size = sizeof(entry);
	entry.flags = 0UL;
	{
		LARGE_INTEGER fileSize = {};
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) return false;
		BOOL sized = GetFileSizeEx(file, &fileSize);
		CloseHandle(file);
		if (!sized) return false;
		entry.fileSize = static_cast<unsigned __int64>(fileSize.QuadPart);
	}
	if (ComputeSha256(path.c_str(), entry.sha256)) {
		entry.flags |= JDRV_GUARD_ENTRY_FLAG_HASH_VALID;
	}
	wcsncpy_s(reinterpret_cast<wchar_t*>(entry.name), JDRV_GUARD_NAME_CHARS,
		lower.c_str(), _TRUNCATE);
	entries->push_back(entry);
	return true;
}

void EnumerateSysFiles(
	const std::wstring& directory,
	bool recursive,
	std::vector<JDRV_GUARD_ENTRY>* entries,
	std::set<std::wstring>* seen,
	size_t cap,
	size_t* scanned,
	GuardTrustCounters* counters)
{
	if (*scanned >= cap) return;
	if (WaitForSingleObject(s_stopEvent, 0) == WAIT_OBJECT_0) return;

	std::wstring pattern = directory;
	if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
	std::wstring search = pattern + L"*";

	WIN32_FIND_DATAW findData = {};
	HANDLE find = FindFirstFileW(search.c_str(), &findData);
	if (find == INVALID_HANDLE_VALUE) return;
	do {
		if (*scanned >= cap ||
			WaitForSingleObject(s_stopEvent, 0) == WAIT_OBJECT_0) {
			break;
		}
		if (wcscmp(findData.cFileName, L".") == 0 ||
			wcscmp(findData.cFileName, L"..") == 0) {
			continue;
		}
		std::wstring fullPath = pattern + findData.cFileName;
		if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0UL) {
			if (recursive) {
				EnumerateSysFiles(fullPath, true, entries, seen, cap, scanned, counters);
			}
			continue;
		}
		size_t length = wcslen(findData.cFileName);
		if (length < 4 ||
			_wcsicmp(findData.cFileName + length - 4, L".sys") != 0) {
			continue;
		}
		++*scanned;
		TryBuildEntry(fullPath, entries, seen, true, nullptr, counters);
	} while (FindNextFileW(find, &findData));
	FindClose(find);
}

// 已加载模块路径形如 \SystemRoot\System32\...，转回 Win32 路径。
std::wstring NtPathToWin32(const std::wstring& ntPath)
{
	const std::wstring prefix = L"\\SystemRoot\\";
	if (_wcsnicmp(ntPath.c_str(), prefix.c_str(), prefix.size()) != 0) return ntPath;
	wchar_t windowsDirectory[MAX_PATH] = {};
	if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) == 0UL)
		return ntPath;
	return std::wstring(windowsDirectory) + ntPath.substr(prefix.size() - 1);
}

void EnumerateLoadedDrivers(
	std::vector<JDRV_GUARD_ENTRY>* entries,
	std::set<std::wstring>* seen,
	GuardTrustCounters* counters)
{
	typedef BOOL(WINAPI* EnumDeviceDriversFn)(LPVOID*, DWORD, LPDWORD);
	typedef DWORD(WINAPI* GetDeviceDriverFileNameFn)(LPVOID, LPWSTR, DWORD);
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (kernel32 == NULL) return;
	EnumDeviceDriversFn enumDeviceDrivers =
		reinterpret_cast<EnumDeviceDriversFn>(
			GetProcAddress(kernel32, "K32EnumDeviceDrivers"));
	GetDeviceDriverFileNameFn getFileName =
		reinterpret_cast<GetDeviceDriverFileNameFn>(
			GetProcAddress(kernel32, "K32GetDeviceDriverFileNameW"));
	if (enumDeviceDrivers == NULL || getFileName == NULL) return;

	DWORD required = 0;
	if (!enumDeviceDrivers(NULL, 0, &required) || required == 0) return;
	std::vector<LPVOID> addresses(required / sizeof(LPVOID));
	if (addresses.empty() ||
		!enumDeviceDrivers(addresses.data(),
			static_cast<DWORD>(addresses.size() * sizeof(LPVOID)), &required)) {
		return;
	}
	for (size_t index = 0; index < addresses.size(); ++index) {
		wchar_t fileName[MAX_PATH * 2] = {};
		if (getFileName(addresses[index], fileName, _countof(fileName)) == 0UL)
			continue;
		TryBuildEntry(NtPathToWin32(fileName), entries, seen, true, nullptr, counters);
	}
}

ULONG PushBatches(const std::vector<JDRV_GUARD_ENTRY>& entries, size_t begin)
{
	size_t index = begin;
	ULONG pushed = 0;
	while (index < entries.size()) {
		if (WaitForSingleObject(s_stopEvent, 0) == WAIT_OBJECT_0) break;
		ULONG count = static_cast<ULONG>(
			min(static_cast<size_t>(kEntriesPerBatch), entries.size() - index));
		if (!SendGuardConfig(
				JDRV_GUARD_ACTION_ADD, &entries[index], count)) {
			if (currentLogger)
				currentLogger->LogError(
					L"Driver guard whitelist push failed at entry %u", index);
			break;
		}
		index += count;
		pushed += count;
	}
	return pushed;
}

DWORD WINAPI DriverGuardThreadProc(LPVOID)
{
	if (hKDrv == NULL || hKDrv == INVALID_HANDLE_VALUE) return 0;

	// 第一阶段：快速集合（自身驱动 + 已加载模块 + drivers 目录），先 ARM。
	if (!SendGuardConfig(JDRV_GUARD_ACTION_CLEAR, NULL, 0)) {
		if (currentLogger)
			currentLogger->LogError(L"Driver guard whitelist clear failed; guard not armed");
		return 0;
	}

	std::vector<JDRV_GUARD_ENTRY> fastEntries;
	std::set<std::wstring> seen;
	GuardTrustCounters counters = {};
	if (currentApp != NULL) {
		LPCWSTR ownDriver = currentApp->GetPartFullPath(PART_DRIVER);
		if (ownDriver != NULL && ownDriver[0] != L'\0') {
			// 自身驱动始终入白名单（自签证书，走 JiYu 例外规则）。
		TryBuildEntry(ownDriver, &fastEntries, &seen, true, nullptr, &counters);
		}
	}
	EnumerateLoadedDrivers(&fastEntries, &seen, &counters);

	wchar_t windowsDirectory[MAX_PATH] = {};
	std::wstring driversDir;
	if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) != 0UL) {
		// 32 位进程访问真实 System32 需要 Sysnative 别名。
		driversDir = std::wstring(windowsDirectory) + L"\\Sysnative\\drivers";
	}
	size_t scanned = 0;
	if (!driversDir.empty()) {
		EnumerateSysFiles(driversDir, false, &fastEntries, &seen,
			kMaxWhitelistEntries, &scanned, &counters);
	}

	ULONG fastPushed = PushBatches(fastEntries, 0);
	if (fastPushed == 0 && fastEntries.empty()) {
		if (currentLogger)
			currentLogger->LogError(
				L"Driver guard whitelist is empty; guard not armed");
		return 0;
	}
	if (!SendGuardConfig(JDRV_GUARD_ACTION_ARM, NULL, 0)) {
		if (currentLogger)
			currentLogger->LogError(L"Driver guard arm request failed");
		return 0;
	}
	if (currentLogger)
		currentLogger->LogInfo(
			L"Driver guard interception active: armed=1; whitelist active: entries=%u scanned=%u (stage 1)",
			fastPushed, static_cast<unsigned long>(fastEntries.size()));
	if (currentLogger)
		currentLogger->LogInfo(
			L"Driver trust path results: system-protected=%lu static-publisher=%lu dynamic-publisher=%lu md5=%lu rejected=%lu",
			counters.systemProtected, counters.staticPublisher,
			counters.dynamicPublisher, counters.md5, counters.rejected);

	// 第二阶段：DriverStore 递归，追加 PnP 驱动（上线后再插入的设备）。
	if (!driversDir.empty() &&
		WaitForSingleObject(s_stopEvent, 0) != WAIT_OBJECT_0) {
		std::wstring driverStore = std::wstring(windowsDirectory) +
			L"\\Sysnative\\DriverStore\\FileRepository";
		std::vector<JDRV_GUARD_ENTRY> storeEntries;
		size_t storeScanned = 0;
		EnumerateSysFiles(driverStore, true, &storeEntries, &seen,
			kMaxDriverStoreFiles, &storeScanned, &counters);
		ULONG storePushed = 0;
		size_t index = 0;
		while (index < storeEntries.size()) {
			if (WaitForSingleObject(s_stopEvent, 0) == WAIT_OBJECT_0) break;
			ULONG count = static_cast<ULONG>(
				min(static_cast<size_t>(kEntriesPerBatch),
					storeEntries.size() - index));
			if (!SendGuardConfig(
					JDRV_GUARD_ACTION_ADD, &storeEntries[index], count)) {
				if (currentLogger)
					currentLogger->LogError(
						L"Driver guard DriverStore push failed at entry %u", index);
				break;
			}
			index += count;
			storePushed += count;
		}
		if (currentLogger)
			currentLogger->LogInfo(
				L"Driver guard stage 2 done: DriverStore entries=%lu scanned=%u",
				storePushed, static_cast<unsigned long>(storeScanned));
	}

	// 收尾：回读内核统计，确认状态。
	if (WaitForSingleObject(s_stopEvent, 0) != WAIT_OBJECT_0 &&
		hKDrv != NULL && hKDrv != INVALID_HANDLE_VALUE) {
		JDRV_GUARD_STATUS status = {};
		DWORD returned = 0;
		if (DeviceIoControl(hKDrv, CTL_DRIVER_GUARD_QUERY,
				NULL, 0, &status, sizeof(status), &returned, NULL) &&
			returned >= sizeof(status)) {
			if (currentLogger)
				currentLogger->LogInfo(
					L"Driver guard status: armed=%lu entries=%lu evaluated=%I64u allowed=%I64u blocked=%I64u",
					status.armed, status.entryCount,
					status.evaluatedImages, status.allowedImages,
					status.blockedImages);
			if (currentLogger)
				currentLogger->LogInfo(
					L"Driver guard verification: interception=%s whitelist=%s",
					status.armed != 0UL ? L"effective" : L"inactive",
					status.armed != 0UL && status.entryCount != 0UL
						? L"effective" : L"inactive");
		}
		else if (currentLogger) {
			currentLogger->LogError(
				L"Driver guard verification failed: interception=unknown whitelist=unknown");
		}
	}
	return 0;
}

} // namespace

BOOL XStartDriverGuard()
{
	if (InterlockedCompareExchange(&s_running, 1L, 0L) != 0L) return TRUE;
	if (s_stopEvent == NULL) {
		s_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	}
	if (s_stopEvent == NULL) {
		InterlockedExchange(&s_running, 0L);
		return FALSE;
	}
	ResetEvent(s_stopEvent);
	s_thread = CreateThread(NULL, 0, DriverGuardThreadProc, NULL, 0, NULL);
	if (s_thread == NULL) {
		InterlockedExchange(&s_running, 0L);
		return FALSE;
	}
	return TRUE;
}

VOID XStopDriverGuard()
{
	if (InterlockedCompareExchange(&s_running, 0L, 1L) != 1L) return;
	if (s_stopEvent != NULL) SetEvent(s_stopEvent);
	if (s_thread != NULL) {
		WaitForSingleObject(s_thread, INFINITE);
		CloseHandle(s_thread);
		s_thread = NULL;
	}
	if (s_stopEvent != NULL) {
		CloseHandle(s_stopEvent);
		s_stopEvent = NULL;
	}
}
