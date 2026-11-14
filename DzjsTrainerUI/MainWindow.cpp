#include "stdafx.h"
#include "MainWindow.h"
#include "../DzjsTrainerUpdater/BuiltinUpdate.h"
#include "../WindowCaptureProtection.h"
#include "DriverServiceActions.h"
#include "resource.h"
#include "DzjsTrainerUI.h"
#include "../DzjsTrainerUpdater/DzjsTrainerUpdater.h"
#include "../DzjsTrainerUpdater/UpdaterIpc.h"
#include "../DzjsTrainer/AppPublic.h"

// 窗口默认从抓屏里排除（防极域/录屏）。排查 UI 视觉效果期间临时关掉，
// 这样截图工具能拍到真实界面；视觉调整完成后改回 1 即可恢复。
#ifndef DZJS_ENABLE_CAPTURE_EXCLUSION
#define DZJS_ENABLE_CAPTURE_EXCLUSION 1
#endif

// The About page shows the version of the binary that is actually running, read
// from its own file version resource. A compile-time constant drifts from
// DzjsTrainerVersion.rc whenever only one of the two is bumped.
std::wstring ReadOwnFileVersion()
{
	WCHAR path[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, path, _countof(path))) return std::wstring();
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size) return std::wstring();
	std::vector<BYTE> data(size);
	if (!GetFileVersionInfoW(path, 0, size, data.data())) return std::wstring();
	VS_FIXEDFILEINFO* info = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || !info)
		return std::wstring();
	WCHAR buffer[32] = {};
	// Three components, matching how the version is written elsewhere in the UI.
	swprintf_s(buffer, L"%u.%u.%u",
		HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
		HIWORD(info->dwFileVersionLS));
	return buffer;
}

// Cached because PaintAbout runs from the animation timer.
const std::wstring& AboutVersionLine()
{
	static const std::wstring line = [] {
		std::wstring version = ReadOwnFileVersion();
		if (version.empty()) version = L"\u672a\u77e5";
		return std::wstring(L"\u7248\u672c  ") + version;
	}();
	return line;
}

// Same reason as AboutVersionLine: PaintNavigation also runs from the animation
// timer. The sidebar used to carry a hard-coded "v1.7.6" that drifted far away
// from the real file version, so it is read from the running binary now.
const std::wstring& NavVersionLine()
{
	static const std::wstring line = [] {
		std::wstring version = ReadOwnFileVersion();
		if (version.empty()) version = L"\u672a\u77e5";
		return std::wstring(L"v") + version + L"  \u00b7  \u672c\u673a";
	}();
	return line;
}

#include "../DzjsTrainer/DriverLoader.h"
#include "../DzjsTrainer/DriverSignaturePolicy.h"
#include "../DzjsTrainer/SysHlp.h"
#include "../DzjsTrainer/NetUtils.h"
#include "../DzjsTrainer/StringSplit.h"
#include "../DzjsTrainer/StringHlp.h"
#include "../DzjsTrainer/JyUdpAttack.h"
#include "TeacherEndpoint/teacher_service.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <TlHelp32.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <shobjidl.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <array>
#include <vector>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <map>
#include <memory>
#include <random>
#include <regex>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

extern JTApp* currentApp;
extern MainWindow* currentMainWindow;
Logger* currentLogger = nullptr;
int screenWidth = 0;
int screenHeight = 0;

namespace {
constexpr wchar_t kUiAccessRestartArgument[] = L"--uiaccess-restarted";
constexpr UINT WM_NATIVE_REFRESH = WM_APP + 41;
constexpr UINT WM_BLOCKED_INJECTED_INPUT = WM_APP + 42;
constexpr UINT WM_AV_SCAN_FINISHED = WM_APP + 43;
// 更新探测在工作线程里完成后，用它把结果交回 UI 线程。
constexpr UINT WM_UPDATE_PROBE_DONE = WM_APP + 44;
// 内置更新的下载进度/结果回到 UI 线程用。
constexpr UINT WM_UPDATE_DOWNLOAD_STATE = WM_APP + 45;

struct UpdateDownloadContext {
	HWND window = nullptr;
	std::wstring url;
	std::wstring packagePath;
	std::wstring sha256;
	int lastReportedPercent = -100;
	unsigned long long lastReportedBytes = 0;
};

// 进度回调跑在下载线程里，只能把数据打包丢回 UI 线程。用堆上的小结构体
// 而不是把数值塞进 wParam/lParam —— 窗口不大也塞不下，读起来也更容易错。
struct UpdateProgressPayload {
	int percent;
	unsigned long long done;
	unsigned long long total;
};

// 弹窗里的两个按钮和普通按钮共用一套 hover/press 机制，编号避开页面上的。
constexpr int kUpdateDialogPrimary = 120;
constexpr int kUpdateDialogSecondary = 121;

std::wstring FormatByteSize(unsigned long long bytes)
{
	wchar_t buffer[64] = {};
	if (bytes >= 1024ull * 1024ull * 1024ull)
		swprintf_s(buffer, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024ull * 1024ull)
		swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024ull)
		swprintf_s(buffer, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
	else
		swprintf_s(buffer, L"%llu B", bytes);
	return buffer;
}

// 本进程里 WinHTTP 的 TLS 起不来：SendRequest 直接返回 SEC_E_SECPKG_NOT_FOUND
// (0x80090305)，换个独立进程跑同一段代码却正常。失败一次就本会话不再走进程内
// 路径，网络抓取统一交给子进程，省掉每次探测都先等一轮超时。
bool g_httpInProcessBroken = false;

void __stdcall UpdateProgressCallback(unsigned long long done, unsigned long long total, void* context)
{
	UpdateDownloadContext* data = static_cast<UpdateDownloadContext*>(context);
	if (!data || !data->window) return;
	const int percent = total ? static_cast<int>(done * 100 / total) : -1;
	// 百分比没变时也让字节数慢慢走，否则 5% 一跳的进度条看不出在动。
	// 这里按百分位 + 32 KB 双阈值节流，下载 3 MB 大约回几十次消息。
	if (percent >= 0 && percent == data->lastReportedPercent &&
		done < data->lastReportedBytes + 32768) return;
	data->lastReportedPercent = percent;
	data->lastReportedBytes = done;
	auto* payload = new (std::nothrow) UpdateProgressPayload{ percent, done, total };
	if (!payload) return;
	if (!PostMessageW(data->window, WM_UPDATE_DOWNLOAD_STATE, 1, reinterpret_cast<LPARAM>(payload))) {
		delete payload;
	}
}

DWORD WINAPI UpdateProbeThread(LPVOID parameter)
{
	UpdateProbeContext* context = static_cast<UpdateProbeContext*>(parameter);
	if (!context) return 0;
	// 新建线程会**继承创建线程的令牌**，包括模拟（impersonate）状态。主程序在提升
	// 置顶权限时曾模拟 SYSTEM，一旦这个线程继承了模拟令牌，它调用 CreateProcess
	// 就会失败并返回 ERROR_BAD_IMPERSONATION_LEVEL(1346)。
	// 本线程只做只读探测、不需要任何模拟身份，先撤掉。线程用完即退，无需恢复。
	RevertToSelf();
	context->startedAt = GetTickCount64();
	context->ok = false;
	// 和启动时那一次预热对照：启动成功、这时失败说明中间某个启动动作把
	// SSPI 弄坏了；两次都失败则是进程一出生就不行。两条线索指向完全不同的
	// 修法，所以必须分开记。
	const BOOL sspiNow = JUpdater_WarmupSspi();
	if (currentLogger) {
		// 两行对照：启动时那次（写不进日志文件，靠全局带过来）和现在这次。
		// 启动成功 + 现在失败 = 启动期有动作破坏了已建好的状态；
		// 两次都失败 = 进程一出生就没有可用 Schannel。0xFFFFFFFF 表示启动那次没跑。
		currentLogger->LogInfo(L"更新探测：SSPI 复测=%s（启动预热 schannel=0x%08X negotiate=0x%08X）",
			sspiNow ? L"可用" : L"不可用",
			g_sspiEarlySchannel, g_sspiEarlyNegotiate);
	}
	// 联网预检搬到这里。原来是 UI 线程上直接调 JUpdater_CheckInternet()，那个函数
	// 走的是 WinINet —— 而本进程里 WinINet 本身就是坏的（见下面 g_httpInProcessBroken），
	// 它卡多久、点完到出现任何反应就有多久的空白。耗时单独记一行，好和子进程那段区分。
	const ULONGLONG netCheckStart = GetTickCount64();
	const bool online = JUpdater_CheckInternet() != FALSE;
	if (currentLogger) {
		currentLogger->LogInfo(L"更新探测：联网预检=%d 耗时=%llu ms", online ? 1 : 0,
			static_cast<unsigned long long>(GetTickCount64() - netCheckStart));
	}
	if (!online) {
		context->offline = true;
		if (!PostMessageW(context->window, WM_UPDATE_PROBE_DONE, context->byUser ? 1 : 0,
			reinterpret_cast<LPARAM>(context))) {
			delete context;
		}
		return 0;
	}
	// 先在本进程里直接取清单（WinHTTP）。历史上这里只能让独立更新器代取，
	// 因为 WinINet 在 GUI 进程里会 ERROR_INTERNET_CANNOT_CONNECT；而那个更新器
	// 是控制台程序，代取时会 AllocConsole 弹一个黑窗口 —— 用户看到的就是它。
	// 清单获取改成 WinHTTP 后这条路不再需要子进程，取不到才退回更新器。
	// 线程本身不碰界面，只把结果投回 UI 线程。
	if (!g_httpInProcessBroken) {
		context->ok = JUpdater_FetchManifest(&context->manifest) != FALSE;
		if (!context->ok) g_httpInProcessBroken = true;
	}
	if (!context->ok) {
		// 进程内 TLS 不通（或已知不通）时才退回子进程；更新器已被要求不建控制台。
		context->viaSubprocess = true;
		context->ok = JUpdater_ProbeManifest(&context->manifest) != FALSE;
	}
	// 线程本身不碰界面，只把结果投回 UI 线程。
	if (!PostMessageW(context->window, WM_UPDATE_PROBE_DONE, context->byUser ? 1 : 0,
		reinterpret_cast<LPARAM>(context))) {
		delete context;
	}
	return 0;
}
MainWindow* inputGuardWindow = nullptr;
volatile LONG inputGuardNoticePending = 0;

bool IsCurrentProcessUiAccessEnabled()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	DWORD enabled = 0;
	DWORD returned = 0;
	const bool result = GetTokenInformation(token, TokenUIAccess, &enabled, sizeof(enabled), &returned) != FALSE && enabled != 0;
	CloseHandle(token);
	return result;
}

bool IsWindowActuallyTopmost(HWND window)
{
	return window != nullptr &&
		(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

struct UiAccessLaunchResult {
	bool success{};
	DWORD error{ ERROR_SUCCESS };
	std::wstring step;
};

bool EnableUiPrivilege(HANDLE token, LPCWSTR name, DWORD* errorCode = nullptr)
{
	if (errorCode) *errorCode = ERROR_SUCCESS;
	LUID luid{};
	if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
		if (errorCode) *errorCode = GetLastError();
		return false;
	}
	TOKEN_PRIVILEGES privileges{};
	privileges.PrivilegeCount = 1;
	privileges.Privileges[0].Luid = luid;
	privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	SetLastError(ERROR_SUCCESS);
	if (!AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) ||
		GetLastError() != ERROR_SUCCESS) {
		if (errorCode) *errorCode = GetLastError();
		return false;
	}
	return true;
}

bool TokenBelongsToLocalSystem(HANDLE token)
{
	DWORD required = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &required);
	if (required == 0) return false;
	std::vector<BYTE> buffer(required);
	if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required)) return false;
	BYTE systemSid[SECURITY_MAX_SID_SIZE]{};
	DWORD sidSize = sizeof(systemSid);
	if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &sidSize)) return false;
	const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
	return EqualSid(tokenUser->User.Sid, systemSid) != FALSE;
}

DWORD FindSystemTokenSourceProcess(DWORD currentSessionId)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	DWORD bestPid = 0;
	int bestRank = 0;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			DWORD sessionId = 0;
			ProcessIdToSessionId(entry.th32ProcessID, &sessionId);
			int rank = 0;
			if (_wcsicmp(entry.szExeFile, L"winlogon.exe") == 0)
				rank = sessionId == currentSessionId ? 40 : 30;
			else if (_wcsicmp(entry.szExeFile, L"services.exe") == 0)
				rank = sessionId == currentSessionId ? 20 : 10;
			if (rank > bestRank) {
				bestRank = rank;
				bestPid = entry.th32ProcessID;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return bestPid;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument)
{
	std::wstring quoted = L"\"";
	size_t backslashes = 0;
	for (wchar_t ch : argument) {
		if (ch == L'\\') {
			++backslashes;
			continue;
		}
		if (ch == L'\"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(ch);
		}
		else {
			quoted.append(backslashes, L'\\');
			quoted.push_back(ch);
		}
		backslashes = 0;
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

UiAccessLaunchResult RelaunchWithUiAccess()
{
	UiAccessLaunchResult result;
	HANDLE currentToken = nullptr;
	HANDLE sourceProcess = nullptr;
	HANDLE sourceToken = nullptr;
	HANDLE systemImpersonationToken = nullptr;
	HANDLE uiAccessToken = nullptr;
	bool impersonating = false;
	auto fail = [&](LPCWSTR step, DWORD error = GetLastError()) {
		result.step = step;
		result.error = error;
	};

	do {
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_PRIVILEGES, &currentToken)) {
			fail(L"OpenProcessToken(current)");
			break;
		}
		DWORD privilegeError = ERROR_SUCCESS;
		if (!EnableUiPrivilege(currentToken, SE_DEBUG_NAME, &privilegeError)) {
			fail(L"Enable SeDebugPrivilege", privilegeError);
			break;
		}
		DWORD sessionId = 0;
		if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
			fail(L"ProcessIdToSessionId");
			break;
		}
		const DWORD sourcePid = FindSystemTokenSourceProcess(sessionId);
		if (sourcePid == 0) {
			fail(L"Find SYSTEM token source", ERROR_NOT_FOUND);
			break;
		}
		sourceProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sourcePid);
		if (!sourceProcess) {
			fail(L"OpenProcess(SYSTEM source)");
			break;
		}
		if (!OpenProcessToken(sourceProcess, TOKEN_QUERY | TOKEN_DUPLICATE, &sourceToken)) {
			fail(L"OpenProcessToken(SYSTEM source)");
			break;
		}
		if (!TokenBelongsToLocalSystem(sourceToken)) {
			fail(L"Verify LocalSystem token", ERROR_INVALID_OWNER);
			break;
		}
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		if (!DuplicateTokenEx(sourceToken, MAXIMUM_ALLOWED, &attributes, SecurityImpersonation, TokenImpersonation, &systemImpersonationToken)) {
			fail(L"DuplicateTokenEx(SYSTEM impersonation)");
			break;
		}
		if (!ImpersonateLoggedOnUser(systemImpersonationToken)) {
			fail(L"ImpersonateLoggedOnUser(SYSTEM)");
			break;
		}
		impersonating = true;
		EnableUiPrivilege(systemImpersonationToken, SE_ASSIGNPRIMARYTOKEN_NAME);
		EnableUiPrivilege(systemImpersonationToken, SE_INCREASE_QUOTA_NAME);
		EnableUiPrivilege(systemImpersonationToken, SE_TCB_NAME);
		if (!DuplicateTokenEx(currentToken, MAXIMUM_ALLOWED, &attributes, SecurityImpersonation, TokenPrimary, &uiAccessToken)) {
			fail(L"DuplicateTokenEx(current primary)");
			break;
		}
		DWORD uiAccess = 1;
		if (!SetTokenInformation(uiAccessToken, TokenUIAccess, &uiAccess, sizeof(uiAccess))) {
			fail(L"SetTokenInformation(TokenUIAccess)");
			break;
		}
		DWORD verifiedUiAccess = 0;
		DWORD returned = 0;
		if (!GetTokenInformation(uiAccessToken, TokenUIAccess, &verifiedUiAccess, sizeof(verifiedUiAccess), &returned) || verifiedUiAccess == 0) {
			fail(L"Verify TokenUIAccess");
			break;
		}
		wchar_t path[32768]{};
		DWORD pathLength = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
		if (pathLength == 0 || pathLength >= ARRAYSIZE(path)) {
			fail(L"GetModuleFileNameW");
			break;
		}
		std::wstring executable(path, pathLength);
		std::wstring commandLine = QuoteCommandLineArgument(executable) + L" " + kUiAccessRestartArgument;
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		wchar_t desktop[] = L"winsta0\\default";
		startup.lpDesktop = desktop;
		PROCESS_INFORMATION process{};
		if (!CreateProcessAsUserW(uiAccessToken, executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
			CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) {
			fail(L"CreateProcessAsUserW(UIAccess)");
			break;
		}
		HANDLE launchedToken = nullptr;
		DWORD launchedUiAccess = 0;
		DWORD launchedReturned = 0;
		const bool openedLaunchedToken = OpenProcessToken(process.hProcess, TOKEN_QUERY, &launchedToken) != FALSE;
		const bool queriedLaunchedToken = openedLaunchedToken && GetTokenInformation(launchedToken, TokenUIAccess,
			&launchedUiAccess, sizeof(launchedUiAccess), &launchedReturned) != FALSE;
		const bool launchedWithUiAccess = queriedLaunchedToken && launchedUiAccess != 0;
		const DWORD verificationError = launchedWithUiAccess ? ERROR_SUCCESS :
			(queriedLaunchedToken ? ERROR_PRIVILEGE_NOT_HELD : GetLastError());
		if (launchedToken) CloseHandle(launchedToken);
		if (!launchedWithUiAccess) {
			TerminateProcess(process.hProcess, 1);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			fail(L"Verify launched process TokenUIAccess", verificationError);
			break;
		}
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		result.success = true;
	} while (false);
	if (impersonating) RevertToSelf();
	if (uiAccessToken) CloseHandle(uiAccessToken);
	if (systemImpersonationToken) CloseHandle(systemImpersonationToken);
	if (sourceToken) CloseHandle(sourceToken);
	if (sourceProcess) CloseHandle(sourceProcess);
	if (currentToken) CloseHandle(currentToken);
	return result;
}

using SetWindowBandFunction = BOOL(WINAPI*)(HWND, HWND, DWORD);

SetWindowBandFunction ResolveSetWindowBand()
{
	static SetWindowBandFunction function = [] {
		const HMODULE user32 = GetModuleHandleW(L"user32.dll");
		const FARPROC address = user32 ? GetProcAddress(user32, "SetWindowBand") : nullptr;
		return address ? std::bit_cast<SetWindowBandFunction>(address) : nullptr;
	}();
	return function;
}

// SetWindowBand 是未文档化 API，返回值约定不稳：可能是 BOOL（非 0 成功），
// 也可能是 NTSTATUS（0 才是成功）。按非 0 判成功会把成功误判成失败，
// 结果就是"窗口确实是超级置顶，状态却显示普通置顶"。
// 这里不再依赖返回值，改用 GetWindowBand 复核实际窗口带。
using GetWindowBandFunction = BOOL(WINAPI*)(HWND, PDWORD);

GetWindowBandFunction ResolveGetWindowBand()
{
	static GetWindowBandFunction function = [] {
		const HMODULE user32 = GetModuleHandleW(L"user32.dll");
		const FARPROC address = user32 ? GetProcAddress(user32, "GetWindowBand") : nullptr;
		return address ? std::bit_cast<GetWindowBandFunction>(address) : nullptr;
	}();
	return function;
}

bool ApplyHighestPermittedTopmost(HWND window, bool pinned, bool* uiAccessBandApplied = nullptr)
{
	if (uiAccessBandApplied) *uiAccessBandApplied = false;
	if (!window || !IsWindow(window)) return false;
	// 普通窗口的实际窗口带是 1（ZBID_DESKTOP），不是 0。
	constexpr DWORD kWindowBandDesktop = 1;
	constexpr DWORD kWindowBandUiAccess = 2;
	const bool uiAccessProcess = IsCurrentProcessUiAccessEnabled();

	// 先真正设成 topmost，再调 SetWindowBand。窗口带只对 topmost 窗口有意义，
	// 顺序反过来 win32k 会拒绝这次调用。
	if (!SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return false;
	if (pinned) BringWindowToTop(window);

	bool bandApplied = false;
	if (uiAccessProcess) {
		if (SetWindowBandFunction setWindowBand = ResolveSetWindowBand()) {
			// hWndInsertAfter 只接受 NULL（即 HWND_TOP）或一个有效窗口句柄。
			// 旧代码传 HWND_TOPMOST(-1) / HWND_NOTOPMOST(-2)，win32k 视为无效句柄，
			// 直接返回 FALSE —— 日志里 setResult=0、actual 一直停在 1 就是这个原因。
			const DWORD targetBand = pinned ? kWindowBandUiAccess : kWindowBandDesktop;
			GetWindowBandFunction getWindowBand = ResolveGetWindowBand();
			DWORD bandBefore = 0xFFFFFFFFUL;
			if (getWindowBand) getWindowBand(window, &bandBefore);
			SetLastError(ERROR_SUCCESS);
			const BOOL setResult = setWindowBand(window, HWND_TOP, targetBand);
			const DWORD setError = GetLastError();
			DWORD bandAfter = 0xFFFFFFFFUL;
			BOOL queryResult = FALSE;
			if (getWindowBand) {
				queryResult = getWindowBand(window, &bandAfter);
				if (queryResult != FALSE) bandApplied = bandAfter == targetBand;
			}
			if (currentLogger) {
				currentLogger->LogInfo(
					L"置顶带：pinned=%d target=%lu before=%lu setResult=%d setError=%lu query=%d after=%lu applied=%d",
					pinned ? 1 : 0, static_cast<unsigned long>(targetBand),
					static_cast<unsigned long>(bandBefore), static_cast<int>(setResult),
					static_cast<unsigned long>(setError), static_cast<int>(queryResult),
					static_cast<unsigned long>(bandAfter), bandApplied ? 1 : 0);
			}
		}
		else if (currentLogger) {
			currentLogger->LogWarn(L"置顶带：user32!SetWindowBand 未解析到");
		}
	}
	else if (currentLogger) {
		currentLogger->LogInfo(L"置顶带：当前进程不是 UIAccess，跳过窗口带设置");
	}

	// 显示文案的依据是「进程是否拿到 TokenUIAccess」，不是窗口带是否设置成功。
	// 实测 SetWindowBand 失败时超级置顶的效果依然在（UIAccess 进程的 topmost
	// 窗口本身就压得住普通 topmost），拿窗口带当判据会把成功显示成普通置顶。
	if (uiAccessBandApplied) *uiAccessBandApplied = uiAccessProcess && pinned;
	if (currentLogger && uiAccessProcess) {
		currentLogger->LogInfo(L"置顶带：窗口带 applied=%d（仅诊断用，不参与超级置顶判定）",
			bandApplied ? 1 : 0);
	}
	return true;
}
constexpr UINT WM_COPYGLOBALDATA_COMPAT = 0x0049;
constexpr UINT TIMER_ANIMATION = 20;
// 服务轮询和动画分开：轮询 5 次/秒足够，动画只在有东西在动时才跑 60 帧。
constexpr UINT TIMER_SERVICE_POLL = 23;
constexpr UINT TIMER_TRAY_REPAIR = 21;
constexpr UINT TIMER_RERUN = 21;
constexpr UINT TIMER_AUTO_SHUT = 22;
// 更新替换完成后停一下再退出，让「更新完成」这一帧留得住。
constexpr UINT TIMER_UPDATE_RESTART = 24;
constexpr int CMD_POWER_SHUTDOWN = 41001;
constexpr int CMD_POWER_RESTART = 41002;
constexpr int CMD_POWER_EXIT = 41003;
constexpr int IDC_NET_IP = 42001;
constexpr int IDC_NET_PORT = 42002;
constexpr int IDC_NET_MESSAGE = 42003;
constexpr int IDC_NET_COMMAND = 42004;
constexpr int IDC_NET_DETECT_PORT = 42005;
constexpr int IDC_NET_SEND_MESSAGE = 42006;
constexpr int IDC_NET_SEND_COMMAND = 42007;
constexpr int IDC_NET_SHUTDOWN = 42008;
constexpr int IDC_NET_REBOOT = 42009;
constexpr int IDC_NET_SCAN = 42010;
constexpr int IDC_NET_RESULTS = 42011;
constexpr int IDC_NET_TEACHER = 42012;
constexpr int IDC_TEACHER_REFRESH = 43001;
constexpr int IDC_TEACHER_CHAT_INPUT = 43002;
constexpr int IDC_TEACHER_CHAT_SEND = 43007;
constexpr int IDC_TEACHER_BLACK = 43003;
constexpr int IDC_TEACHER_UNLOCK = 43004;
constexpr int IDC_TEACHER_SHUTDOWN = 43005;
constexpr int IDC_TEACHER_REBOOT = 43006;
constexpr int IDC_TEACHER_VIEW = 43008;
constexpr int IDC_TEACHER_STUDENTS = 43009;
constexpr int IDC_ADV_HOTKEY_FAKE = 44001;
constexpr int IDC_ADV_HOTKEY_SHOW = 44002;
constexpr int IDC_ADV_INTERVAL = 44003;
constexpr int IDC_AV_PROMPT_EDIT = 45001;
constexpr int IDC_AV_PROMPT_OK = 45002;
constexpr int IDC_AV_PROMPT_CANCEL = 45003;
constexpr int IDC_AV_PROCESS_LIST = 45101;
constexpr int IDC_AV_PROCESS_REFRESH = 45102;
constexpr int IDC_AV_PROCESS_OK = 45103;
constexpr int IDC_AV_PROCESS_CANCEL = 45104;
constexpr int IDC_EXIT_CONFIRM_EDIT = 46001;
constexpr int IDC_EXIT_CONFIRM_OK = 46002;
constexpr int IDC_EXIT_CONFIRM_CANCEL = 46003;

HANDLE materialIconFontHandle = nullptr;
DWORD materialIconFontCount = 0;
std::unique_ptr<PrivateFontCollection> materialIconCollection;
std::unique_ptr<FontFamily> materialIconFamily;
std::map<int, std::unique_ptr<Font>> materialIconFonts;
std::unique_ptr<StringFormat> materialIconFormat;
std::array<bool, 13> materialIconGlyphs{};
bool materialIconFontUsable = false;

const wchar_t* MaterialIconGlyph(int icon);
void UnloadMaterialIconFont();

bool MaterialIconGlyphExists(const wchar_t* glyph)
{
	if (!glyph || !*glyph) return false;
	HDC dc = CreateCompatibleDC(nullptr);
	if (!dc) return false;
	HFONT font = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, DEFAULT_PITCH, L"Material Icons");
	if (!font) {
		DeleteDC(dc);
		return false;
	}
	HGDIOBJ old = SelectObject(dc, font);
	WORD glyphIndex = 0;
	const DWORD result = GetGlyphIndicesW(dc, glyph, 1, &glyphIndex, GGI_MARK_NONEXISTING_GLYPHS);
	SelectObject(dc, old);
	DeleteObject(font);
	DeleteDC(dc);
	return result != GDI_ERROR && glyphIndex != 0 && glyphIndex != 0xFFFF;
}

bool LoadMaterialIconFont(HINSTANCE instance)
{
	if (materialIconFontHandle) return true;
	HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(153), RT_RCDATA);
	if (!resource) return false;
	HGLOBAL loaded = LoadResource(instance, resource);
	DWORD size = SizeofResource(instance, resource);
	void* data = loaded ? LockResource(loaded) : nullptr;
	if (!data || size == 0) return false;
	materialIconFontHandle = AddFontMemResourceEx(data, size, nullptr, &materialIconFontCount);
	if (!materialIconFontHandle || materialIconFontCount == 0) return false;
	materialIconCollection = std::make_unique<PrivateFontCollection>();
	materialIconFamily = std::make_unique<FontFamily>();
	INT familyCount = 0;
	if (!materialIconCollection || !materialIconFamily ||
		materialIconCollection->AddMemoryFont(data, static_cast<INT>(size)) != Ok ||
		materialIconCollection->GetFamilies(1, materialIconFamily.get(), &familyCount) != Ok ||
		familyCount != 1 || materialIconFamily->GetLastStatus() != Ok) {
		UnloadMaterialIconFont();
		return false;
	}
	for (int i = 0; i < static_cast<int>(materialIconGlyphs.size()); ++i)
		materialIconGlyphs[i] = MaterialIconGlyphExists(MaterialIconGlyph(i));
	materialIconFontUsable = std::any_of(materialIconGlyphs.begin(), materialIconGlyphs.end(),
		[](bool available) { return available; });
	if (materialIconFontUsable) {
		materialIconFormat = std::make_unique<StringFormat>();
		materialIconFormat->SetAlignment(StringAlignmentCenter);
		materialIconFormat->SetLineAlignment(StringAlignmentCenter);
	}
	return materialIconFontUsable;
}

void UnloadMaterialIconFont()
{
	materialIconFonts.clear();
	materialIconFormat.reset();
	materialIconFamily.reset();
	materialIconCollection.reset();
	materialIconFontUsable = false;
	materialIconGlyphs.fill(false);
	if (materialIconFontHandle) {
		RemoveFontMemResourceEx(materialIconFontHandle);
		materialIconFontHandle = nullptr;
		materialIconFontCount = 0;
	}
}

// 原生 owner-draw 按钮不会向父窗口汇报悬停，各自挂一个子类化过程记录状态，
// 再让 DrawNativeButton 按这个标志混色 —— 否则网络页的按钮是全界面唯一没有悬停的。
std::unordered_map<HWND, bool>& NativeButtonHoverMap()
{
	static std::unordered_map<HWND, bool> map;
	return map;
}

LRESULT CALLBACK NativeButtonHoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR, DWORD_PTR)
{
	switch (message) {
	case WM_MOUSEMOVE: {
		bool& hovered = NativeButtonHoverMap()[window];
		if (!hovered) {
			hovered = true;
			InvalidateRect(window, nullptr, FALSE);
		}
		TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
		TrackMouseEvent(&track);
		return 0;
	}
	case WM_MOUSELEAVE:
		NativeButtonHoverMap()[window] = false;
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	case WM_NCDESTROY:
		NativeButtonHoverMap().erase(window);
		RemoveWindowSubclass(window, NativeButtonHoverProc, 0);
		break;
	}
	return DefSubclassProc(window, message, wParam, lParam);
}

void EnableNativeButtonHover(HWND button)
{
	if (!button) return;
	SetWindowSubclass(button, NativeButtonHoverProc, 0, 0);
}

COLORREF BlendColorRef(COLORREF from, COLORREF to, double amount)
{
	auto mix = [amount](int a, int b) {
		return static_cast<int>(a + (b - a) * amount);
	};
	return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
		mix(GetBValue(from), GetBValue(to)));
}

void DrawNativeButton(const DRAWITEMSTRUCT* item, HFONT font, bool dark, bool hover = false)
{
	const bool pressed = (item->itemState & ODS_SELECTED) != 0;
	const int id = static_cast<int>(item->CtlID);
	COLORREF fill = RGB(219, 238, 232);
	COLORREF textColor = RGB(0, 81, 72);
	if (id == IDC_NET_SEND_MESSAGE || id == IDC_NET_SEND_COMMAND) {
		fill = pressed ? RGB(0, 81, 72) : RGB(0, 107, 95);
		textColor = RGB(255, 255, 255);
	}
	else if (id == IDC_NET_SHUTDOWN || id == IDC_NET_REBOOT) {
		fill = pressed ? RGB(238, 220, 178) : RGB(255, 243, 205);
		textColor = RGB(91, 66, 0);
	}
	if (dark) {
		fill = RGB(43, 76, 69);
		textColor = RGB(177, 239, 228);
		if (id == IDC_NET_SEND_MESSAGE || id == IDC_NET_SEND_COMMAND) {
			fill = pressed ? RGB(95, 186, 171) : RGB(130, 213, 199);
			textColor = RGB(0, 55, 49);
		}
		else if (id == IDC_NET_SHUTDOWN || id == IDC_NET_REBOOT) {
			fill = RGB(64, 53, 28);
			textColor = RGB(255, 220, 141);
		}
	}
	if (!pressed) {
		// 悬停往深一档走；主色按钮压暗、浅色按钮加深一档，方向一致。
		if (id == IDC_NET_SEND_MESSAGE || id == IDC_NET_SEND_COMMAND) {
			fill = hover ? BlendColorRef(fill, dark ? RGB(110, 200, 186) : RGB(0, 86, 77), 0.65) : fill;
		}
		else if (id == IDC_NET_SHUTDOWN || id == IDC_NET_REBOOT) {
			fill = hover ? BlendColorRef(fill, dark ? RGB(82, 68, 36) : RGB(252, 231, 176), 0.65) : fill;
		}
		else {
			fill = hover ? BlendColorRef(fill, dark ? RGB(56, 96, 87) : RGB(203, 228, 219), 0.65) : fill;
		}
	}
	RECT rect = item->rcItem;
	HBRUSH brush = CreateSolidBrush(fill);
	HPEN pen = CreatePen(PS_NULL, 0, 0);
	HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
	HGDIOBJ oldPen = SelectObject(item->hDC, pen);
	RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, 22, 22);
	HGDIOBJ oldFont = SelectObject(item->hDC, font);
	SetBkMode(item->hDC, TRANSPARENT);
	SetTextColor(item->hDC, textColor);
	wchar_t text[64]{};
	GetWindowTextW(item->hwndItem, text, _countof(text));
	DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(item->hDC, oldFont);
	SelectObject(item->hDC, oldBrush);
	SelectObject(item->hDC, oldPen);
	DeleteObject(brush);
	DeleteObject(pen);
}

const wchar_t* kNavLabels[] = {
	L"\u72b6\u6001\u603b\u89c8", L"\u9632\u62a4\u7b56\u7565", L"\u66ff\u6362\u753b\u9762", L"\u7279\u5f81\u626b\u63cf", L"\u9ad8\u7ea7\u914d\u7f6e", L"\u4f7f\u7528\u5e2e\u52a9", L"\u7f51\u7edc\u5de5\u5177", L"\u8bbe\u5907\u8bca\u65ad", L"\u8fd0\u884c\u65e5\u5fd7", L"\u5173\u4e8e\u8f6f\u4ef6"
};
const wchar_t* kPageTitles[] = {
	L"\u72b6\u6001\u603b\u89c8", L"\u9632\u62a4\u7b56\u7565", L"\u66ff\u6362\u753b\u9762", L"\u7279\u5f81\u7801\u626b\u63cf", L"\u9ad8\u7ea7\u914d\u7f6e", L"\u4f7f\u7528\u5e2e\u52a9", L"\u7f51\u7edc\u5de5\u5177", L"\u8bbe\u5907\u8bca\u65ad", L"\u8fd0\u884c\u65e5\u5fd7", L"\u5173\u4e8e Dzjs Trainer"
};
const wchar_t* kToggleLabels[] = {
	L"\u62e6\u622a\u8fdc\u7a0b\u8fd0\u884c\u7a0b\u5e8f", L"\u76f4\u63a5\u62d2\u7edd\u8fdc\u7a0b\u547d\u4ee4", L"\u5141\u8bb8\u5e7f\u64ad\u7a97\u53e3\u7f6e\u9876", L"\u963b\u6b62\u8fdc\u7a0b\u7ed3\u675f\u8fdb\u7a0b",
	L"\u5141\u8bb8\u6559\u5e08\u67e5\u770b\u5c4f\u5e55", L"\u963b\u6b62\u8fdc\u7a0b\u5173\u95ed\u7a97\u53e3", L"\u5141\u8bb8\u6559\u5e08\u63a7\u5236\u7535\u8111",
	L"\u89c6\u9891\u6d41\u4fdd\u62a4"
};

Color C(bool dark, BYTE r, BYTE g, BYTE b, BYTE dr, BYTE dg, BYTE db, BYTE a = 255) {
	return Color(a, dark ? dr : r, dark ? dg : g, dark ? db : b);
}

Color Blend(const Color& from, const Color& to, float amount) {
	amount = std::max(0.0f, std::min(1.0f, amount));
	auto mix = [amount](BYTE a, BYTE b) {
		return static_cast<BYTE>(a + (b - a) * amount);
	};
	return Color(mix(from.GetA(), to.GetA()), mix(from.GetR(), to.GetR()),
		mix(from.GetG(), to.GetG()), mix(from.GetB(), to.GetB()));
}

void BuildRoundPath(GraphicsPath& path, const RectF& rect, float radius) {
	const float d = radius * 2.0f;
	path.AddArc(rect.X, rect.Y, d, d, 180, 90);
	path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
	path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
	path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
	path.CloseFigure();
}

void FillRound(Graphics& g, const RectF& rect, float radius, const Color& color) {
	SolidBrush brush(color);
	GraphicsPath path;
	BuildRoundPath(path, rect, radius);
	g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, const RectF& rect, float radius, const Color& color, float width = 1.0f) {
	Pen pen(color, width);
	GraphicsPath path;
	BuildRoundPath(path, rect, radius);
	g.DrawPath(&pen, &path);
}

void FillRound(Graphics& g, const RectF& rect, float radius, Brush& brush) {
	GraphicsPath path;
	BuildRoundPath(path, rect, radius);
	g.FillPath(&brush, &path);
}

// 卡片投影。GDI+ 没有高斯模糊，用三层低透明度的外扩圆角矩形叠出柔和过渡。
// 实测单次 FillRound 约 0.01 ms，一页十来张卡片只多 0.3 ms，换来整页的立体感。
void SoftShadow(Graphics& g, const RectF& rect, float radius, bool dark, float strength = 1.0f)
{
	static const BYTE lightAlpha[3] = { 13, 8, 4 };
	static const BYTE darkAlpha[3] = { 30, 18, 9 };
	for (int layer = 2; layer >= 0; --layer) {
		const float spread = (layer + 1) * 2.5f;
		const int raw = static_cast<int>((dark ? darkAlpha[layer] : lightAlpha[layer]) * strength);
		if (raw <= 0) continue;
		FillRound(g,
			RectF(rect.X - spread, rect.Y - spread + 2.0f,
				rect.Width + spread * 2.0f, rect.Height + spread * 2.0f),
			radius + spread, Color(static_cast<BYTE>(raw > 255 ? 255 : raw), 0, 0, 0));
	}
}

const wchar_t* MaterialIconGlyph(int icon)
{
	// Google Material Icons code points. The hand-drawn implementation below
	// remains the fallback for systems where the embedded font cannot load.
	switch (icon) {
	case 0: return L"\uE871"; // dashboard
	case 1: return L"\uE32A"; // security
	case 2: return L"\uE3D7"; // list_alt
	case 3: return L"\uE88E"; // info
	case 4: return L"\uE0F3"; // push_pin
	case 5: return L"\uE30C"; // terminal
	case 6: return L"\uE429"; // tune
	case 7: return L"\uE8AC"; // power_settings_new
	case 8: return L"\uE518"; // light_mode
	case 9: return L"\uE51C"; // dark_mode
	// 更新弹窗用的三个图标。
	case 10: return L"\uE2C4"; // file_download
	case 11: return L"\uE86C"; // check_circle
	case 12: return L"\uE002"; // warning
	default: return nullptr;
	}
}

void DrawIcon(Graphics& g, int icon, float x, float y, const Color& color, float size = 20.0f) {
	const wchar_t* glyph = MaterialIconGlyph(icon);
	if (materialIconFontUsable && glyph && icon >= 0 && icon < static_cast<int>(materialIconGlyphs.size()) && materialIconGlyphs[icon]) {
		const int pixelSize = std::max(1, static_cast<int>(std::lround(size)));
		auto found = materialIconFonts.find(pixelSize);
		if (found == materialIconFonts.end()) {
			auto font = std::make_unique<Font>(materialIconFamily.get(), static_cast<REAL>(pixelSize), FontStyleRegular, UnitPixel);
			if (font->GetLastStatus() == Ok)
				found = materialIconFonts.emplace(pixelSize, std::move(font)).first;
		}
		if (found != materialIconFonts.end() && materialIconFormat) {
			SolidBrush brush(color);
			if (g.DrawString(glyph, -1, found->second.get(), RectF(x, y, size, size), materialIconFormat.get(), &brush) == Ok)
				return;
		}
	}
	Pen pen(color, 1.8f);
	pen.SetStartCap(LineCapRound);
	pen.SetEndCap(LineCapRound);
	pen.SetLineJoin(LineJoinRound);
	const float s = size / 20.0f;
	auto X = [x, s](float value) { return x + value * s; };
	auto Y = [y, s](float value) { return y + value * s; };
	switch (icon) {
	case 0: // dashboard
		g.DrawRectangle(&pen, X(2), Y(2), 6 * s, 6 * s); g.DrawRectangle(&pen, X(12), Y(2), 6 * s, 6 * s);
		g.DrawRectangle(&pen, X(2), Y(12), 6 * s, 6 * s); g.DrawRectangle(&pen, X(12), Y(12), 6 * s, 6 * s); break;
	case 1: { // shield
		PointF points[] = { {X(10),Y(1)}, {X(17),Y(4)}, {X(16),Y(12)}, {X(10),Y(19)}, {X(4),Y(12)}, {X(3),Y(4)} };
		g.DrawPolygon(&pen, points, 6); g.DrawLine(&pen, X(7), Y(10), X(9), Y(12)); g.DrawLine(&pen, X(9), Y(12), X(13), Y(8)); break;
	}
	case 2: // logs
		g.DrawRectangle(&pen, X(3), Y(2), 14 * s, 16 * s);
		for (int i = 0; i < 3; ++i) {
			const float row = static_cast<float>(i * 4);
			g.DrawLine(&pen, X(7), Y(6.0f + row), X(14), Y(6.0f + row));
			g.DrawEllipse(&pen, X(5), Y(5.0f + row), 1 * s, 1 * s);
		} break;
	case 3: // info
		g.DrawEllipse(&pen, X(2), Y(2), 16 * s, 16 * s); g.DrawLine(&pen, X(10), Y(9), X(10), Y(14)); g.DrawEllipse(&pen, X(9.5f), Y(5), 1 * s, 1 * s); break;
	case 4: // pin
		g.DrawLine(&pen, X(5), Y(4), X(15), Y(14)); g.DrawLine(&pen, X(12), Y(3), X(17), Y(8));
		g.DrawLine(&pen, X(8), Y(9), X(3), Y(14)); g.DrawLine(&pen, X(8), Y(9), X(12), Y(13)); g.DrawLine(&pen, X(8), Y(13), X(4), Y(17)); break;
	case 5: // process
		g.DrawRectangle(&pen, X(3), Y(3), 14 * s, 14 * s); g.DrawLine(&pen, X(7), Y(10), X(13), Y(10)); break;
	case 6: // sliders
		g.DrawLine(&pen, X(3), Y(5), X(17), Y(5)); g.DrawEllipse(&pen, X(6), Y(3), 4 * s, 4 * s);
		g.DrawLine(&pen, X(3), Y(15), X(17), Y(15)); g.DrawEllipse(&pen, X(11), Y(13), 4 * s, 4 * s); break;
	case 7: // power
		g.DrawArc(&pen, X(3), Y(3), 14 * s, 14 * s, -55, 290); g.DrawLine(&pen, X(10), Y(1), X(10), Y(10)); break;
	case 8: // sun
		g.DrawEllipse(&pen, X(6), Y(6), 8 * s, 8 * s);
		g.DrawLine(&pen, X(10), Y(1), X(10), Y(4)); g.DrawLine(&pen, X(10), Y(16), X(10), Y(19));
		g.DrawLine(&pen, X(1), Y(10), X(4), Y(10)); g.DrawLine(&pen, X(16), Y(10), X(19), Y(10)); break;
	case 9: // moon
	{
		GraphicsPath moon;
		moon.StartFigure();
		moon.AddBezier(PointF(X(12), Y(2)), PointF(X(3), Y(3)), PointF(X(3), Y(17)), PointF(X(12), Y(18)));
		moon.AddBezier(PointF(X(12), Y(18)), PointF(X(8), Y(14)), PointF(X(8), Y(6)), PointF(X(12), Y(2)));
		moon.CloseFigure();
		SolidBrush fill(color);
		g.FillPath(&fill, &moon);
		break;
	}
	// 以下三个只给更新弹窗用：字体没加载成功时才走这里。
	case 10: // download
		g.DrawLine(&pen, X(10), Y(2), X(10), Y(13));
		g.DrawLine(&pen, X(10), Y(13), X(6), Y(9));
		g.DrawLine(&pen, X(10), Y(13), X(14), Y(9));
		g.DrawLine(&pen, X(3), Y(16), X(17), Y(16));
		break;
	case 11: // check_circle
		g.DrawEllipse(&pen, X(2), Y(2), 16 * s, 16 * s);
		g.DrawLine(&pen, X(6), Y(10), X(9), Y(13));
		g.DrawLine(&pen, X(9), Y(13), X(14), Y(7));
		break;
	case 12: // warning
		g.DrawEllipse(&pen, X(2), Y(2), 16 * s, 16 * s);
		g.DrawLine(&pen, X(10), Y(6), X(10), Y(12));
		g.DrawEllipse(&pen, X(9.5f), Y(14), 1 * s, 1 * s);
		break;
	default: g.DrawEllipse(&pen, X(3), Y(3), 14 * s, 14 * s); break;
	}
}

// GDI+ 的 Font 构造要走一次字体库解析，单次约 0.15 ms。一帧里几十次 Text()
// 累计起来就是十几毫秒 —— 实测 60 次绘制从 9.2 ms 降到 1.8 ms。按
// (像素字号, 字形) 缓存，绘制线程只有一个，不需要加锁。
std::unordered_map<int, std::unique_ptr<Font>>& TextFontCache()
{
	static std::unordered_map<int, std::unique_ptr<Font>> cache;
	return cache;
}

Font* ResolveTextFont(float size, FontStyle style)
{
	const int pixelSize = std::max(1, static_cast<int>(std::lround(size)));
	const int key = pixelSize * 16 + static_cast<int>(style);
	auto& cache = TextFontCache();
	auto found = cache.find(key);
	if (found != cache.end()) return found->second.get();
	auto font = std::make_unique<Font>(L"Microsoft YaHei UI", static_cast<REAL>(pixelSize), style, UnitPixel);
	if (font->GetLastStatus() != Ok) return nullptr;
	Font* raw = font.get();
	cache.emplace(key, std::move(font));
	return raw;
}

// 静态缓存必须在 GdiplusShutdown 之前清空，否则进程退出时 Font 析构会碰到
// 已经拆掉的 GDI+ 状态而崩溃（和析构函数里那段注释是同一类问题）。
void ReleaseTextFontCache()
{
	TextFontCache().clear();
}

// StringFormat 同样有析构顺序问题，这里故意 new 出来不释放：进程退出时不需要
// 回收，也就不会在 GdiplusShutdown 之后被析构。
StringFormat* ResolveTextFormat(StringAlignment align)
{
	struct FormatSet {
		StringFormat nearFormat;
		StringFormat centerFormat;
		StringFormat farFormat;
		FormatSet()
		{
			nearFormat.SetAlignment(StringAlignmentNear);
			centerFormat.SetAlignment(StringAlignmentCenter);
			farFormat.SetAlignment(StringAlignmentFar);
			for (StringFormat* format : { &nearFormat, &centerFormat, &farFormat }) {
				format->SetLineAlignment(StringAlignmentNear);
				format->SetTrimming(StringTrimmingEllipsisCharacter);
			}
		}
	};
	static FormatSet* formats = new FormatSet();
	if (align == StringAlignmentCenter) return &formats->centerFormat;
	if (align == StringAlignmentFar) return &formats->farFormat;
	return &formats->nearFormat;
}

void Text(Graphics& g, const wchar_t* value, float x, float y, float width, float height,
	float size, FontStyle style, const Color& color, StringAlignment align = StringAlignmentNear) {
	Font* font = ResolveTextFont(size, style);
	if (!font) return;
	SolidBrush brush(color);
	g.DrawString(value ? value : L"", -1, font, RectF(x, y, width, height), ResolveTextFormat(align), &brush);
}

bool Hit(const RECT& rect, POINT point) {
	return PtInRect(&rect, point) != FALSE;
}

// 单行 + 省略号。日志里一条记录可能很长，普通 Text 会折行，
// 第二行落到 20px 行高之外被裁成半截字。
StringFormat* ResolveTextFormatSingleLine()
{
	static StringFormat* format = [] {
		auto created = new StringFormat();
		created->SetAlignment(StringAlignmentNear);
		created->SetLineAlignment(StringAlignmentNear);
		created->SetTrimming(StringTrimmingEllipsisCharacter);
		created->SetFormatFlags(StringFormatFlagsNoWrap);
		return created;
	}();
	return format;
}

void TextSingleLine(Graphics& g, const wchar_t* value, float x, float y, float width, float height,
	float size, FontStyle style, const Color& color)
{
	Font* font = ResolveTextFont(size, style);
	if (!font) return;
	SolidBrush brush(color);
	g.DrawString(value ? value : L"", -1, font, RectF(x, y, width, height), ResolveTextFormatSingleLine(), &brush);
}

constexpr wchar_t kAvPromptClass[] = L"DzjsTrainerAvTextPrompt";
struct AvTextPromptContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HWND edit = nullptr;
	std::wstring title;
	std::wstring prompt;
	std::wstring value;
	bool accepted = false;
};

LRESULT CALLBACK AvTextPromptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	AvTextPromptContext* context = reinterpret_cast<AvTextPromptContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		context = static_cast<AvTextPromptContext*>(create->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_CREATE:
		CreateWindowExW(0, L"STATIC", context->prompt.c_str(), WS_CHILD | WS_VISIBLE,
			18, 16, 444, 28, window, nullptr, GetModuleHandleW(nullptr), nullptr);
		context->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", context->value.c_str(),
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
			18, 50, 444, 170, window, reinterpret_cast<HMENU>(IDC_AV_PROMPT_EDIT), GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			276, 230, 88, 30, window, reinterpret_cast<HMENU>(IDC_AV_PROMPT_OK), GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			374, 230, 88, 30, window, reinterpret_cast<HMENU>(IDC_AV_PROMPT_CANCEL), GetModuleHandleW(nullptr), nullptr);
		SetFocus(context->edit);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_AV_PROMPT_OK) {
			wchar_t buffer[2048] = {};
			GetWindowTextW(context->edit, buffer, _countof(buffer));
			context->value = buffer;
			context->accepted = true;
			DestroyWindow(window);
			return 0;
		}
		if (LOWORD(wParam) == IDC_AV_PROMPT_CANCEL) { DestroyWindow(window); return 0; }
		break;
	case WM_CLOSE: DestroyWindow(window); return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

bool PromptAvText(HWND owner, LPCWSTR title, LPCWSTR prompt, std::wstring* value)
{
	if (!value) return false;
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW cls = {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = AvTextPromptProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		cls.lpszClassName = kAvPromptClass;
		if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		registered = true;
	}
	AvTextPromptContext context;
	context.owner = owner;
	context.title = title ? title : L"输入";
	context.prompt = prompt ? prompt : L"请输入：";
	context.value = *value;
	RECT work = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int x = work.left + ((work.right - work.left) - 480) / 2;
	const int y = work.top + ((work.bottom - work.top) - 305) / 2;
	HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST, kAvPromptClass,
		context.title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		x, y, 480, 305, owner, nullptr, GetModuleHandleW(nullptr), &context);
	if (!window) return false;
	if (owner && IsWindowEnabled(owner)) EnableWindow(owner, FALSE);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	MSG message = {};
	while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	if (owner && IsWindow(owner)) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
	if (!context.accepted) return false;
	*value = context.value;
	return true;
}

constexpr wchar_t kExitConfirmClass[] = L"DzjsTrainerExitConfirm";

struct ExitConfirmContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HDESK desktop = nullptr;
	HANDLE readyEvent = nullptr;
	DWORD desktopFailure = ERROR_SUCCESS;
	std::unique_ptr<Gdiplus::Bitmap> blurredWallpaper;
	std::wstring wallpaperPath;
	int panelX = 0;
	int panelY = 0;
	HWND edit = nullptr;
	HFONT font = nullptr;
	HBRUSH editBrush = nullptr;
	WNDPROC originalEditProc = nullptr;
	bool rawInputRegistered = false;
	std::wstring challenge;
	std::wstring value;
	bool darkMode = false;
	bool accepted = false;
	bool sawPhysicalKeyboard = false;
	bool injectedInput = false;
	bool invalidInput = false;
	bool updatingEdit = false;
};

std::unique_ptr<Bitmap> BuildExitWallpaper(LPCWSTR path)
{
	if (!path || path[0] == L'\0') return {};
	std::unique_ptr<Image> source(Image::FromFile(path, FALSE));
	if (!source || source->GetLastStatus() != Ok || source->GetWidth() == 0 || source->GetHeight() == 0)
		return {};
	const int targetWidth = 240;
	const int targetHeight = 135;
	std::unique_ptr<Bitmap> result = std::make_unique<Bitmap>(targetWidth, targetHeight, PixelFormat32bppARGB);
	if (!result || result->GetLastStatus() != Ok) return {};
	Graphics graphics(result.get());
	graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
	graphics.Clear(Color(255, 25, 32, 29));
	const REAL sourceAspect = static_cast<REAL>(source->GetWidth()) / static_cast<REAL>(source->GetHeight());
	const REAL targetAspect = static_cast<REAL>(targetWidth) / static_cast<REAL>(targetHeight);
	REAL drawWidth = static_cast<REAL>(targetWidth);
	REAL drawHeight = static_cast<REAL>(targetHeight);
	if (sourceAspect > targetAspect) drawWidth = drawHeight * sourceAspect;
	else drawHeight = drawWidth / sourceAspect;
	const REAL drawX = (static_cast<REAL>(targetWidth) - drawWidth) / 2.0f;
	const REAL drawY = (static_cast<REAL>(targetHeight) - drawHeight) / 2.0f;
	graphics.DrawImage(source.get(), drawX, drawY, drawWidth, drawHeight);

	BitmapData data{};
	const Rect bounds(0, 0, targetWidth, targetHeight);
	if (result->LockBits(&bounds, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok)
		return result;
	std::vector<DWORD> sourcePixels(static_cast<size_t>(targetWidth) * targetHeight);
	for (int y = 0; y < targetHeight; ++y) {
		const auto* row = reinterpret_cast<const DWORD*>(reinterpret_cast<const BYTE*>(data.Scan0) + y * data.Stride);
		std::copy(row, row + targetWidth, sourcePixels.begin() + static_cast<size_t>(y) * targetWidth);
	}
	const int radius = 4;
	for (int y = 0; y < targetHeight; ++y) {
		auto* row = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(data.Scan0) + y * data.Stride);
		for (int x = 0; x < targetWidth; ++x) {
			unsigned int blue = 0, green = 0, red = 0, alpha = 0, count = 0;
			for (int dy = -radius; dy <= radius; ++dy) {
				const int sampleY = (std::max)(0, (std::min)(targetHeight - 1, y + dy));
				for (int dx = -radius; dx <= radius; ++dx) {
					const int sampleX = (std::max)(0, (std::min)(targetWidth - 1, x + dx));
					const DWORD pixel = sourcePixels[static_cast<size_t>(sampleY) * targetWidth + sampleX];
					blue += (pixel >> 0) & 0xff;
					green += (pixel >> 8) & 0xff;
					red += (pixel >> 16) & 0xff;
					alpha += (pixel >> 24) & 0xff;
					++count;
				}
			}
			row[x] = ((alpha / count) << 24) | ((red / count) << 16) |
				((green / count) << 8) | (blue / count);
		}
	}
	result->UnlockBits(&data);
	return result;
}

DWORD WINAPI ExitConfirmDesktopThreadProc(LPVOID parameter)
{
	auto* context = reinterpret_cast<ExitConfirmContext*>(parameter);
	if (!context || !context->desktop) {
		if (context) {
			context->desktopFailure = ERROR_INVALID_HANDLE;
			SetEvent(context->readyEvent);
		}
		return ERROR_INVALID_HANDLE;
	}
	if (!SetThreadDesktop(context->desktop)) {
		context->desktopFailure = GetLastError();
		SetEvent(context->readyEvent);
		return context->desktopFailure;
	}

	const int width = 520;
	const int height = 350;
	const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	context->panelX = (screenWidth - width) / 2;
	context->panelY = (screenHeight - height) / 2;
	context->blurredWallpaper = BuildExitWallpaper(context->wallpaperPath.c_str());
	context->window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST,
		kExitConfirmClass, L"确认退出", WS_POPUP | WS_BORDER,
		0, 0, screenWidth, screenHeight, nullptr, nullptr, GetModuleHandleW(nullptr), context);
	if (!context->window) {
		context->desktopFailure = GetLastError();
		SetEvent(context->readyEvent);
		return context->desktopFailure;
	}
	RAWINPUTDEVICE keyboardDevice{};
	keyboardDevice.usUsagePage = 0x01;
	keyboardDevice.usUsage = 0x06;
	keyboardDevice.dwFlags = RIDEV_INPUTSINK;
	keyboardDevice.hwndTarget = context->window;
	context->rawInputRegistered = RegisterRawInputDevices(&keyboardDevice, 1, sizeof(keyboardDevice)) != FALSE;
	ShowWindow(context->window, SW_SHOW);
	UpdateWindow(context->window);
	SetEvent(context->readyEvent);

	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(context->window, &message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (context->rawInputRegistered) {
		keyboardDevice.dwFlags = RIDEV_REMOVE;
		keyboardDevice.hwndTarget = nullptr;
		RegisterRawInputDevices(&keyboardDevice, 1, sizeof(keyboardDevice));
		context->rawInputRegistered = false;
	}
	return ERROR_SUCCESS;
}

LRESULT CALLBACK ExitConfirmEditProc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* context = reinterpret_cast<ExitConfirmContext*>(GetWindowLongPtrW(edit, GWLP_USERDATA));
	if (context && !context->updatingEdit &&
		(message == WM_CHAR || message == WM_UNICHAR || message == WM_SYSCHAR ||
			message == WM_KEYDOWN || message == WM_KEYUP ||
			message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
			message == WM_IME_CHAR || message == WM_IME_COMPOSITION ||
			message == WM_PASTE || message == WM_CUT || message == WM_CLEAR || message == WM_UNDO ||
			message == WM_SETTEXT || message == EM_REPLACESEL)) {
		// Physical key presses also produce legacy key messages after WM_INPUT.
		// Drop those duplicates silently; only WM_INPUT is allowed to update text.
		return 0;
	}
	return context && context->originalEditProc
		? CallWindowProcW(context->originalEditProc, edit, message, wParam, lParam)
		: DefWindowProcW(edit, message, wParam, lParam);
}

std::wstring MakeExitChallenge()
{
	static constexpr wchar_t digits[] = L"0123456789";
	std::mt19937 generator(static_cast<unsigned int>(GetTickCount64() ^ (static_cast<ULONGLONG>(GetCurrentProcessId()) << 16)));
	std::uniform_int_distribution<int> distribution(0, 9);
	std::wstring result;
	result.reserve(6);
	for (int index = 0; index < 6; ++index) result.push_back(digits[distribution(generator)]);
	return result;
}

void SetExitConfirmEditText(ExitConfirmContext* context, const std::wstring& value, size_t caret)
{
	if (!context || !context->edit) return;
	context->updatingEdit = true;
	SetWindowTextW(context->edit, value.c_str());
	context->updatingEdit = false;
	const LONG position = static_cast<LONG>((std::min)(caret, value.size()));
	SendMessageW(context->edit, EM_SETSEL, position, position);
}

void HandleExitConfirmKeyboard(HWND window, ExitConfirmContext* context, const RAWKEYBOARD& keyboard)
{
	if (!context || !context->edit || (keyboard.Flags & RI_KEY_BREAK) != 0) return;
	context->sawPhysicalKeyboard = true;
	if (keyboard.VKey == VK_RETURN) {
		SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_EXIT_CONFIRM_OK, BN_CLICKED), 0);
		return;
	}
	if (keyboard.VKey == VK_ESCAPE) {
		SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_EXIT_CONFIRM_CANCEL, BN_CLICKED), 0);
		return;
	}

	wchar_t digit = L'\0';
	if (keyboard.VKey >= '0' && keyboard.VKey <= '9') {
		if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0)
			digit = static_cast<wchar_t>(keyboard.VKey);
	}
	else if (keyboard.VKey >= VK_NUMPAD0 && keyboard.VKey <= VK_NUMPAD9) {
		digit = static_cast<wchar_t>(L'0' + (keyboard.VKey - VK_NUMPAD0));
	}
	if (digit == L'\0' && keyboard.VKey != VK_BACK) return;

	wchar_t buffer[64]{};
	GetWindowTextW(context->edit, buffer, _countof(buffer));
	std::wstring value = buffer;
	DWORD selectionStart = 0;
	DWORD selectionEnd = 0;
	SendMessageW(context->edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart),
		reinterpret_cast<LPARAM>(&selectionEnd));
	const size_t start = (std::min)(static_cast<size_t>(selectionStart), value.size());
	const size_t end = (std::min)(static_cast<size_t>(selectionEnd), value.size());
	if (digit != L'\0') {
		if (value.size() - (end - start) >= context->challenge.size()) return;
		value.replace(start, end - start, 1, digit);
		SetExitConfirmEditText(context, value, start + 1);
		return;
	}
	if (start != end) {
		value.erase(start, end - start);
		SetExitConfirmEditText(context, value, start);
	}
	else if (start > 0) {
		value.erase(start - 1, 1);
		SetExitConfirmEditText(context, value, start - 1);
	}
}

void PaintExitConfirm(HWND window, ExitConfirmContext* context)
{
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(window, &ps);
	RECT client{};
	GetClientRect(window, &client);
	Graphics graphics(dc);
	const int width = client.right;
	const int height = client.bottom;
	graphics.Clear(C(context->darkMode, 31, 38, 34, 25, 31, 28));
	if (context->blurredWallpaper && context->blurredWallpaper->GetLastStatus() == Ok)
		graphics.DrawImage(context->blurredWallpaper.get(), 0, 0, width, height);
	SolidBrush dimmer(Color(context->darkMode ? 172 : 136, context->darkMode ? 12 : 22,
		context->darkMode ? 22 : 42, context->darkMode ? 19 : 35));
	graphics.FillRectangle(&dimmer, 0, 0, width, height);
	const float panelX = static_cast<float>(context->panelX);
	const float panelY = static_cast<float>(context->panelY);
	FillRound(graphics, RectF(panelX, panelY, 520.0f, 350.0f), 12,
		C(context->darkMode, 255, 255, 255, 38, 45, 42));
	Text(graphics, L"确认退出 Dzjs Trainer", panelX + 34, panelY + 30, 452, 30, 19, FontStyleBold,
		C(context->darkMode, 30, 36, 33, 225, 233, 229));
	Text(graphics, L"为确认是本人操作，请在下方原样输入这组数字：", panelX + 34, panelY + 76, 452, 24, 11, FontStyleRegular,
		C(context->darkMode, 81, 91, 86, 171, 183, 177));
	Text(graphics, context->challenge.c_str(), panelX + 34, panelY + 108, 452, 38, 25, FontStyleBold,
		C(context->darkMode, 0, 107, 95, 130, 213, 199), StringAlignmentCenter);
	Text(graphics, L"仅接受真实键盘按键，粘贴或程序模拟输入不会通过验证。", panelX + 34, panelY + 178, 452, 22, 11, FontStyleRegular,
		C(context->darkMode, 91, 70, 20, 232, 211, 162), StringAlignmentCenter);
	if (context->invalidInput) {
		Text(graphics, L"输入不匹配或不是直接键盘输入，请重试。", panelX + 34, panelY + 204, 452, 22, 11, FontStyleBold,
			C(context->darkMode, 155, 45, 36, 244, 178, 165), StringAlignmentCenter);
	}
	EndPaint(window, &ps);
}

LRESULT CALLBACK ExitConfirmProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	ExitConfirmContext* context = reinterpret_cast<ExitConfirmContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
		context = static_cast<ExitConfirmContext*>(create->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_CREATE:
		context->font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
		context->editBrush = CreateSolidBrush(context->darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
		context->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL | ES_NUMBER,
			context->panelX + 34, context->panelY + 236, 452, 36, window, reinterpret_cast<HMENU>(IDC_EXIT_CONFIRM_EDIT), GetModuleHandleW(nullptr), nullptr);
		if (context->font) SendMessageW(context->edit, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
		SetWindowLongPtrW(context->edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		context->originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(context->edit, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(ExitConfirmEditProc)));
		CreateWindowExW(0, L"BUTTON", L"确认退出", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
			context->panelX + 276, context->panelY + 286, 100, 34, window, reinterpret_cast<HMENU>(IDC_EXIT_CONFIRM_OK), GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
			context->panelX + 386, context->panelY + 286, 100, 34, window, reinterpret_cast<HMENU>(IDC_EXIT_CONFIRM_CANCEL), GetModuleHandleW(nullptr), nullptr);
		SetFocus(context->edit);
		return 0;
	case WM_PAINT:
		PaintExitConfirm(window, context);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_NCHITTEST:
		return HTCLIENT;
	case WM_WINDOWPOSCHANGING: {
		// The confirmation desktop is a fixed full-screen surface.  Ignore any
		// move or resize request so the verification panel cannot be displaced.
		auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
		if (position) {
			position->x = 0;
			position->y = 0;
			position->cx = GetSystemMetrics(SM_CXSCREEN);
			position->cy = GetSystemMetrics(SM_CYSCREEN);
			position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
		}
		return 0;
	}
	case WM_INPUT: {
		UINT size = 0;
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
			sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) && size >= sizeof(RAWINPUTHEADER)) {
			std::vector<BYTE> rawData(size);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawData.data(), &size,
				sizeof(RAWINPUTHEADER)) == size) {
				const auto* raw = reinterpret_cast<const RAWINPUT*>(rawData.data());
				if (raw->header.dwType == RIM_TYPEKEYBOARD)
					HandleExitConfirmKeyboard(window, context, raw->data.keyboard);
			}
		}
		return 0;
	}
	case WM_PASTE:
		context->invalidInput = true;
		return 0;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_EXIT_CONFIRM_OK) {
			wchar_t buffer[64]{};
			GetWindowTextW(context->edit, buffer, _countof(buffer));
			context->value = buffer;
			context->accepted = context->sawPhysicalKeyboard && !context->injectedInput &&
				context->value == context->challenge;
			if (context->accepted) DestroyWindow(window);
			else {
				context->invalidInput = true;
				SetFocus(context->edit);
				SendMessageW(context->edit, EM_SETSEL, 0, -1);
				InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		if (LOWORD(wParam) == IDC_EXIT_CONFIRM_CANCEL) {
			DestroyWindow(window);
			return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_CTLCOLORSTATIC: {
		HDC dc = reinterpret_cast<HDC>(wParam);
		SetTextColor(dc, context->darkMode ? RGB(222, 230, 226) : RGB(35, 42, 39));
		SetBkMode(dc, TRANSPARENT);
		return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
	}
	case WM_CTLCOLOREDIT: {
		HDC dc = reinterpret_cast<HDC>(wParam);
		SetTextColor(dc, context->darkMode ? RGB(222, 230, 226) : RGB(35, 42, 39));
		SetBkColor(dc, context->darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
		return reinterpret_cast<LRESULT>(context->editBrush);
	}
	case WM_DRAWITEM: {
		const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
		if (item && (item->CtlID == IDC_EXIT_CONFIRM_OK || item->CtlID == IDC_EXIT_CONFIRM_CANCEL)) {
			DrawNativeButton(item, context->font, context->darkMode);
			return TRUE;
		}
		break;
	}
	case WM_NCDESTROY:
		if (context->edit && context->originalEditProc && IsWindow(context->edit)) {
			SetWindowLongPtrW(context->edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(context->originalEditProc));
			context->originalEditProc = nullptr;
		}
		else {
			context->originalEditProc = nullptr;
		}
		if (context->editBrush) {
			DeleteObject(context->editBrush);
			context->editBrush = nullptr;
		}
		if (context->font) {
			DeleteObject(context->font);
			context->font = nullptr;
		}
		context->window = nullptr;
		break;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

bool ConfirmExit(HWND owner, bool darkMode)
{
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW cls{};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = ExitConfirmProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cls.lpszClassName = kExitConfirmClass;
		if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		registered = true;
	}
	ExitConfirmContext context;
	context.owner = owner;
	context.darkMode = darkMode;
	context.challenge = MakeExitChallenge();
	wchar_t wallpaperPath[MAX_PATH] = {};
	if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, _countof(wallpaperPath), wallpaperPath, 0))
		context.wallpaperPath = wallpaperPath;
	context.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!context.readyEvent) return false;
	wchar_t desktopName[96] = {};
	swprintf_s(desktopName, L"DzjsTrainerPassword_%lu_%lu",
		static_cast<unsigned long>(GetCurrentProcessId()),
		static_cast<unsigned long>(GetTickCount()));
	context.desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0,
		DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP,
		nullptr);
	if (!context.desktop) {
		CloseHandle(context.readyEvent);
		context.readyEvent = nullptr;
		return false;
	}
	HDESK previousDesktop = OpenInputDesktop(0, FALSE,
		DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
	if (!previousDesktop) {
		CloseDesktop(context.desktop);
		CloseHandle(context.readyEvent);
		context.desktop = nullptr;
		context.readyEvent = nullptr;
		return false;
	}
	if (owner && IsWindowEnabled(owner)) EnableWindow(owner, FALSE);
	HANDLE thread = CreateThread(nullptr, 0, ExitConfirmDesktopThreadProc, &context, 0, nullptr);
	if (!thread) {
		if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
		CloseDesktop(context.desktop);
		CloseHandle(previousDesktop);
		CloseHandle(context.readyEvent);
		return false;
	}
	WaitForSingleObject(context.readyEvent, INFINITE);
	bool switched = false;
	if (context.desktopFailure == ERROR_SUCCESS && context.window)
		switched = SwitchDesktop(context.desktop) != FALSE;
	if (switched) {
		SetForegroundWindow(context.window);
		WaitForSingleObject(thread, INFINITE);
	}
	else {
		if (context.window) PostMessageW(context.window, WM_CLOSE, 0, 0);
		WaitForSingleObject(thread, INFINITE);
	}
	CloseHandle(thread);
	bool restored = !switched;
	if (switched) {
		// Switching back can briefly fail while win32k finishes the last window
		// transition. Never close the private desktop until restoration succeeds.
		for (int attempt = 0; attempt < 20 && !restored; ++attempt) {
			restored = SwitchDesktop(previousDesktop) != FALSE;
			if (!restored) Sleep(50);
		}
	}
	if (owner && IsWindow(owner)) {
		EnableWindow(owner, TRUE);
		if (restored) SetForegroundWindow(owner);
	}
	if (restored) CloseDesktop(context.desktop);
	CloseHandle(previousDesktop);
	CloseHandle(context.readyEvent);
	return context.accepted;
}

constexpr wchar_t kAvProcessPickerClass[] = L"DzjsTrainerAvProcessPicker";
struct AvProcessEntry {
	std::wstring name;
	DWORD processId = 0;
};
struct AvProcessPickerContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HWND list = nullptr;
	HWND status = nullptr;
	std::vector<AvProcessEntry> entries;
	DWORD selectedProcessId = 0;
	std::wstring selectedName;
	bool accepted = false;
};

void PopulateAvProcessPicker(AvProcessPickerContext* context)
{
	if (!context || !context->list) return;
	context->entries.clear();
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Process32FirstW(snapshot, &entry)) do {
			if (entry.th32ProcessID != 0)
				context->entries.push_back({ entry.szExeFile, entry.th32ProcessID });
		} while (Process32NextW(snapshot, &entry));
		CloseHandle(snapshot);
	}
	std::sort(context->entries.begin(), context->entries.end(), [](const AvProcessEntry& left, const AvProcessEntry& right) {
		const int nameOrder = _wcsicmp(left.name.c_str(), right.name.c_str());
		return nameOrder != 0 ? nameOrder < 0 : left.processId < right.processId;
	});
	ListView_DeleteAllItems(context->list);
	for (size_t index = 0; index < context->entries.size(); ++index) {
		LVITEMW item = {};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = static_cast<int>(index);
		item.pszText = const_cast<LPWSTR>(context->entries[index].name.c_str());
		item.lParam = static_cast<LPARAM>(index);
		const int row = ListView_InsertItem(context->list, &item);
		wchar_t pid[24] = {};
		swprintf_s(pid, L"%lu", static_cast<unsigned long>(context->entries[index].processId));
		ListView_SetItemText(context->list, row, 1, pid);
	}
	wchar_t status[96] = {};
	swprintf_s(status, L"共 %lu 个进程，选择后双击或点击确定", static_cast<unsigned long>(context->entries.size()));
	SetWindowTextW(context->status, status);
}

bool AcceptAvProcessSelection(AvProcessPickerContext* context)
{
	if (!context || !context->list) return false;
	const int selected = ListView_GetNextItem(context->list, -1, LVNI_SELECTED);
	if (selected < 0) {
		SetWindowTextW(context->status, L"请先选择一个运行中的进程");
		return false;
	}
	LVITEMW item = {};
	item.mask = LVIF_PARAM;
	item.iItem = selected;
	if (!ListView_GetItem(context->list, &item) || item.lParam < 0 ||
		static_cast<size_t>(item.lParam) >= context->entries.size()) return false;
	const AvProcessEntry& entry = context->entries[static_cast<size_t>(item.lParam)];
	context->selectedProcessId = entry.processId;
	context->selectedName = entry.name;
	context->accepted = true;
	DestroyWindow(context->window);
	return true;
}

LRESULT CALLBACK AvProcessPickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	AvProcessPickerContext* context = reinterpret_cast<AvProcessPickerContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		context = static_cast<AvProcessPickerContext*>(create->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_CREATE: {
		context->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			16, 16, 572, 344, window, reinterpret_cast<HMENU>(IDC_AV_PROCESS_LIST), GetModuleHandleW(nullptr), nullptr);
		ListView_SetExtendedListViewStyle(context->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
		LVCOLUMNW column = {};
		column.mask = LVCF_TEXT | LVCF_WIDTH;
		column.cx = 420;
		column.pszText = const_cast<LPWSTR>(L"进程名称");
		ListView_InsertColumn(context->list, 0, &column);
		column.cx = 120;
		column.pszText = const_cast<LPWSTR>(L"PID");
		ListView_InsertColumn(context->list, 1, &column);
		context->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			16, 370, 280, 28, window, nullptr, GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			308, 366, 82, 32, window, reinterpret_cast<HMENU>(IDC_AV_PROCESS_REFRESH), GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			400, 366, 88, 32, window, reinterpret_cast<HMENU>(IDC_AV_PROCESS_OK), GetModuleHandleW(nullptr), nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			498, 366, 88, 32, window, reinterpret_cast<HMENU>(IDC_AV_PROCESS_CANCEL), GetModuleHandleW(nullptr), nullptr);
		PopulateAvProcessPicker(context);
		SetFocus(context->list);
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_AV_PROCESS_REFRESH) { PopulateAvProcessPicker(context); return 0; }
		if (LOWORD(wParam) == IDC_AV_PROCESS_OK) { AcceptAvProcessSelection(context); return 0; }
		if (LOWORD(wParam) == IDC_AV_PROCESS_CANCEL) { DestroyWindow(window); return 0; }
		break;
	case WM_NOTIFY:
		if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_AV_PROCESS_LIST &&
			reinterpret_cast<NMHDR*>(lParam)->code == NM_DBLCLK) {
			AcceptAvProcessSelection(context);
			return 0;
		}
		break;
	case WM_CLOSE: DestroyWindow(window); return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

bool ChooseAvProcess(HWND owner, DWORD* processId, std::wstring* processName)
{
	if (!processId) return false;
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW cls = {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = AvProcessPickerProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		cls.lpszClassName = kAvProcessPickerClass;
		if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		registered = true;
	}
	AvProcessPickerContext context;
	context.owner = owner;
	RECT work = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int width = 620;
	const int height = 450;
	const int x = work.left + ((work.right - work.left) - width) / 2;
	const int y = work.top + ((work.bottom - work.top) - height) / 2;
	HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST, kAvProcessPickerClass,
		L"选择运行进程", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &context);
	if (!window) return false;
	if (owner && IsWindowEnabled(owner)) EnableWindow(owner, FALSE);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	MSG message = {};
	while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(window, &message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (owner && IsWindow(owner)) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
	if (!context.accepted) return false;
	*processId = context.selectedProcessId;
	if (processName) *processName = context.selectedName;
	return true;
}

enum class SampleDialogResult {
	Selected,
	Cancelled,
	Failed
};

constexpr wchar_t kSamplePickerClass[] = L"DzjsTrainerExeSamplePicker";
constexpr int IDC_SAMPLE_PATH = 61001;
constexpr int IDC_SAMPLE_GO = 61002;
constexpr int IDC_SAMPLE_UP = 61003;
constexpr int IDC_SAMPLE_LIST = 61004;
constexpr int IDC_SAMPLE_STATUS = 61005;
constexpr int IDC_SAMPLE_ALL = 61006;
constexpr int IDC_SAMPLE_OK = 61007;
constexpr int IDC_SAMPLE_CANCEL = 61008;

struct SamplePickerEntry {
	std::wstring name;
	std::wstring path;
	bool directory = false;
};

struct SampleDialogThreadContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HWND pathEdit = nullptr;
	HWND list = nullptr;
	HWND status = nullptr;
	HFONT font = nullptr;
	std::wstring currentDirectory;
	std::vector<SamplePickerEntry> entries;
	std::vector<std::wstring> paths;
	DWORD failure = ERROR_SUCCESS;
	SampleDialogResult result = SampleDialogResult::Failed;
};

bool IsExeFileName(const std::wstring& name)
{
	const size_t dot = name.find_last_of(L'.');
	return dot != std::wstring::npos && _wcsicmp(name.c_str() + dot, L".exe") == 0;
}

std::wstring JoinSamplePath(const std::wstring& directory, const std::wstring& name)
{
	if (directory.empty()) return name;
	if (directory.back() == L'\\' || directory.back() == L'/') return directory + name;
	return directory + L"\\" + name;
}

void SetSamplePickerStatus(SampleDialogThreadContext* context, LPCWSTR text)
{
	if (context && context->status) SetWindowTextW(context->status, text ? text : L"");
}

void UpdateSamplePickerSelectionStatus(SampleDialogThreadContext* context)
{
	if (!context || !context->list) return;
	int checked = 0;
	for (int index = 0; index < static_cast<int>(context->entries.size()); ++index) {
		if (!context->entries[index].directory && ListView_GetCheckState(context->list, index))
			++checked;
	}
	wchar_t message[96] = {};
	if (checked > 16)
		swprintf_s(message, L"已勾选 %d 个 EXE，将录入前 16 个", checked);
	else if (checked != 0)
		swprintf_s(message, L"已勾选 %d 个 EXE，最多录入 16 个", checked);
	else
		wcscpy_s(message, L"勾选一个或多个 EXE 样本");
	SetSamplePickerStatus(context, message);
}

void PopulateSamplePickerList(SampleDialogThreadContext* context)
{
	if (!context || !context->list) return;
	ListView_DeleteAllItems(context->list);
	for (size_t index = 0; index < context->entries.size(); ++index) {
		SamplePickerEntry& entry = context->entries[index];
		LVITEMW item = {};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = static_cast<int>(index);
		item.pszText = const_cast<LPWSTR>(entry.name.c_str());
		item.lParam = static_cast<LPARAM>(index);
		const int row = ListView_InsertItem(context->list, &item);
		ListView_SetItemText(context->list, row, 1,
			const_cast<LPWSTR>(entry.directory ? L"文件夹" : L"应用程序"));
		if (entry.directory) ListView_SetItemState(
			context->list, row, 0, LVIS_STATEIMAGEMASK);
	}
	UpdateSamplePickerSelectionStatus(context);
}

bool NavigateSamplePicker(SampleDialogThreadContext* context, const std::wstring& requestedPath)
{
	if (!context) return false;
	context->entries.clear();
	context->failure = ERROR_SUCCESS;

	if (requestedPath.empty()) {
		wchar_t drives[512] = {};
		const DWORD length = GetLogicalDriveStringsW(_countof(drives), drives);
		if (length == 0 || length >= _countof(drives)) {
			context->failure = GetLastError();
			SetSamplePickerStatus(context, L"无法枚举本机磁盘");
			return false;
		}
		for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
			context->entries.push_back({ drive, drive, true });
		context->currentDirectory.clear();
		SetWindowTextW(context->pathEdit, L"此电脑");
		PopulateSamplePickerList(context);
		return true;
	}

	wchar_t fullPath[32768] = {};
	const DWORD fullLength = GetFullPathNameW(
		requestedPath.c_str(), _countof(fullPath), fullPath, nullptr);
	if (fullLength == 0 || fullLength >= _countof(fullPath)) {
		context->failure = GetLastError();
		SetSamplePickerStatus(context, L"路径无效或过长");
		return false;
	}
	const DWORD attributes = GetFileAttributesW(fullPath);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		context->failure = GetLastError();
		SetSamplePickerStatus(context, L"路径不存在或当前无法访问");
		return false;
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		if (!IsExeFileName(fullPath)) {
			context->failure = ERROR_BAD_FORMAT;
			SetSamplePickerStatus(context, L"只能录入 EXE 文件");
			return false;
		}
		context->paths.assign(1, fullPath);
		context->result = SampleDialogResult::Selected;
		DestroyWindow(context->window);
		return true;
	}

	std::wstring directory(fullPath);
	while (directory.size() > 3 && (directory.back() == L'\\' || directory.back() == L'/'))
		directory.pop_back();
	std::wstring search = JoinSamplePath(directory, L"*");
	WIN32_FIND_DATAW data = {};
	HANDLE find = FindFirstFileW(search.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		context->failure = GetLastError();
		SetSamplePickerStatus(context, L"无法读取该目录");
		return false;
	}
	do {
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
			continue;
		const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (!isDirectory && !IsExeFileName(data.cFileName)) continue;
		context->entries.push_back({
			data.cFileName,
			JoinSamplePath(directory, data.cFileName),
			isDirectory
		});
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(context->entries.begin(), context->entries.end(),
		[](const SamplePickerEntry& left, const SamplePickerEntry& right) {
			if (left.directory != right.directory) return left.directory > right.directory;
			return CompareStringOrdinal(
				left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
		});
	context->currentDirectory = directory;
	SetWindowTextW(context->pathEdit, directory.c_str());
	PopulateSamplePickerList(context);
	return true;
}

void CompleteSamplePickerSelection(SampleDialogThreadContext* context)
{
	if (!context || !context->list) return;
	context->paths.clear();
	for (int index = 0;
		index < static_cast<int>(context->entries.size()) && context->paths.size() < 16;
		++index) {
		if (!context->entries[index].directory && ListView_GetCheckState(context->list, index)) {
			context->paths.push_back(context->entries[index].path);
		}
	}
	if (context->paths.empty()) {
		SetSamplePickerStatus(context, L"请先勾选至少一个 EXE 样本");
		return;
	}
	context->result = SampleDialogResult::Selected;
	DestroyWindow(context->window);
}

LRESULT CALLBACK SamplePickerWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* context = reinterpret_cast<SampleDialogThreadContext*>(
		GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		context = reinterpret_cast<SampleDialogThreadContext*>(
			reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		if (context) context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);

	switch (message) {
	case WM_CREATE:
	{
		context->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH, L"Microsoft YaHei UI");
		context->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_PATH), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"转到", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_GO), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"上一级", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_UP), nullptr, nullptr);
		context->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_LIST), nullptr, nullptr);
		context->status = CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_STATUS), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"全选 EXE", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_ALL), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_OK), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SAMPLE_CANCEL), nullptr, nullptr);
		EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
			SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
			return TRUE;
		}, reinterpret_cast<LPARAM>(context->font));
		ListView_SetExtendedListViewStyle(context->list,
			LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
		LVCOLUMNW column = {};
		column.mask = LVCF_TEXT | LVCF_WIDTH;
		column.cx = 510; column.pszText = const_cast<LPWSTR>(L"名称");
		ListView_InsertColumn(context->list, 0, &column);
		column.cx = 120; column.pszText = const_cast<LPWSTR>(L"类型");
		ListView_InsertColumn(context->list, 1, &column);

		wchar_t modulePath[32768] = {};
		GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
		wchar_t* slash = wcsrchr(modulePath, L'\\');
		if (slash) *slash = L'\0';
		NavigateSamplePicker(context, slash ? modulePath : L"");
		return 0;
	}
	case WM_SIZE:
	{
		const int width = LOWORD(lParam);
		const int height = HIWORD(lParam);
		MoveWindow(context->pathEdit, 16, 16, (std::max)(160, width - 190), 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_SAMPLE_GO), width - 164, 16, 68, 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_SAMPLE_UP), width - 88, 16, 72, 30, TRUE);
		MoveWindow(context->list, 16, 58, width - 32, (std::max)(120, height - 136), TRUE);
		MoveWindow(context->status, 18, height - 66, (std::max)(120, width - 330), 24, TRUE);
		MoveWindow(GetDlgItem(window, IDC_SAMPLE_ALL), width - 306, height - 72, 94, 32, TRUE);
		MoveWindow(GetDlgItem(window, IDC_SAMPLE_OK), width - 202, height - 72, 88, 32, TRUE);
		MoveWindow(GetDlgItem(window, IDC_SAMPLE_CANCEL), width - 104, height - 72, 88, 32, TRUE);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_SAMPLE_GO:
		{
			wchar_t path[32768] = {};
			GetWindowTextW(context->pathEdit, path, _countof(path));
			NavigateSamplePicker(context, wcscmp(path, L"此电脑") == 0 ? L"" : path);
			return 0;
		}
		case IDC_SAMPLE_UP:
			if (context->currentDirectory.empty() ||
				(context->currentDirectory.size() >= 2 && context->currentDirectory.size() <= 3 &&
					context->currentDirectory[1] == L':')) {
				NavigateSamplePicker(context, L"");
			}
			else {
				std::wstring parent = context->currentDirectory;
				const size_t slash = parent.find_last_of(L"\\/");
				if (slash == std::wstring::npos) NavigateSamplePicker(context, L"");
				else NavigateSamplePicker(context, parent.substr(0, slash == 2 ? 3 : slash));
			}
			return 0;
		case IDC_SAMPLE_ALL:
		{
			int checked = 0;
			for (int index = 0; index < static_cast<int>(context->entries.size()); ++index) {
				if (!context->entries[index].directory && checked < 16) {
					ListView_SetCheckState(context->list, index, TRUE);
					++checked;
				}
			}
			UpdateSamplePickerSelectionStatus(context);
			return 0;
		}
		case IDC_SAMPLE_OK:
			CompleteSamplePickerSelection(context);
			return 0;
		case IDC_SAMPLE_CANCEL:
			context->result = SampleDialogResult::Cancelled;
			DestroyWindow(window);
			return 0;
		}
		break;
	case WM_NOTIFY:
		if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_SAMPLE_LIST) {
			const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
			if (header->code == NM_DBLCLK) {
				const int index = reinterpret_cast<const NMITEMACTIVATE*>(lParam)->iItem;
				if (index >= 0 && index < static_cast<int>(context->entries.size())) {
					if (context->entries[index].directory)
						NavigateSamplePicker(context, context->entries[index].path);
					else
						ListView_SetCheckState(context->list, index,
							!ListView_GetCheckState(context->list, index));
				}
				return 0;
			}
			if (header->code == LVN_ITEMCHANGED) UpdateSamplePickerSelectionStatus(context);
		}
		break;
	case WM_CLOSE:
		context->result = SampleDialogResult::Cancelled;
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		if (context->font) {
			DeleteObject(context->font);
			context->font = nullptr;
		}
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI ChooseExecutableSamplesThreadProc(LPVOID parameter)
{
	auto* context = static_cast<SampleDialogThreadContext*>(parameter);
	if (!context) return ERROR_INVALID_PARAMETER;
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = SamplePickerWindowProc;
	windowClass.hInstance = JTAppGetInstanceDirect();
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_APP));
	windowClass.hIconSm = windowClass.hIcon;
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	windowClass.lpszClassName = kSamplePickerClass;
	if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		context->failure = GetLastError();
		return context->failure;
	}

	RECT workArea = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
	RECT ownerRect = workArea;
	if (context->owner) GetWindowRect(context->owner, &ownerRect);
	const int width = 760;
	const int height = 540;
	const int x = (std::max)(workArea.left,
		(std::min)(workArea.right - width, ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2));
	const int y = (std::max)(workArea.top,
		(std::min)(workArea.bottom - height, ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2));
	HWND window = CreateWindowExW(
		WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST,
		kSamplePickerClass,
		L"录入版本样本",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX,
		x, y, width, height,
		nullptr, nullptr, windowClass.hInstance, context);
	if (!window) {
		context->failure = GetLastError();
		return context->failure;
	}
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	SetForegroundWindow(window);
	MSG message = {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(window, &message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	return ERROR_SUCCESS;
}

SampleDialogResult ChooseExecutableSamplesOnStaThread(
	HWND owner,
	std::vector<std::wstring>* paths,
	DWORD* failure)
{
	if (!paths) {
		if (failure) *failure = ERROR_INVALID_PARAMETER;
		return SampleDialogResult::Failed;
	}

	SampleDialogThreadContext context;
	context.owner = owner;
	HANDLE thread = CreateThread(
		nullptr, 0, ChooseExecutableSamplesThreadProc, &context, 0, nullptr);
	if (!thread) {
		const DWORD error = GetLastError();
		if (failure) *failure = error;
		return SampleDialogResult::Failed;
	}

	const BOOL ownerWasEnabled = owner && IsWindowEnabled(owner);
	if (ownerWasEnabled) EnableWindow(owner, FALSE);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	if (ownerWasEnabled && IsWindow(owner)) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}

	*paths = std::move(context.paths);
	if (failure) *failure = context.failure;
	return context.result;
}

constexpr wchar_t kLogSavePickerClass[] = L"DzjsTrainerLogSavePicker";
constexpr int IDC_LOG_SAVE_PATH = 62001;
constexpr int IDC_LOG_SAVE_GO = 62002;
constexpr int IDC_LOG_SAVE_UP = 62003;
constexpr int IDC_LOG_SAVE_LIST = 62004;
constexpr int IDC_LOG_SAVE_STATUS = 62005;
constexpr int IDC_LOG_SAVE_NAME = 62006;
constexpr int IDC_LOG_SAVE_REFRESH = 62007;
constexpr int IDC_LOG_SAVE_OK = 62008;
constexpr int IDC_LOG_SAVE_CANCEL = 62009;
constexpr int IDC_LOG_SAVE_LABEL = 62010;

struct LogSavePickerEntry {
	std::wstring name;
	std::wstring path;
	bool directory = false;
};

struct LogSavePickerContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HWND pathEdit = nullptr;
	HWND nameEdit = nullptr;
	HWND list = nullptr;
	HWND status = nullptr;
	HFONT font = nullptr;
	std::wstring currentDirectory;
	std::wstring initialName = L"DzjsTrainer-log.txt";
	std::vector<LogSavePickerEntry> entries;
	std::wstring selectedPath;
	bool accepted = false;
};

void SetLogSavePickerStatus(LogSavePickerContext* context, LPCWSTR text)
{
	if (context && context->status) SetWindowTextW(context->status, text ? text : L"");
}

void PopulateLogSavePickerList(LogSavePickerContext* context)
{
	if (!context || !context->list) return;
	ListView_DeleteAllItems(context->list);
	for (size_t index = 0; index < context->entries.size(); ++index) {
		const auto& entry = context->entries[index];
		LVITEMW item = {};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = static_cast<int>(index);
		item.pszText = const_cast<LPWSTR>(entry.name.c_str());
		item.lParam = static_cast<LPARAM>(index);
		const int row = ListView_InsertItem(context->list, &item);
		ListView_SetItemText(context->list, row, 1,
			const_cast<LPWSTR>(entry.directory ? L"文件夹" : L"文件"));
	}
}

bool NavigateLogSavePicker(LogSavePickerContext* context, const std::wstring& requestedPath)
{
	if (!context) return false;
	context->entries.clear();

	if (requestedPath.empty()) {
		wchar_t drives[512] = {};
		const DWORD length = GetLogicalDriveStringsW(_countof(drives), drives);
		if (length == 0 || length >= _countof(drives)) {
			SetLogSavePickerStatus(context, L"无法枚举本机磁盘");
			return false;
		}
		for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
			context->entries.push_back({ drive, drive, true });
		context->currentDirectory.clear();
		SetWindowTextW(context->pathEdit, L"此电脑");
		SetLogSavePickerStatus(context, L"选择保存位置");
		PopulateLogSavePickerList(context);
		return true;
	}

	wchar_t fullPath[32768] = {};
	const DWORD fullLength = GetFullPathNameW(
		requestedPath.c_str(), _countof(fullPath), fullPath, nullptr);
	if (fullLength == 0 || fullLength >= _countof(fullPath)) {
		SetLogSavePickerStatus(context, L"路径无效或过长");
		return false;
	}
	const DWORD attributes = GetFileAttributesW(fullPath);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		SetLogSavePickerStatus(context, L"路径不存在或当前无法访问");
		return false;
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		const wchar_t* slash = wcsrchr(fullPath, L'\\');
		if (!slash) {
			SetLogSavePickerStatus(context, L"请输入文件夹路径");
			return false;
		}
		SetWindowTextW(context->nameEdit, slash + 1);
		return NavigateLogSavePicker(context,
			std::wstring(fullPath, static_cast<size_t>(slash - fullPath)));
	}

	std::wstring directory(fullPath);
	while (directory.size() > 3 && (directory.back() == L'\\' || directory.back() == L'/'))
		directory.pop_back();
	WIN32_FIND_DATAW data = {};
	HANDLE find = FindFirstFileW(JoinSamplePath(directory, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		SetLogSavePickerStatus(context, L"无法读取该目录");
		return false;
	}
	do {
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
			continue;
		const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		context->entries.push_back({ data.cFileName, JoinSamplePath(directory, data.cFileName), isDirectory });
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(context->entries.begin(), context->entries.end(),
		[](const LogSavePickerEntry& left, const LogSavePickerEntry& right) {
			if (left.directory != right.directory) return left.directory > right.directory;
			return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
		});
	context->currentDirectory = directory;
	SetWindowTextW(context->pathEdit, directory.c_str());
	SetLogSavePickerStatus(context, L"选择文件名后点击保存");
	PopulateLogSavePickerList(context);
	return true;
}

void AcceptLogSavePicker(LogSavePickerContext* context)
{
	if (!context) return;
	wchar_t name[32768] = {};
	GetWindowTextW(context->nameEdit, name, _countof(name));
	std::wstring fileName(name);
	while (!fileName.empty() && iswspace(fileName.front())) fileName.erase(fileName.begin());
	while (!fileName.empty() && iswspace(fileName.back())) fileName.pop_back();
	if (fileName.empty()) {
		SetLogSavePickerStatus(context, L"请输入文件名");
		SetFocus(context->nameEdit);
		return;
	}
	if (context->currentDirectory.empty()) {
		SetLogSavePickerStatus(context, L"请先进入一个保存文件夹");
		return;
	}
	if (fileName.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
		SetLogSavePickerStatus(context, L"文件名包含无效字符");
		return;
	}
	if (fileName.find_last_of(L'.') == std::wstring::npos)
		fileName += L".txt";
	const std::wstring target = JoinSamplePath(context->currentDirectory, fileName);
	if (target.size() >= 32768) {
		SetLogSavePickerStatus(context, L"目标路径过长");
		return;
	}
	const DWORD targetAttributes = GetFileAttributesW(target.c_str());
	if (targetAttributes != INVALID_FILE_ATTRIBUTES) {
		if ((targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			SetLogSavePickerStatus(context, L"目标名称是文件夹，请重新输入文件名");
			return;
		}
		if (MessageBoxW(context->window, L"文件已存在，是否覆盖？", L"导出日志",
			MB_YESNO | MB_ICONQUESTION) != IDYES)
			return;
	}
	context->selectedPath = target;
	context->accepted = true;
	DestroyWindow(context->window);
}

LRESULT CALLBACK LogSavePickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* context = reinterpret_cast<LogSavePickerContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		context = reinterpret_cast<LogSavePickerContext*>(
			reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		if (context) context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_CREATE: {
		context->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH, L"Microsoft YaHei UI");
		context->pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
			window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_PATH), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"转到", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_GO), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"上一级", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_UP), nullptr, nullptr);
		context->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_LIST), nullptr, nullptr);
		context->status = CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
			window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_STATUS), nullptr, nullptr);
		CreateWindowExW(0, L"STATIC", L"文件名:", WS_CHILD | WS_VISIBLE | SS_LEFT,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_LABEL), nullptr, nullptr);
		context->nameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", context->initialName.c_str(),
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
			window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_NAME), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_REFRESH), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_OK), nullptr, nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_SAVE_CANCEL), nullptr, nullptr);
		EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
			SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
			return TRUE;
		}, reinterpret_cast<LPARAM>(context->font));
		ListView_SetExtendedListViewStyle(context->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		LVCOLUMNW column = {};
		column.mask = LVCF_TEXT | LVCF_WIDTH;
		column.cx = 520; column.pszText = const_cast<LPWSTR>(L"名称");
		ListView_InsertColumn(context->list, 0, &column);
		column.cx = 120; column.pszText = const_cast<LPWSTR>(L"类型");
		ListView_InsertColumn(context->list, 1, &column);
		wchar_t modulePath[32768] = {};
		GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
		wchar_t* slash = wcsrchr(modulePath, L'\\');
		if (slash) *slash = L'\0';
		NavigateLogSavePicker(context, slash ? modulePath : L"");
		SetFocus(context->nameEdit);
		SendMessageW(context->nameEdit, EM_SETSEL, 0, -1);
		return 0;
	}
	case WM_SIZE: {
		const int width = LOWORD(lParam);
		const int height = HIWORD(lParam);
		MoveWindow(context->pathEdit, 16, 16, (std::max)(160, width - 190), 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_GO), width - 164, 16, 68, 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_UP), width - 88, 16, 72, 30, TRUE);
		MoveWindow(context->list, 16, 58, width - 32, (std::max)(120, height - 180), TRUE);
		MoveWindow(context->status, 18, height - 150, width - 36, 24, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_LABEL), 18, height - 112, 48, 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_NAME), 72, height - 112, (std::max)(180, width - 440), 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_REFRESH), width - 350, height - 112, 78, 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_OK), width - 256, height - 112, 88, 30, TRUE);
		MoveWindow(GetDlgItem(window, IDC_LOG_SAVE_CANCEL), width - 158, height - 112, 88, 30, TRUE);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_LOG_SAVE_GO: {
			wchar_t path[32768] = {};
			GetWindowTextW(context->pathEdit, path, _countof(path));
			NavigateLogSavePicker(context, wcscmp(path, L"此电脑") == 0 ? L"" : path);
			return 0;
		}
		case IDC_LOG_SAVE_UP: {
			if (context->currentDirectory.empty()) NavigateLogSavePicker(context, L"");
			else if (context->currentDirectory.size() <= 3 && context->currentDirectory[1] == L':')
				NavigateLogSavePicker(context, L"");
			else {
				const size_t slash = context->currentDirectory.find_last_of(L"\\/");
				NavigateLogSavePicker(context, slash == std::wstring::npos ? L"" :
					context->currentDirectory.substr(0, slash == 2 ? 3 : slash));
			}
			return 0;
		}
		case IDC_LOG_SAVE_REFRESH: NavigateLogSavePicker(context, context->currentDirectory); return 0;
		case IDC_LOG_SAVE_OK: AcceptLogSavePicker(context); return 0;
		case IDC_LOG_SAVE_CANCEL: DestroyWindow(window); return 0;
		}
		break;
	case WM_NOTIFY:
		if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_LOG_SAVE_LIST &&
			reinterpret_cast<NMHDR*>(lParam)->code == NM_DBLCLK) {
			const int index = reinterpret_cast<const NMITEMACTIVATE*>(lParam)->iItem;
			if (index >= 0 && index < static_cast<int>(context->entries.size())) {
				const auto& entry = context->entries[static_cast<size_t>(index)];
				if (entry.directory) NavigateLogSavePicker(context, entry.path);
				else SetWindowTextW(context->nameEdit, entry.name.c_str());
			}
			return 0;
		}
		break;
	case WM_CLOSE: DestroyWindow(window); return 0;
	case WM_DESTROY:
		if (context->font) {
			DeleteObject(context->font);
			context->font = nullptr;
		}
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

bool ChooseLogSavePath(HWND owner, std::wstring* path)
{
	if (!path) return false;
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW windowClass = {};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = LogSavePickerProc;
		windowClass.hInstance = JTAppGetInstanceDirect();
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_APP));
		windowClass.hIconSm = windowClass.hIcon;
		windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		windowClass.lpszClassName = kLogSavePickerClass;
		if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		registered = true;
	}
	LogSavePickerContext context;
	context.owner = owner;
	context.initialName = *path;
	RECT workArea = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
	RECT ownerRect = workArea;
	if (owner) GetWindowRect(owner, &ownerRect);
	const int width = 760;
	const int height = 540;
	const int x = (std::max)(workArea.left,
		(std::min)(workArea.right - width, ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2));
	const int y = (std::max)(workArea.top,
		(std::min)(workArea.bottom - height, ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2));
	// 主窗口是置顶的（TopMost）。这个选择器如果不跟着置顶、也不挂 owner，
	// 创建出来必然被主窗口盖住 —— 用户点了「导出日志」却什么都看不到。
	// 既挂上 owner，又加 WS_EX_TOPMOST，保证它出现在主窗口之上。
	HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST,
		kLogSavePickerClass, L"导出日志", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
		x, y, width, height, owner, nullptr, JTAppGetInstanceDirect(), &context);
	if (!window) return false;
	SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
	const BOOL ownerWasEnabled = owner && IsWindowEnabled(owner);
	if (ownerWasEnabled) EnableWindow(owner, FALSE);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	SetForegroundWindow(window);
	MSG message = {};
	while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(window, &message)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (ownerWasEnabled && IsWindow(owner)) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
	if (!context.accepted) return false;
	*path = std::move(context.selectedPath);
	return true;
}

// A compact, owner-drawn browser used by the session-only media controls.
// Keeping the file browser in-process avoids falling back to the system file dialog.
constexpr wchar_t kMediaPickerClass[] = L"DzjsTrainerMediaPicker";

struct MediaPickerEntry {
	std::wstring name;
	std::wstring path;
	bool directory = false;
};

struct MediaPickerContext {
	HWND owner = nullptr;
	HWND window = nullptr;
	HFONT font = nullptr;
	HFONT titleFont = nullptr;
	bool image = true;
	std::wstring currentDirectory;
	std::vector<MediaPickerEntry> entries;
	std::wstring selectedPath;
	int selectedIndex = -1;
	int scrollOffset = 0;
	bool accepted = false;
	RECT upRect{};
	RECT useRect{};
	RECT cancelRect{};
	RECT listRect{};
};

bool IsSupportedMediaName(const std::wstring& name, bool image)
{
	const size_t dot = name.find_last_of(L'.');
	if (dot == std::wstring::npos) return false;
	const wchar_t* extension = name.c_str() + dot;
	if (image)
		return _wcsicmp(extension, L".bmp") == 0 || _wcsicmp(extension, L".jpg") == 0 ||
			_wcsicmp(extension, L".jpeg") == 0 || _wcsicmp(extension, L".png") == 0;
	return _wcsicmp(extension, L".mp4") == 0 || _wcsicmp(extension, L".wmv") == 0 ||
		_wcsicmp(extension, L".avi") == 0 || _wcsicmp(extension, L".mov") == 0 ||
		_wcsicmp(extension, L".mkv") == 0;
}

void MediaPickerNavigate(MediaPickerContext* context, const std::wstring& requestedPath)
{
	if (!context) return;
	context->entries.clear();
	context->selectedIndex = -1;
	context->scrollOffset = 0;
	if (requestedPath.empty()) {
		wchar_t drives[512] = {};
		const DWORD length = GetLogicalDriveStringsW(_countof(drives), drives);
		if (length != 0 && length < _countof(drives)) {
			for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1)
				context->entries.push_back({ drive, drive, true });
		}
		context->currentDirectory.clear();
		InvalidateRect(context->window, nullptr, FALSE);
		return;
	}

	wchar_t fullPath[32768] = {};
	const DWORD length = GetFullPathNameW(requestedPath.c_str(), _countof(fullPath), fullPath, nullptr);
	if (length == 0 || length >= _countof(fullPath) ||
		(GetFileAttributesW(fullPath) & FILE_ATTRIBUTE_DIRECTORY) == 0) return;
	std::wstring directory(fullPath);
	while (directory.size() > 3 && (directory.back() == L'\\' || directory.back() == L'/')) directory.pop_back();
	WIN32_FIND_DATAW data{};
	HANDLE find = FindFirstFileW(JoinSamplePath(directory, L"*").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return;
	do {
		if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
		const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (isDirectory || IsSupportedMediaName(data.cFileName, context->image))
			context->entries.push_back({ data.cFileName, JoinSamplePath(directory, data.cFileName), isDirectory });
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(context->entries.begin(), context->entries.end(),
		[](const MediaPickerEntry& left, const MediaPickerEntry& right) {
			if (left.directory != right.directory) return left.directory > right.directory;
			return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
		});
	context->currentDirectory = directory;
	InvalidateRect(context->window, nullptr, FALSE);
}

void MediaPickerDrawButton(HDC dc, const RECT& rect, LPCWSTR text, bool primary, HFONT font)
{
	HBRUSH brush = CreateSolidBrush(primary ? RGB(0, 107, 95) : RGB(224, 235, 230));
	FillRect(dc, &rect, brush);
	DeleteObject(brush);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, primary ? RGB(255, 255, 255) : RGB(0, 81, 72));
	HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
	RECT content = rect;
	DrawTextW(dc, text, -1, &content, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
	SelectObject(dc, previous);
}

void MediaPickerPaint(MediaPickerContext* context, HDC dc)
{
	RECT client{};
	GetClientRect(context->window, &client);
	HBRUSH background = CreateSolidBrush(RGB(245, 249, 247));
	FillRect(dc, &client, background);
	DeleteObject(background);

	RECT header{ 0, 0, client.right, 72 };
	HBRUSH headerBrush = CreateSolidBrush(RGB(24, 50, 44));
	FillRect(dc, &header, headerBrush);
	DeleteObject(headerBrush);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	HFONT oldTitle = static_cast<HFONT>(SelectObject(dc, context->titleFont));
	RECT title{ 22, 15, client.right - 22, 40 };
	DrawTextW(dc, context->image ? L"选择图片" : L"选择视频", -1, &title, DT_SINGLELINE | DT_VCENTER);
	SelectObject(dc, oldTitle);
	SetTextColor(dc, RGB(197, 222, 214));
	HFONT oldFont = static_cast<HFONT>(SelectObject(dc, context->font));
	RECT subtitle{ 22, 42, client.right - 22, 64 };
	DrawTextW(dc, context->image ? L"BMP / JPG / PNG" : L"MP4 / WMV / AVI / MOV / MKV", -1, &subtitle, DT_SINGLELINE | DT_VCENTER);

	RECT path{ 22, 90, client.right - 122, 122 };
	HBRUSH pathBrush = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(dc, &path, pathBrush);
	DeleteObject(pathBrush);
	FrameRect(dc, &path, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
	SetTextColor(dc, RGB(53, 67, 62));
	RECT pathText{ path.left + 10, path.top, path.right - 10, path.bottom };
	const wchar_t* location = context->currentDirectory.empty() ? L"此电脑" : context->currentDirectory.c_str();
	DrawTextW(dc, location, -1, &pathText, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	context->upRect = { client.right - 108, 90, client.right - 22, 122 };
	MediaPickerDrawButton(dc, context->upRect, L"上一级", false, context->font);

	context->listRect = { 22, 140, client.right - 22, client.bottom - 76 };
	HBRUSH listBrush = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(dc, &context->listRect, listBrush);
	DeleteObject(listBrush);
	FrameRect(dc, &context->listRect, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
	const int rowHeight = 32;
	const int visibleRows = (std::max)(1, static_cast<int>((context->listRect.bottom - context->listRect.top) / rowHeight));
	const int first = (std::min)(context->scrollOffset, (std::max)(0, static_cast<int>(context->entries.size()) - visibleRows));
	context->scrollOffset = first;
	for (int slot = 0; slot < visibleRows; ++slot) {
		const int index = first + slot;
		if (index >= static_cast<int>(context->entries.size())) break;
		RECT row{ context->listRect.left + 1, context->listRect.top + slot * rowHeight + 1,
			context->listRect.right - 1, context->listRect.top + (slot + 1) * rowHeight };
		if (index == context->selectedIndex) {
			HBRUSH selectedBrush = CreateSolidBrush(RGB(216, 238, 231));
			FillRect(dc, &row, selectedBrush);
			DeleteObject(selectedBrush);
		}
		SetTextColor(dc, context->entries[index].directory ? RGB(0, 98, 84) : RGB(53, 67, 62));
		RECT name{ row.left + 12, row.top, row.right - 110, row.bottom };
		DrawTextW(dc, context->entries[index].name.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		SetTextColor(dc, RGB(112, 125, 120));
		RECT type{ row.right - 96, row.top, row.right - 12, row.bottom };
		DrawTextW(dc, context->entries[index].directory ? L"文件夹" : L"媒体文件", -1, &type, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
	}
	if (context->entries.empty()) {
		SetTextColor(dc, RGB(112, 125, 120));
		RECT empty = context->listRect;
		DrawTextW(dc, L"此位置没有可选文件", -1, &empty, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
	}

	context->cancelRect = { client.right - 118, client.bottom - 54, client.right - 22, client.bottom - 20 };
	context->useRect = { client.right - 224, client.bottom - 54, client.right - 128, client.bottom - 20 };
	MediaPickerDrawButton(dc, context->useRect, L"使用文件", true, context->font);
	MediaPickerDrawButton(dc, context->cancelRect, L"取消", false, context->font);
	SetTextColor(dc, RGB(86, 101, 95));
	RECT hint{ 22, client.bottom - 54, client.right - 238, client.bottom - 20 };
	DrawTextW(dc, L"双击文件夹进入，双击媒体文件选择", -1, &hint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	SelectObject(dc, oldFont);
}

void MediaPickerAccept(MediaPickerContext* context)
{
	if (!context || context->selectedIndex < 0 || context->selectedIndex >= static_cast<int>(context->entries.size())) return;
	const auto& entry = context->entries[context->selectedIndex];
	if (entry.directory) {
		MediaPickerNavigate(context, entry.path);
		return;
	}
	context->selectedPath = entry.path;
	context->accepted = true;
	DestroyWindow(context->window);
}

LRESULT CALLBACK MediaPickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* context = reinterpret_cast<MediaPickerContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		context = reinterpret_cast<MediaPickerContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		if (context) context->window = window;
		JiYuWindowCapture::ExcludeWindowFromCapture(window);
	}
	if (!context) return DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_CREATE:
		context->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
		context->titleFont = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
		{
			wchar_t modulePath[32768] = {};
			GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
			wchar_t* slash = wcsrchr(modulePath, L'\\');
			if (slash) *slash = L'\0';
			MediaPickerNavigate(context, slash ? modulePath : L"");
		}
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		HDC dc = BeginPaint(window, &paint);
		MediaPickerPaint(context, dc);
		EndPaint(window, &paint);
		return 0;
	}
	case WM_MOUSEWHEEL: {
		const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
		context->scrollOffset = (std::max)(0, context->scrollOffset - notches * 3);
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (PtInRect(&context->upRect, point)) {
			if (context->currentDirectory.empty()) MediaPickerNavigate(context, L"");
			else if (context->currentDirectory.size() <= 3 && context->currentDirectory[1] == L':') MediaPickerNavigate(context, L"");
			else {
				const size_t slash = context->currentDirectory.find_last_of(L"\\/");
				MediaPickerNavigate(context, slash == std::wstring::npos ? L"" : context->currentDirectory.substr(0, slash == 2 ? 3 : slash));
			}
			return 0;
		}
		if (PtInRect(&context->useRect, point)) { MediaPickerAccept(context); return 0; }
		if (PtInRect(&context->cancelRect, point)) { DestroyWindow(window); return 0; }
		if (PtInRect(&context->listRect, point)) {
			const int row = (point.y - context->listRect.top) / 32;
			const int index = context->scrollOffset + row;
			if (index >= 0 && index < static_cast<int>(context->entries.size())) {
				context->selectedIndex = index;
				if (message == WM_LBUTTONDBLCLK) MediaPickerAccept(context);
				else InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		break;
	}
	case WM_CLOSE: DestroyWindow(window); return 0;
	case WM_DESTROY:
		if (context->font) DeleteObject(context->font);
		if (context->titleFont) DeleteObject(context->titleFont);
		context->font = nullptr;
		context->titleFont = nullptr;
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

bool ChooseMediaPath(HWND owner, bool image, std::wstring* path)
{
	if (!path) return false;
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = MediaPickerProc;
		windowClass.hInstance = JTAppGetInstanceDirect();
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_APP));
		windowClass.hIconSm = windowClass.hIcon;
		windowClass.lpszClassName = kMediaPickerClass;
		if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		registered = true;
	}
	MediaPickerContext context;
	context.owner = owner;
	context.image = image;
	RECT work{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const int width = 720;
	const int height = 560;
	const int x = work.left + ((work.right - work.left) - width) / 2;
	const int y = work.top + ((work.bottom - work.top) - height) / 2;
	HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW | WS_EX_TOPMOST, kMediaPickerClass,
		image ? L"选择图片" : L"选择视频", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
		x, y, width, height, owner, nullptr, JTAppGetInstanceDirect(), &context);
	if (!window) return false;
	const BOOL ownerWasEnabled = owner && IsWindowEnabled(owner);
	if (ownerWasEnabled) EnableWindow(owner, FALSE);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	SetForegroundWindow(window);
	MSG message{};
	while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	if (ownerWasEnabled && IsWindow(owner)) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
	if (!context.accepted) return false;
	*path = std::move(context.selectedPath);
	return true;
}

std::unique_ptr<Bitmap> TeacherBitmapFromFrame(const jiyu::RemoteFrame& frame)
{
	if (frame.width <= 0 || frame.height <= 0 ||
		frame.rgb.size() < static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u)
		return {};
	auto bitmap = std::make_unique<Bitmap>(frame.width, frame.height, PixelFormat32bppARGB);
	if (!bitmap || bitmap->GetLastStatus() != Ok) return {};
	BitmapData data{};
	const Rect rect(0, 0, frame.width, frame.height);
	if (bitmap->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok) return {};
	for (int y = 0; y < frame.height; ++y) {
		auto* dst = reinterpret_cast<std::uint8_t*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
		const auto* src = frame.rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 3u;
		for (int x = 0; x < frame.width; ++x) {
			dst[x * 4 + 0] = src[x * 3 + 2];
			dst[x * 4 + 1] = src[x * 3 + 1];
			dst[x * 4 + 2] = src[x * 3 + 0];
			dst[x * 4 + 3] = 255;
		}
	}
	bitmap->UnlockBits(&data);
	return bitmap;
}

std::wstring StripMarkup(LPCWSTR input) {
	std::wstring result;
	bool inTag = false;
	for (const wchar_t* p = input ? input : L""; *p; ++p) {
		if (*p == L'<') inTag = true;
		else if (*p == L'>') inTag = false;
		else if (!inTag) result.push_back(*p);
	}
	return result;
}

struct TeacherWindowSearch {
	DWORD processId = 0;
	HWND window = nullptr;
};

BOOL CALLBACK FindTeacherWindow(HWND hwnd, LPARAM parameter)
{
	auto* search = reinterpret_cast<TeacherWindowSearch*>(parameter);
	if (!search) return TRUE;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	wchar_t className[64]{};
	if (pid != search->processId || GetClassNameW(hwnd, className, _countof(className)) == 0)
		return TRUE;
	// JiYuTeacherGui is a console-subsystem executable. Its console is not part
	// of the teacher UI and must never become visible beside the embedded FLTK
	// window, even when it was created before CREATE_NO_WINDOW took effect.
	if (lstrcmpW(className, L"ConsoleWindowClass") == 0) {
		ShowWindow(hwnd, SW_HIDE);
		return TRUE;
	}
	if (search->window || lstrcmpW(className, L"FLTK") != 0 || GetWindow(hwnd, GW_OWNER) != nullptr)
		return TRUE;
	RECT client{};
	if (GetClientRect(hwnd, &client) && client.right >= 320 && client.bottom >= 200) {
		search->window = hwnd;
	}
	return TRUE;
}

}

struct MainWindow::TeacherBackend {
	jiyu::TeacherService service;
	jiyu::TeacherServiceOptions options;
	std::vector<jiyu::StudentInfo> students;
	std::vector<jiyu::RemoteFrame> remote_frames;
	std::string list_signature;
	std::string selected_ip;
	std::filesystem::path latest_preview;
	std::filesystem::path displayed_preview;
	std::unique_ptr<Bitmap> preview_bitmap;
	std::unique_ptr<Bitmap> remote_bitmap;
	std::string remote_ip;
	std::uint32_t remote_sequence = 0;
	bool running = false;
};

MainWindow::MainWindow()
{
	// Register the object before CreateWindowExW can synchronously deliver WM_NCCREATE.
	// The window manager's CREATESTRUCT payload was observed corrupted in the failing
	// build, while the constructor's this pointer is still valid at this point.
	currentMainWindow = this;
	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
	GdiplusStartupInput input;
	GdiplusStartup(&gdiplusToken, &input, nullptr);
	if (!RegisterWindowClass()) return;

	RECT desired{ 0, 0, 1120, 760 };
	AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
	_hWnd = CreateWindowExW(0, MAIN_WND_CLS_NAME, MAIN_WND_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
		nullptr, nullptr, JTAppGetInstanceDirect(), this);
	if (!_hWnd) return;

	Initialize();
	ShowWindow(_hWnd, JTAppGetShowCmdDirect());
	UpdateWindow(_hWnd);
}

MainWindow::~MainWindow()
{
	if (currentLogger) {
		// App::ExitClear and background workers can log after the UI loop exits.
		// Detach before any member is torn down so no callback reaches this object.
		currentLogger->SetLogOutPutCallback(nullptr, 0);
		currentLogger->SetLogOutPut(LogOutPutFile);
		currentLogger->LogInfo(L"UI teardown started; logger restored to file output");
	}
	StopAvProcessScan();
	StopEmbeddedTeacherGui();
	StopTeacherService();
	if (controlSurfaceBrush) DeleteObject(controlSurfaceBrush);
	if (advancedFieldBrush) DeleteObject(advancedFieldBrush);
	if (controlFont) DeleteObject(controlFont);
	ReleaseBackBuffer();
	// Member destructors run AFTER this body, so every object holding a GDI+ type
	// must be released here. Destroying one after GdiplusShutdown touches torn-down
	// GDI+ state and crashes on exit — this is why quitting used to crash whenever a
	// replacement image had been picked (temporaryVideoPreview non-null) or the
	// teacher preview had loaded a frame (teacherBackend bitmaps non-null).
	temporaryVideoPreview.reset();
	if (teacherBackend) {
		teacherBackend->preview_bitmap.reset();
		teacherBackend->remote_bitmap.reset();
	}
	// 文本字体是文件级静态缓存，同样要在 GdiplusShutdown 之前清掉。
	ReleaseTextFontCache();
	UnloadMaterialIconFont();
	if (gdiplusToken) GdiplusShutdown(gdiplusToken);
	if (currentMainWindow == this) currentMainWindow = nullptr;
}

bool MainWindow::RegisterWindowClass()
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	// 不要再加 CS_HREDRAW/CS_VREDRAW：那会让窗口每拖动一个像素就整窗重绘，
	// 配合整页 GDI+ 重画，拖拽缩放时明显掉帧。WM_SIZE 里已经自己 Invalidate 了。
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = JTAppGetInstanceDirect();
	wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
	wc.hIconSm = wc.hIcon;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = MAIN_WND_CLS_NAME;
	return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool MainWindow::Initialize()
{
	LoadSettings();
	LoadMaterialIconFont(JTAppGetInstanceDirect());
	currentLogger = JTAppGetLoggerDirect();
	::currentLogger = currentLogger;
	currentLogger->SetLogOutPut(LogOutPutCallback);
	currentLogger->SetLogOutPutCallback(LogCallBack, reinterpret_cast<LPARAM>(this));
	currentWorker = JTAppGetTrainerWorkerDirect();
	if (currentWorker) currentWorker->SetUpdateInfoCallback(this);
	// WM_COPYDATA uses WM_COPYGLOBALDATA internally when data crosses integrity levels.
	CHANGEFILTERSTRUCT copyDataFilter{ sizeof(copyDataFilter) };
	SetLastError(ERROR_SUCCESS);
	const BOOL copyDataAllowed = ChangeWindowMessageFilterEx(
		_hWnd, WM_COPYDATA, MSGFLT_ALLOW, &copyDataFilter);
	const DWORD copyDataError = GetLastError();

	CHANGEFILTERSTRUCT globalDataFilter{ sizeof(globalDataFilter) };
	SetLastError(ERROR_SUCCESS);
	const BOOL globalDataAllowed = ChangeWindowMessageFilterEx(
		_hWnd, WM_COPYGLOBALDATA_COMPAT, MSGFLT_ALLOW, &globalDataFilter);
	const DWORD globalDataError = GetLastError();

	currentLogger->LogInfo(
		L"Control reply filter copyData=%d error=%lu status=%lu globalData=%d error=%lu status=%lu",
		copyDataAllowed, copyDataError, copyDataFilter.ExtStatus,
		globalDataAllowed, globalDataError, globalDataFilter.ExtStatus);
	CreateNetworkControls();
	CreateAdvancedControls();
	InstallInputOriginHooks();

	SetTimer(_hWnd, TIMER_SERVICE_POLL, 200, nullptr);
	int corner = 2;
	DwmSetWindowAttribute(_hWnd, 33, &corner, sizeof(corner));
	OnFirstShow();
	return true;
}

bool MainWindow::IsProtectedInputTarget(HWND target, DWORD* targetPid)
{
	if (!target) return false;
	HWND root = GetAncestor(target, GA_ROOT);
	if (root) target = root;
	DWORD pid = 0;
	GetWindowThreadProcessId(target, &pid);
	if (targetPid) *targetPid = pid;
	// Guard this program's own windows only. Synthetic input aimed at any other
	// application — StudentMain included — is deliberately left alone: the guard
	// exists to keep automation from driving our own UI, not to interfere with
	// other software's windows.
	return pid != 0 && pid == GetCurrentProcessId();
}

void MainWindow::NotifyInjectedInput(bool mouse, DWORD targetPid)
{
	MainWindow* self = inputGuardWindow;
	if (!self || !self->_hWnd) return;
	if (InterlockedCompareExchange(&inputGuardNoticePending, 1, 0) != 0) return;
	PostMessageW(self->_hWnd, WM_BLOCKED_INJECTED_INPUT, mouse ? 1 : 0, targetPid);
}

LRESULT CALLBACK MainWindow::KeyboardInputProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0 && lParam) {
		const KBDLLHOOKSTRUCT* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
		const bool injected = (event->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
		if (injected) {
			DWORD targetPid = 0;
			if (IsProtectedInputTarget(GetForegroundWindow(), &targetPid)) {
				NotifyInjectedInput(false, targetPid);
				return 1;
			}
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK MainWindow::MouseInputProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0 && lParam) {
		const MSLLHOOKSTRUCT* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
		const bool injected = (event->flags & LLMHF_INJECTED) != 0;
		const bool click = wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
			wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP ||
			wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP ||
			wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP;
		if (injected && click) {
			DWORD targetPid = 0;
			if (IsProtectedInputTarget(WindowFromPoint(event->pt), &targetPid)) {
				NotifyInjectedInput(true, targetPid);
				return 1;
			}
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD WINAPI MainWindow::InputHookThreadProc(LPVOID parameter)
{
	MainWindow* self = static_cast<MainWindow*>(parameter);
	if (!self) return ERROR_INVALID_PARAMETER;
	MSG message{};
	PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	HINSTANCE module = JTAppGetInstanceDirect();
	self->keyboardInputHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardInputProc, module, 0);
	self->mouseInputHook = SetWindowsHookExW(WH_MOUSE_LL, MouseInputProc, module, 0);
	self->inputHookInstallError = (!self->keyboardInputHook || !self->mouseInputHook)
		? GetLastError() : ERROR_SUCCESS;
	if (self->inputHookReadyEvent) SetEvent(self->inputHookReadyEvent);

	if (self->keyboardInputHook && self->mouseInputHook) {
		while (GetMessageW(&message, nullptr, 0, 0) > 0) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (self->keyboardInputHook) {
		UnhookWindowsHookEx(self->keyboardInputHook);
		self->keyboardInputHook = nullptr;
	}
	if (self->mouseInputHook) {
		UnhookWindowsHookEx(self->mouseInputHook);
		self->mouseInputHook = nullptr;
	}
	return self->inputHookInstallError;
}

bool MainWindow::InstallInputOriginHooks()
{
	if (inputHookThread && keyboardInputHook && mouseInputHook) return true;
	inputGuardWindow = this;
	inputHookInstallError = ERROR_SUCCESS;
	inputHookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!inputHookReadyEvent) {
		currentLogger->LogError2(L"Unable to create input hook ready event: %lu", GetLastError());
		return false;
	}
	inputHookThread = CreateThread(nullptr, 0, InputHookThreadProc, this, 0, &inputHookThreadId);
	if (!inputHookThread || WaitForSingleObject(inputHookReadyEvent, 3000) != WAIT_OBJECT_0 ||
		!keyboardInputHook || !mouseInputHook) {
		const DWORD error = inputHookInstallError != ERROR_SUCCESS ? inputHookInstallError : GetLastError();
		RemoveInputOriginHooks();
		currentLogger->LogError2(L"Unable to install input origin hooks: %lu", error);
		return false;
	}
	CloseHandle(inputHookReadyEvent);
	inputHookReadyEvent = nullptr;
	currentLogger->LogInfo(L"Input origin guard installed on worker thread=%lu", inputHookThreadId);
	return true;
}

void MainWindow::RemoveInputOriginHooks()
{
	if (inputHookThread && inputHookThreadId) {
		PostThreadMessageW(inputHookThreadId, WM_QUIT, 0, 0);
		WaitForSingleObject(inputHookThread, 3000);
		CloseHandle(inputHookThread);
		inputHookThread = nullptr;
	}
	if (inputHookReadyEvent) CloseHandle(inputHookReadyEvent);
	inputHookReadyEvent = nullptr;
	inputHookThreadId = 0;
	inputHookInstallError = ERROR_SUCCESS;
	inputGuardWindow = nullptr;
	InterlockedExchange(&inputGuardNoticePending, 0);
}
MainWindow* MainWindow::FromWindow(HWND hwnd)
{
	return reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	MainWindow* self = FromWindow(hwnd);
	if (message == WM_NCCREATE) {
		auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
		// CreateWindowExW invokes WM_NCCREATE synchronously. Prefer the object
		// registered by the constructor and only fall back to lpCreateParams for
		// callers that create this class through the normal Win32 convention.
		self = currentMainWindow;
		if (!self && cs) self = static_cast<MainWindow*>(cs->lpCreateParams);
		if (!self) return FALSE;
		self->_hWnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
#if DZJS_ENABLE_CAPTURE_EXCLUSION
		JiYuWindowCapture::ExcludeWindowFromCapture(hwnd);
#endif
	}
	if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
	if (self->WM_TASKBARCREATED != 0 && message == static_cast<UINT>(self->WM_TASKBARCREATED)) {
		self->CreateTrayIcon();
		return 0;
	}

	switch (message) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: self->Paint(); return 0;
	case WM_SIZE: {
		// 最小化时客户区是 0x0，没必要重排控件，也避免把后备缓冲建成 0 尺寸。
		if (wParam == SIZE_MINIMIZED) return 0;
		self->LayoutEmbeddedWindows();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_GETMINMAXINFO: {
		auto info = reinterpret_cast<MINMAXINFO*>(lParam);
		info->ptMinTrackSize = { 960, 760 };
		return 0;
	}
	case WM_MOUSEMOVE: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		self->UpdateHover(point);
		if (!self->mouseTracking) {
			TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
			self->mouseTracking = TrackMouseEvent(&track) != FALSE;
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		self->mouseTracking = false;
		self->hoverTarget = -1;
		self->pressTarget = -1;
		self->EnsureAnimationTimer();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_MOUSEWHEEL: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &point);
		if (self->page == Page::Diagnostics && PtInRect(&self->diagnosticsListRect, point) &&
			!self->diagnosticsVisibleDrivers.empty()) {
			const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
			const int maxOffset = std::max(0, static_cast<int>(self->diagnosticsVisibleDrivers.size()) - self->diagnosticsVisibleRowCount);
			self->diagnosticsListOffset = std::max(0, std::min(maxOffset, self->diagnosticsListOffset - notches * 3));
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		break;
	}
	case WM_NCLBUTTONDOWN:
		if (wParam == HTCLOSE) {
			self->closeButtonClickPending = true;
		}
		break;
	case WM_NCLBUTTONUP:
		if (wParam == HTCLOSE) {
			// Keep the marker through WM_SYSCOMMAND/SC_CLOSE. Any close message
			// without this real non-client gesture is treated as external.
			self->closeButtonClickPending = true;
		}
		else {
			self->closeButtonClickPending = false;
		}
		break;
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_CLOSE && !self->closeButtonClickPending)
			return 0;
		break;
	case WM_LBUTTONDOWN: {
		// 只是记住按下的目标，用来给按钮画"按下去"的样子；真正的动作仍在抬起时执行。
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (self->UpdateDialogVisible()) {
			self->pressTarget = self->HitUpdateDialogAt(point);
			if (self->pressTarget >= 0) {
				self->EnsureAnimationTimer();
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		}
		self->focusTarget = -1;
		self->pressTarget = self->HitTargetAt(point);
		if (self->pressTarget >= 0) {
			self->EnsureAnimationTimer();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		self->HandleClick(point);
		self->pressTarget = -1;
		self->EnsureAnimationTimer();
		return 0;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		// 录制快捷键时，主键先归这里，别让 DefWindowProc 抢去（Alt 会拉系统菜单）。
		if (self->hotkeyCaptureIndex >= 0) {
			self->HandleHotkeyKey(static_cast<UINT>(wParam));
			return 0;
		}
		// 弹窗上的 Esc：等同关掉/取消，不让它去清焦点环。
		if (self->UpdateDialogVisible() && wParam == VK_ESCAPE) {
			if (self->updateDialog.mode == UpdateDialogMode::Confirm) {
				self->CloseUpdateDialog();
				self->ShowFastTip(L"已取消更新");
			}
			else if (self->updateDialog.mode == UpdateDialogMode::Failed) {
				self->CloseUpdateDialog();
			}
			return 0;
		}
		// 键盘操作：Tab 走焦点，Enter/Space 触发，Esc 取消。
		if (wParam == VK_TAB) {
			self->MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
			return 0;
		}
		if ((wParam == VK_RETURN || wParam == VK_SPACE) && self->ActivateFocus()) {
			return 0;
		}
		if (wParam == VK_ESCAPE && self->focusTarget >= 0) {
			self->focusTarget = -1;
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		break;
	case WM_KILLFOCUS:
		if (self->hotkeyCaptureIndex >= 0) {
			self->hotkeyCaptureIndex = -1;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		break;
	case WM_TIMER: self->OnWmTimer(wParam); return 0;
	case WM_COMMAND: self->OnWmCommand(wParam); return 0;
	case WM_DRAWITEM: {
		auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
		if (item && ((item->CtlID >= IDC_NET_DETECT_PORT && item->CtlID <= IDC_NET_SCAN) || item->CtlID == IDC_NET_TEACHER ||
			(item->CtlID >= IDC_TEACHER_REFRESH && item->CtlID <= IDC_TEACHER_REBOOT) || item->CtlID == IDC_TEACHER_VIEW || item->CtlID == IDC_TEACHER_CHAT_SEND)) {
			DrawNativeButton(item, self->controlFont, self->darkMode, NativeButtonHoverMap()[item->hwndItem]);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	// STATIC 也要接管，否则教师端宿主框（WS_EX_CLIENTEDGE）会用系统灰
	// COLOR_BTNFACE 画底，把下面自绘的圆角卡片盖成一个灰方块。
	case WM_CTLCOLORSTATIC: {
		HDC controlDc = reinterpret_cast<HDC>(wParam);
		// 高级配置页的输入框要跟自绘的圆角框同色，否则控件那块方底色会顶出来。
		const int controlId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
		if (controlId == IDC_ADV_INTERVAL) {
			SetTextColor(controlDc, self->darkMode ? RGB(222, 230, 226) : RGB(35, 42, 39));
			SetBkColor(controlDc, self->darkMode ? RGB(26, 33, 30) : RGB(255, 255, 255));
			if (!self->advancedFieldBrush) break;
			return reinterpret_cast<LRESULT>(self->advancedFieldBrush);
		}
		SetTextColor(controlDc, self->darkMode ? RGB(222, 230, 226) : RGB(35, 42, 39));
		SetBkColor(controlDc, self->darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
		if (!self->controlSurfaceBrush) break;
		return reinterpret_cast<LRESULT>(self->controlSurfaceBrush);
	}
	case WM_COPYDATA: {
		auto data = reinterpret_cast<PCOPYDATASTRUCT>(lParam);
		if (!data || !data->lpData || data->cbData < sizeof(wchar_t)) return FALSE;
		wchar_t messageBuffer[256]{};
		const size_t sourceChars = data->cbData / sizeof(wchar_t);
		const size_t copyChars = std::min(sourceChars, _countof(messageBuffer) - 1);
		memcpy(messageBuffer, data->lpData, copyChars * sizeof(wchar_t));
		messageBuffer[copyChars] = L'\0';
		if (self->currentWorker) self->currentWorker->HandleMessageFromVirus(messageBuffer);
		return TRUE;
	}
	case WM_HOTKEY: self->OnWmHotKey(wParam); return 0;
	case WM_USER: self->OnWmUser(wParam, lParam); return 0;
	case WM_NATIVE_REFRESH: InvalidateRect(hwnd, nullptr, FALSE); return 0;
	case WM_AV_SCAN_FINISHED: self->FinishAvProcessScan(); return 0;
	case WM_UPDATE_DOWNLOAD_STATE:
		self->OnUpdateDownloadState(wParam, lParam);
		return 0;
	case WM_JUP_DOWNLOAD_PROGRESS:
		// 下载子进程发来的真实进度。跨进程消息，wParam/lParam 里只有数值。
		self->OnChildDownloadProgress(wParam, lParam);
		return 0;
	case WM_UPDATE_PROBE_DONE: {
		// 探测线程把结果投回来了。上下文由本线程负责释放。
		UpdateProbeContext* context = reinterpret_cast<UpdateProbeContext*>(lParam);
		if (context) {
			self->FinishUpdateProbe(*context);
			delete context;
		}
		return 0;
	}
	case WM_BLOCKED_INJECTED_INPUT: {
		InterlockedExchange(&inputGuardNoticePending, 0);
		// The guard only ever covers this program's own windows, so there is no
		// per-target caption to pick any more.
		MessageBoxW(hwnd,
			wParam ? L"\u5df2\u62e6\u622a\u9488\u5bf9\u672c\u7a0b\u5e8f\u7a97\u53e3\u7684 API \u6a21\u62df\u9f20\u6807\u8f93\u5165\u3002" : L"\u5df2\u62e6\u622a\u9488\u5bf9\u672c\u7a0b\u5e8f\u7a97\u53e3\u7684 API \u6a21\u62df\u952e\u76d8\u8f93\u5165\u3002",
			L"Dzjs Trainer \u8f93\u5165\u4fdd\u62a4",
			MB_OK | MB_ICONWARNING | MB_TOPMOST);
		return 0;
	}
	case WM_MY_GET_SM_TICP_PORT_FINISH:
		if (wParam == NOT_FOUND_JY_PORT) self->AppendNetworkResult(L"\u672a\u627e\u5230 StudentMain \u8fdb\u7a0b");
		else if (wParam == FAILED_FOUND_JY_PORT) self->AppendNetworkResult(L"\u83b7\u53d6 TCP \u7aef\u53e3\u5931\u8d25");
		else {
			wchar_t port[16]{};
			swprintf_s(port, L"%u", static_cast<unsigned int>(wParam));
			SetWindowTextW(GetDlgItem(hwnd, IDC_NET_PORT), port);
			self->AppendNetworkResult(L"\u5df2\u68c0\u6d4b\u5230 StudentMain \u7aef\u53e3");
		}
		return 0;
	case WM_MY_SEND_ADD_RESULT: {
		auto result = reinterpret_cast<std::wstring*>(wParam);
		if (result) { self->AppendNetworkResult(result->c_str()); delete result; }
		return 0;
	}
	case WM_MY_SCAN_IP_ADD: {
		auto data = reinterpret_cast<JyNetworkIP*>(wParam);
		if (data) {
			std::wstring line = data->ipAddress;
			if (!data->hostName.empty()) line += L"  \u00b7  " + data->hostName;
			self->AppendNetworkResult(line.c_str());
			delete data;
		}
		return 0;
	}
	case WM_MY_SCAN_IP_FINISH: self->AppendNetworkResult(L"\u5c40\u57df\u7f51\u626b\u63cf\u5b8c\u6210"); return 0;
	case WM_CLOSE:
		if (!self->closeButtonClickPending) return 0;
		self->closeButtonClickPending = false;
		ShowWindow(hwnd, SW_HIDE);
		if (!self->hideTipShowed) {
		self->ShowTrayBalloon(L"Dzjs Trainer", L"\u7a0b\u5e8f\u5df2\u7f29\u5c0f\u5230\u901a\u77e5\u533a\u57df\uff0c\u53cc\u51fb\u56fe\u6807\u53ef\u6062\u590d\u3002");
			self->hideTipShowed = true;
		}
		return 0;
	case WM_DESTROY: self->OnWmDestroy(); return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::ReleaseBackBuffer()
{
	// Bitmap 只是包住 DIB 的内存，必须先于 DIB 释放，否则析构时会碰到已回收的像素。
	delete backBuffer;
	backBuffer = nullptr;
	if (backBufferDc) {
		if (backBufferPreviousBitmap) SelectObject(backBufferDc, backBufferPreviousBitmap);
		backBufferPreviousBitmap = nullptr;
		DeleteDC(backBufferDc);
		backBufferDc = nullptr;
	}
	if (backBufferDib) {
		DeleteObject(backBufferDib);
		backBufferDib = nullptr;
	}
	backBufferBits = nullptr;
	backBufferWidth = 0;
	backBufferHeight = 0;
}

bool MainWindow::EnsureBackBuffer(int width, int height)
{
	if (backBuffer && backBufferWidth == width && backBufferHeight == height) return true;
	ReleaseBackBuffer();
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height; // 负值 = 自上而下，行序与 GDI+ 一致
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	HDC screen = GetDC(nullptr);
	if (!screen) return false;
	backBufferDib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &backBufferBits, nullptr, 0);
	backBufferDc = CreateCompatibleDC(screen);
	ReleaseDC(nullptr, screen);
	if (!backBufferDib || !backBufferDc || !backBufferBits) {
		ReleaseBackBuffer();
		return false;
	}
	backBufferPreviousBitmap = SelectObject(backBufferDc, backBufferDib);
	backBuffer = new Bitmap(width, height, width * 4, PixelFormat32bppPARGB,
		static_cast<BYTE*>(backBufferBits));
	if (backBuffer->GetLastStatus() != Ok) {
		ReleaseBackBuffer();
		return false;
	}
	backBufferWidth = width;
	backBufferHeight = height;
	return true;
}

void MainWindow::Paint()
{
	static LARGE_INTEGER frequency{};
	if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER paintStarted{};
	QueryPerformanceCounter(&paintStarted);
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(_hWnd, &ps);
	{
		const ULONGLONG now = GetTickCount64();
		if (lastPaintTick != 0) {
			const double delta = static_cast<double>(now - lastPaintTick) / 1000.0;
			frameDelta = (std::max)(0.001, (std::min)(0.05, delta));
		}
		lastPaintTick = now;
	}
	RECT client{};
	GetClientRect(_hWnd, &client);
	const int width = client.right;
	const int height = client.bottom;
	if (width <= 0 || height <= 0 || !EnsureBackBuffer(width, height)) {
		EndPaint(_hWnd, &ps);
		return;
	}
	Graphics g(backBuffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	// ClearType assumes an opaque target; the ARGB back buffer needs alpha-safe antialiasing.
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	// 三层表面明度：侧栏 (226,234,230) 低于 画布 (237,242,239) 低于 卡片 (255,255,255)。
	// 原来画布 (246,248,246) 与白卡只差 9 级，卡片完全浮不起来。
	g.Clear(C(darkMode, 237, 242, 239, 15, 18, 17));

	PaintNavigation(g, height);
	PaintHeader(g, width);
	const double easedMotion = 1.0 - pow(1.0 - pageMotion, 4.0);
	const int offset = static_cast<int>((1.0 - easedMotion) * 22.0);
	switch (page) {
	case Page::Overview: PaintOverview(g, width, height, offset); break;
	case Page::Protection: PaintProtection(g, width, height, offset); break;
	case Page::Replacement: PaintProtection(g, width, height, offset); break;
	case Page::Antivirus: PaintAntivirus(g, width, height, offset); break;
	case Page::Advanced: PaintAdvanced(g, width, height, offset); break;
	case Page::Help: PaintHelp(g, width, height, offset); break;
	case Page::Network: PaintNetwork(g, width, height, offset); break;
	case Page::Diagnostics: PaintDiagnostics(g, width, height, offset); break;
	case Page::Logs: PaintLogs(g, width, height, offset); break;
	case Page::About: PaintAbout(g, width, height, offset); break;
	}
	// 键盘焦点环。只在 Tab 导航时出现，鼠标点击会清掉。
	// 更新弹窗有自己的一套焦点环（画在卡片里），这里跳过，免得环被卡片盖住。
	if (focusTarget >= 0 && !UpdateDialogVisible()) {
		RECT focusRect{};
		if (TargetRect(focusTarget, &focusRect)) {
			const RectF ring(static_cast<REAL>(focusRect.left) - 3.0f, static_cast<REAL>(focusRect.top) - 3.0f,
				static_cast<REAL>(focusRect.right - focusRect.left) + 6.0f,
				static_cast<REAL>(focusRect.bottom - focusRect.top) + 6.0f);
			const float ringRadius = (std::min)(ring.Height / 2.0f, 20.0f);
			StrokeRound(g, ring, ringRadius, C(darkMode, 0, 107, 95, 130, 213, 199), 2.0f);
		}
	}
	PaintToast(g, width, height);
	// 弹窗画在最后：提示条、页面内容、焦点环都得在它下面。
	PaintUpdateDialog(g, width, height);
	// 直接 BitBlt 到窗口 DC：比 Graphics::DrawImage 快约 45 倍。
	// BeginPaint 拿到的 DC 已经按更新区裁剪，被子控件挡住的部分不会覆盖。
	BitBlt(dc, 0, 0, width, height, backBufferDc, 0, 0, SRCCOPY);
	EndPaint(_hWnd, &ps);
	LARGE_INTEGER paintEnded{};
	QueryPerformanceCounter(&paintEnded);
	LogPaintDuration(static_cast<double>(paintEnded.QuadPart - paintStarted.QuadPart) * 1000.0 /
		static_cast<double>(frequency.QuadPart));
}

void MainWindow::LogPaintDuration(double milliseconds)
{
	// 只在连续动画（悬停渐变、翻页）期间才有参考价值，所以按帧数汇总，
	// 每 300 帧输出一行，平时几乎不会写日志。
	static double totalMs = 0.0;
	static double worstMs = 0.0;
	static int frames = 0;
	totalMs += milliseconds;
	if (milliseconds > worstMs) worstMs = milliseconds;
	if (++frames < 300) return;
	if (currentLogger) {
		currentLogger->LogInfo(L"UI 帧耗时：平均 %.2f ms / 最慢 %.2f ms（%d 帧）",
			totalMs / frames, worstMs, frames);
	}
	totalMs = 0.0;
	worstMs = 0.0;
	frames = 0;
}

void MainWindow::PaintNavigation(Graphics& g, int height)
{
	const bool d = darkMode;
	SolidBrush side(C(d, 226, 234, 230, 24, 29, 27));
	g.FillRectangle(&side, 0, 0, 220, height);

	{
		// 品牌块用同色系线性渐变，比纯色方块有质感得多。
		LinearGradientBrush logoBrush(PointF(20.0f, 18.0f), PointF(64.0f, 62.0f),
			C(d, 0, 133, 117, 148, 233, 219), C(d, 0, 76, 67, 100, 186, 173));
		FillRound(g, RectF(20, 18, 44, 44), 12, logoBrush);
	}
	Text(g, L"J", 20, 24, 44, 32, 21, FontStyleBold, C(d, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
	Text(g, L"Dzjs Trainer", 76, 21, 128, 24, 15, FontStyleBold, C(d, 26, 29, 28, 226, 232, 229));
	Text(g, L"\u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3", 76, 44, 128, 18, 11, FontStyleRegular, C(d, 67, 72, 70, 191, 200, 196));
	Text(g, L"\u5de5\u4f5c\u533a", 24, 94, 150, 18, 11, FontStyleBold, C(d, 91, 97, 94, 164, 174, 169));

	const int navIcons[] = { 0, 1, 5, 5, 6, 3, 0, 2, 3, 6 };
	for (int i = 0; i < 10; ++i) {
		const float y = 110.0f + i * 45.0f;
		navRects[i] = { 12, static_cast<LONG>(y), 208, static_cast<LONG>(y + 40) };
		const bool active = static_cast<int>(page) == i;
		if (!active && hoverProgress[i] > 0.002f) {
			FillRound(g, RectF(12, y, 196, 40), 20,
				Blend(C(d, 235, 241, 238, 27, 33, 31), C(d, 220, 229, 225, 39, 47, 44), hoverProgress[i]));
		}
	}
	{
		// 选中胶囊按浮点位置绘制，翻页时从上一项滑到下一项，不是瞬间跳。
		const float pillY = 110.0f + navIndicator * 45.0f;
		FillRound(g, RectF(12, pillY, 196, 40), 20, C(d, 183, 232, 222, 42, 79, 72));
	}
	for (int i = 0; i < 10; ++i) {
		const float y = 110.0f + i * 45.0f;
		const bool active = static_cast<int>(page) == i;
		const Color navColor = active ? C(d, 0, 81, 72, 156, 240, 225) : C(d, 58, 64, 61, 197, 207, 202);
		DrawIcon(g, navIcons[i], 30, y + 10, navColor, 18);
		Text(g, kNavLabels[i], 66, y + 9, 126, 22, 11, active ? FontStyleBold : FontStyleRegular, navColor);
	}

	FillRound(g, RectF(16, static_cast<float>(height - 82), 188, 62), 12, C(d, 255, 255, 255, 33, 40, 37));
	StrokeRound(g, RectF(16, static_cast<float>(height - 82), 188, 62), 12, C(d, 216, 228, 222, 48, 56, 52));
	SolidBrush online(C(d, 0, 107, 95, 130, 213, 199));
	g.FillEllipse(&online, 32, height - 59, 10, 10);
	Text(g, L"\u9632\u62a4\u670d\u52a1\u5df2\u5c31\u7eea", 54, static_cast<float>(height - 66), 136, 20, 11, FontStyleBold, C(d, 35, 42, 39, 222, 230, 226));
	Text(g, NavVersionLine().c_str(), 54, static_cast<float>(height - 45), 136, 16, 10, FontStyleRegular, C(d, 91, 99, 95, 166, 177, 171));
}

void MainWindow::PaintHeader(Graphics& g, int width)
{
	const int pageIndex = static_cast<int>(page);
	const bool topMostEnabled = setTopMost && IsWindowActuallyTopmost(_hWnd);
	const wchar_t* topMostText = !topMostEnabled
		? L"\u8d85\u7ea7\u7f6e\u9876\uff1a\u672a\u5f00\u542f"
		: (topMostUiAccessBand ? L"\u8d85\u7ea7\u7f6e\u9876\uff1a\u5df2\u5f00\u542f" : L"\u666e\u901a\u7f6e\u9876\uff1a\u5df2\u5f00\u542f");
	Text(g, kPageTitles[pageIndex], 248, 22, 430, 34, 23, FontStyleBold, C(darkMode, 28, 32, 30, 227, 233, 230));
	Text(g, page == Page::Overview ? L"\u5f53\u524d\u8bbe\u5907\u7684\u8fd0\u884c\u4e0e\u9632\u62a4\u72b6\u6001" : L"Dzjs Trainer  \u00b7  \u672c\u673a", 248, 55, 440, 18, 11 ,
		FontStyleRegular, C(darkMode, 86, 92, 89, 174, 184, 179));

	superTopMostRect = { width - 224, 20, width - 92, 68 };
	const float superX = static_cast<float>(width - 218);
	const bool superHover = hoverTarget == 80;
	FillRound(g, RectF(superX, 26, 126, 36), 18,
		topMostEnabled ? C(darkMode, 185, 226, 217, 43, 76, 69) :
			(superHover ? C(darkMode, 220, 237, 231, 49, 63, 58) : C(darkMode, 251, 253, 252, 33, 40, 37)));
	if (!topMostEnabled) {
		StrokeRound(g, RectF(superX, 26, 126, 36), 18, C(darkMode, 219, 228, 223, 48, 56, 52));
	}
	Text(g, topMostText, superX, 36, 126, 18, 11, FontStyleBold,
		topMostEnabled ? C(darkMode, 0, 81, 72, 155, 239, 224) : C(darkMode, 42, 48, 45, 220, 229, 225), StringAlignmentCenter);
	themeRect = { width - 80, 20, width - 24, 68 };
	const bool themeHover = hoverTarget == 41;
	// 常驻一个浅色圆形底，否则月亮图标孤零零浮在标题栏里，看不出是可点的按钮。
	FillRound(g, RectF(static_cast<float>(width - 78), 22, 44, 44), 22,
		themeHover ? C(darkMode, 224, 233, 228, 41, 50, 46) : C(darkMode, 246, 249, 247, 30, 36, 34));
	DrawIcon(g, darkMode ? 8 : 9, static_cast<float>(width - 66), 34, C(darkMode, 52, 59, 56, 205, 215, 210), 20);
}

void MainWindow::PaintButton(Graphics& g, const RectF& rect, LPCWSTR label, int hoverId,
	ButtonStyle style, float radius, int icon, bool enabled)
{
	// 四种语义色 + 三态（常态/悬停/按下）。所有自绘按钮都走这里，
	// 免得每个页面各拼一套颜色，hover 有的一有的一无。
	const bool pressed = enabled && pressTarget == hoverId;
	const float hover = enabled ? hoverProgress[hoverId] : 0.0f;
	const float sink = pressed ? 1.0f : 0.0f;
	Color fill;
	Color textColor;
	switch (style) {
	case ButtonStyle::Primary:
		fill = pressed ? C(darkMode, 0, 74, 66, 108, 186, 173)
			: Blend(C(darkMode, 0, 107, 95, 130, 213, 199), C(darkMode, 0, 82, 73, 154, 232, 217), hover);
		textColor = C(darkMode, 255, 255, 255, 0, 55, 49);
		break;
	case ButtonStyle::Tonal:
		fill = pressed ? C(darkMode, 190, 222, 212, 42, 71, 64)
			: Blend(C(darkMode, 226, 241, 234, 43, 76, 69), C(darkMode, 206, 232, 222, 56, 98, 88), hover);
		textColor = C(darkMode, 0, 81, 72, 177, 239, 228);
		break;
	case ButtonStyle::Danger:
		fill = pressed ? C(darkMode, 232, 190, 182, 96, 52, 46)
			: Blend(C(darkMode, 246, 221, 214, 52, 50, 48), C(darkMode, 236, 198, 190, 76, 59, 54), hover);
		textColor = C(darkMode, 165, 48, 37, 240, 172, 158);
		break;
	default: // Ghost
		fill = pressed ? C(darkMode, 230, 237, 233, 40, 47, 44)
			: Blend(C(darkMode, 247, 250, 248, 32, 38, 35), C(darkMode, 233, 240, 236, 45, 53, 49), hover);
		textColor = C(darkMode, 52, 62, 57, 214, 224, 219);
		break;
	}
	if (!enabled) {
		fill = C(darkMode, 240, 244, 241, 34, 39, 37);
		textColor = C(darkMode, 156, 165, 160, 110, 120, 115);
	}
	const RectF body(rect.X, rect.Y + sink, rect.Width, rect.Height);
	if (enabled && (style == ButtonStyle::Primary || style == ButtonStyle::Tonal)) {
		SoftShadow(g, body, radius, darkMode, (pressed ? 0.30f : 0.50f) + hover * 0.40f);
	}
	FillRound(g, body, radius, fill);
	if (style == ButtonStyle::Ghost) {
		StrokeRound(g, body, radius, enabled ? C(darkMode, 219, 228, 223, 46, 55, 51)
			: C(darkMode, 232, 237, 234, 40, 47, 43));
	}
	const float textTop = rect.Y + sink + (rect.Height - 20.0f) / 2.0f;
	if (icon >= 0) {
		const Color iconColor = style == ButtonStyle::Primary ? textColor
			: (style == ButtonStyle::Danger ? textColor : C(darkMode, 0, 107, 95, 130, 213, 199));
		DrawIcon(g, icon, rect.X + 14.0f, rect.Y + sink + (rect.Height - 16.0f) / 2.0f,
			enabled ? iconColor : textColor, 16);
		Text(g, label, rect.X + 38.0f, textTop, rect.Width - 52.0f, 20, 11, FontStyleBold, textColor);
	}
	else {
		Text(g, label, rect.X + 6.0f, textTop, rect.Width - 12.0f, 20, 11, FontStyleBold, textColor,
			StringAlignmentCenter);
	}
}

void MainWindow::PaintPrimaryButton(Graphics& g, const RectF& rect, LPCWSTR label, int hoverId, float radius)
{
	PaintButton(g, rect, label, hoverId, ButtonStyle::Primary, radius);
}

void MainWindow::RequestSuperTopmost()
{
	const bool currentlyEnabled = setTopMost && IsWindowActuallyTopmost(_hWnd);
	if (currentlyEnabled) {
		if (!ApplyHighestPermittedTopmost(_hWnd, false)) {
			ShowFastTip(L"\u8d85\u7ea7\u7f6e\u9876\u5207\u6362\u5931\u8d25");
			return;
		}
		setTopMost = false;
		topMostUiAccessBand = false;
		SaveSettings();
		ShowFastTip(L"\u5df2\u53d6\u6d88\u7f6e\u9876");
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	// 普通置顶只能设 HWND_TOPMOST，挡不住「前台窗口强制上浮」这类竞争。
	// 要拿到不受影响的窗口带，进程必须带 TokenUIAccess，那就得重启自身一次。
	// 这段与 v1.0.2 一致：那一版就是靠它让"超级置顶"真正生效的。
	if (!IsCurrentProcessUiAccessEnabled()) {
		const UiAccessLaunchResult launch = RelaunchWithUiAccess();
		if (launch.success) {
			if (auto settings = JTAppGetSettingsDirect())
				settings->SetSettingBool(L"TopMost", true);
			isUserCancel = true;
			DestroyWindow(_hWnd);
			return;
		}
		const std::wstring detail = launch.step + L" 失败，错误码: " +
			std::to_wstring(static_cast<unsigned long>(launch.error));
		ShowFastTip(L"超级置顶启动失败");
		if (currentLogger) currentLogger->LogError(L"超级置顶启动失败: %s", detail.c_str());
		MessageBoxW(_hWnd, detail.c_str(), L"超级置顶启动失败",
			MB_OK | MB_ICONERROR | MB_TOPMOST);
		return;
	}
	bool bandApplied = false;
	if (!ApplyHighestPermittedTopmost(_hWnd, true, &bandApplied)) {
		ShowFastTip(L"\u7f6e\u9876\u5207\u6362\u5931\u8d25");
		return;
	}
	setTopMost = true;
	topMostUiAccessBand = bandApplied;
	SaveSettings();
	ShowFastTip(bandApplied ? L"\u5df2\u542f\u7528\u8d85\u7ea7\u7f6e\u9876" : L"\u5df2\u542f\u7528\u666e\u901a\u7f6e\u9876");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::PaintOverview(Graphics& g, int width, int height, int offsetY)
{
	std::lock_guard<std::mutex> guard(stateMutex);
	const float contentX = 248.0f;
	const float contentW = static_cast<float>(width - 276);
	const float top = 94.0f + offsetY;
	const Color primary = C(darkMode, 0, 107, 95, 130, 213, 199);
	const Color onPrimary = C(darkMode, 255, 255, 255, 0, 55, 49);
	SoftShadow(g, RectF(contentX, top, contentW, 170), 12, darkMode);
	FillRound(g, RectF(contentX, top, contentW, 170), 12, C(darkMode, 219, 239, 233, 28, 55, 50));
	FillRound(g, RectF(contentX + 24, top + 22, 96, 28), 14, C(darkMode, 185, 226, 217, 43, 76, 69));
	Text(g, L"\u9632\u62a4\u4e2d", contentX + 24, top + 29, 96, 18, 11, FontStyleBold, C(darkMode, 0, 81, 72, 155, 239, 224), StringAlignmentCenter);
	Text(g, statusTitle.c_str(), contentX + 24, top + 64, contentW - 230, 38, 23, FontStyleBold, C(darkMode, 25, 35, 32, 225, 236, 232));
	Text(g, statusDetail.c_str(), contentX + 24, top + 108, contentW - 230, 38, 11, FontStyleRegular, C(darkMode, 65, 79, 74, 178, 197, 190));

	const float badgeX = contentX + contentW - 152;
	const float badgeY = top + 29;
	FillRound(g, RectF(badgeX, badgeY, 120, 112), 20, primary);
	DrawIcon(g, 1, badgeX + 46, badgeY + 18, onPrimary, 28);
	Text(g, studentRunning ? L"\u5df2\u8fde\u63a5" : L"\u5f85\u68c0\u6d4b", badgeX + 8, badgeY + 56, 104, 26, 15, FontStyleBold, onPrimary, StringAlignmentCenter);
	Text(g, studentRunning ? L"StudentMain" : L"\u8fdb\u7a0b\u672a\u8fd0\u884c", badgeX + 8, badgeY + 84, 104, 18, 10, FontStyleRegular, onPrimary, StringAlignmentCenter);

	const float gap = 12.0f;
	const float cardW = (contentW - gap * 2) / 3.0f;
	const float metricsY = top + 186;
	for (int i = 0; i < 3; ++i) {
		const float x = contentX + i * (cardW + gap);
		SoftShadow(g, RectF(x, metricsY, cardW, 94), 12, darkMode);
		FillRound(g, RectF(x, metricsY, cardW, 94), 12, C(darkMode, 255, 255, 255, 28, 33, 31));
		StrokeRound(g, RectF(x, metricsY, cardW, 94), 12, C(darkMode, 222, 228, 224, 52, 60, 56));
	}
	Text(g, L"\u9632\u62a4\u72b6\u6001", contentX + 18, metricsY + 15, cardW - 32, 18, 11, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, currentControlled ? L"\u5b8c\u6574\u9632\u62a4" : L"\u6b63\u5728\u76d1\u63a7", contentX + 18, metricsY + 42, cardW - 32, 28, 16, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	wchar_t pidText[64];
	swprintf_s(pidText, L"PID  %lu", static_cast<unsigned long>(studentPid));
	Text(g, L"\u5b66\u751f\u7aef\u8fdb\u7a0b", contentX + cardW + gap + 18, metricsY + 15, cardW - 32, 18, 11, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, studentRunning ? pidText : L"\u672a\u8fd0\u884c", contentX + cardW + gap + 18, metricsY + 42, cardW - 32, 28, 16, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	Text(g, L"\u68c0\u6d4b\u8def\u5f84", contentX + (cardW + gap) * 2 + 18, metricsY + 15, cardW - 32, 18, 11, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, studentPath.c_str(), contentX + (cardW + gap) * 2 + 18, metricsY + 40, cardW - 34, 38, 11, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));

	const float actionsY = metricsY + 110;
	const float actionGap = 12.0f;
	const float actionW = (contentW - 40 - actionGap * 4) / 5.0f;
	const float actionH = 116.0f;
	// 卡片按内容收缩（标题 64 + 按钮 + 底部 24）。撑满窗口会让这一页下半截
	// 空出一大块 —— 空白的画布比空白的大卡片看起来自然得多。
	const float actionsH = 64.0f + actionH + 24.0f;
	SoftShadow(g, RectF(contentX, actionsY, contentW, actionsH), 12, darkMode);
	FillRound(g, RectF(contentX, actionsY, contentW, actionsH), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	StrokeRound(g, RectF(contentX, actionsY, contentW, actionsH), 12, C(darkMode, 236, 241, 238, 45, 53, 49));
	Text(g, L"\u5feb\u6377\u64cd\u4f5c", contentX + 20, actionsY + 18, 180, 24, 14, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	Text(g, L"\u5e38\u7528\u7684\u8fd0\u884c\u63a7\u5236\u548c\u8bbe\u5907\u5de5\u5177", contentX + 20, actionsY + 44, 340, 18, 10, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	const wchar_t* labels[] = { L"\u9632\u62a4\u72b6\u6001", setTopMost ? L"\u53d6\u6d88\u7f6e\u9876" : L"\u5f00\u542f\u7f6e\u9876", studentRunning ? L"\u7ed3\u675f\u6781\u57df" : L"\u91cd\u542f\u6781\u57df", L"\u63a7\u5236\u4e0e\u8bbe\u7f6e", L"\u7535\u6e90\u63a7\u5236" };
	const float actionY = actionsY + 64.0f;
	for (int i = 0; i < 5; ++i) {
		const float x = contentX + 20 + i * (actionW + actionGap);
		actionRects[i] = { static_cast<LONG>(x), static_cast<LONG>(actionY),
			static_cast<LONG>(x + actionW), static_cast<LONG>(actionY + actionH) };
		const float hover = hoverProgress[10 + i];
		const bool pressed = pressTarget == 10 + i;
		const float sink = pressed ? 1.0f : 0.0f;
		const Color base = i == 4 ? C(darkMode, 255, 246, 224, 58, 49, 28) : C(darkMode, 243, 248, 245, 39, 46, 43);
		const Color over = i == 4 ? C(darkMode, 255, 236, 194, 76, 63, 34) : C(darkMode, 224, 240, 233, 51, 66, 60);
		// 悬停时投影跟着加重一格，卡片才算"浮起来"；按下再压回去。
		SoftShadow(g, RectF(x, actionY + sink, actionW, actionH), 12, darkMode,
			pressed ? 0.3f : 0.35f + hover * 0.45f);
		FillRound(g, RectF(x, actionY + sink, actionW, actionH), 12, Blend(base, over, hover));
		const Color iconColor = i == 4 ? C(darkMode, 121, 86, 0, 241, 194, 98) : primary;
		// 图标在上、文字在下，两者水平居中 —— 原来图标靠左、文字跟在右边，
		// 按钮右侧会空掉一半。
		DrawIcon(g, i == 0 ? 1 : i == 1 ? 4 : i == 2 ? 5 : i == 3 ? 6 : 7,
			x + actionW / 2.0f - 14.0f, actionY + 24.0f + sink, iconColor, 28);
		Text(g, labels[i], x + 8, actionY + 68 + sink, actionW - 16, 26, 12, FontStyleBold,
			C(darkMode, 42, 48, 45, 220, 229, 225), StringAlignmentCenter);
	}
}

void MainWindow::PaintProtection(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const bool replacementPage = page == Page::Replacement;
	// 防护策略页这里只有标题 + 4 行开关（内容到 y+378），卡片按内容收缩；
	// 替换画面页下方还有整块预览面板，需要撑满可用高度。
	// 固定 68px 行距时卡片只占 384px，窗口剩下的 250px 全是空白画布，
	// 页面看着像没画完。改成按可用高度把四行开关均匀铺开。
	const float fillHeight = (std::max)(320.0f, static_cast<float>(height) - y - 20.0f);
	const float protectionCardH = replacementPage ? fillHeight : (std::min)(fillHeight, 560.0f);
	// 标题区 88 + 底部留白 24，剩下按 4 个槽位平分。
	const float toggleSlot = replacementPage ? 68.0f
		: (std::max)(68.0f, (protectionCardH - 112.0f) / 4.0f);
	// 行卡片在槽位里居中，槽位够大时最多长到 80，避免细长条看着像空行。
	const float toggleRowH = (std::max)(62.0f, (std::min)(80.0f, toggleSlot - 14.0f));
	SoftShadow(g, RectF(x, y, w, protectionCardH), 12, darkMode);
	FillRound(g, RectF(x, y, w, protectionCardH), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	if (!replacementPage) {
	Text(g, L"\u884c\u4e3a\u63a7\u5236", x + 24, y + 20, 200, 28, 16, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	Text(g, L"\u9009\u62e9\u5141\u8bb8\u7684\u8fdc\u7a0b\u884c\u4e3a\uff0c\u66f4\u6539\u5c06\u7acb\u5373\u5e94\u7528", x + 24, y + 50, 440, 20, 11, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	bool values[] = { !setAllowAllRunOp, setBandAllRunOp, setAllowGbTop, setProhibitKillProcess, setAllowMonitor, setProhibitCloseWindow, setAllowControl };
	const float cardW = (w - 62) / 2.0f;
	for (int i = 0; i < 7; ++i) {
		const int col = i % 2;
		const int row = i / 2;
		const float cardX = x + 24 + col * (cardW + 14);
		// 最后一行只有一个开关，仍按槽位对齐，不要让它自己居中跑偏。
		const float cardY = y + 88 + row * toggleSlot + (toggleSlot - toggleRowH) / 2.0f;
		toggleRects[i] = { static_cast<LONG>(cardX), static_cast<LONG>(cardY),
			static_cast<LONG>(cardX + cardW), static_cast<LONG>(cardY + toggleRowH) };
		FillRound(g, RectF(cardX, cardY, cardW, toggleRowH), 12 ,
			Blend(C(darkMode, 248, 251, 249, 31, 37, 35), C(darkMode, 223, 238, 232, 48, 60, 56), hoverProgress[20 + i]));
		const float toggleTextY = cardY + toggleRowH / 2.0f - 12.0f;
		Text(g, kToggleLabels[i], cardX + 18, toggleTextY, cardW - 100, 25, 12, FontStyleRegular,
			C(darkMode, 40, 46, 43, 220, 229, 225));
		const float switchY = cardY + toggleRowH / 2.0f - 13.0f;
		// 滑块按插值走，不是瞬间跳；轨道颜色跟着一起过渡。
		const float motion = AnimateToggle(i, values[i]);
		FillRound(g, RectF(cardX + cardW - 62, switchY, 44, 26), 13,
			Blend(C(darkMode, 116, 124, 120, 97, 108, 103), C(darkMode, 0, 107, 95, 130, 213, 199), motion));
		SolidBrush knob(Blend(C(darkMode, 245, 247, 245, 219, 225, 222), C(darkMode, 255, 255, 255, 0, 55, 49), motion));
		g.FillEllipse(&knob, RectF(cardX + cardW - (58.0f - motion * 18.0f), switchY + 4.0f, 18.0f, 18.0f));
	}
	toggleRects[7] = {};
	driverLoadRect = {};
	driverUnloadRect = {};
		saveSettingsRect = {};
	}
	else {
		for (RECT& rect : toggleRects) rect = {};
		driverLoadRect = {};
		saveSettingsRect = {};
	}
	if (!replacementPage) {
		const float buttonX = x + w - 150;
		const float buttonY = y + 20;
		driverUnloadRect = { static_cast<LONG>(buttonX), static_cast<LONG>(buttonY), static_cast<LONG>(buttonX + 126), static_cast<LONG>(buttonY + 36) };
		PaintButton(g, RectF(buttonX, buttonY, 126, 36), L"\u5378\u8f7d\u4e3b\u9a71\u52a8", 44,
			ButtonStyle::Danger, 18);
		temporaryVideoEnableRect = {};
		temporaryVideoImageModeRect = {};
		temporaryVideoFileModeRect = {};
		temporaryVideoImagePickerRect = {};
		temporaryVideoFilePickerRect = {};
		temporaryVideoLoopRect = {};
		temporaryVideoPreviewRect = {};
		restoreTeacherViewRect = {};
		return;
	}

	const float panelY = replacementPage ? y + 18 : y + 404;
	const float panelH = (std::max)(154.0f, static_cast<float>(height) - panelY - 34.0f);
	// ────────────────────────────────────────────────────────────────────
	// 面板排版：顶部一行放标题 + 高级设置，下面分左右两列。
	// 预览图从 panelY+72 开始，不再压住右上角的高级设置按钮。
	// 这段代码同时服务于「替换画面」页（panelY=y+18，面板高）和「防护策略」页
	// 下方的同名面板（panelY=y+404，只有 228px 高），所以纵向必须控制在 226px 内。
	// ────────────────────────────────────────────────────────────────────
	FillRound(g, RectF(x + 24, panelY, w - 48, panelH), 12, C(darkMode, 248, 251, 249, 31, 37, 35));

	Text(g, page == Page::Replacement ? L"\u66ff\u6362\u753b\u9762" : L"\u4fe1\u606f\u6d41\u4fdd\u62a4", x + 42, panelY + 12, 200, 24, 13, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	Text(g, L"开启后教师端看到你选的图片/视频；未选媒体时为纯黑。", x + 42, panelY + 46, 420, 17, 10, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	// Keep the main-driver action visible on the same protection surface.
	driverUnloadRect = {};

	advancedSettingsRect = { static_cast<LONG>(x + w - 250), static_cast<LONG>(panelY + 12), static_cast<LONG>(x + w - 144), static_cast<LONG>(panelY + 44) };
	PaintButton(g, RectF(x + w - 250, panelY + 12, 106, 32), L"高级设置", 42, ButtonStyle::Tonal, 16);

	const bool imageMode = temporaryVideoMode == L"image" || temporaryVideoMode == L"photo";
	const float controlX = x + 42;

	// 左列 · 信息流保护开关（原先在右上角，被预览图整块盖住，用户看不到）
	Text(g, L"信息流保护", controlX, panelY + 78, 80, 18, 11, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	temporaryVideoEnableRect = { static_cast<LONG>(controlX + 86), static_cast<LONG>(panelY + 72), static_cast<LONG>(controlX + 178), static_cast<LONG>(panelY + 100) };
	FillRound(g, RectF(controlX + 86, panelY + 72, 92, 28), 14,
		temporaryVideoProtection ? C(darkMode, 0, 107, 95, 130, 213, 199) : C(darkMode, 116, 124, 120, 97, 108, 103));
	Text(g, temporaryVideoProtection ? L"已启用" : L"已关闭", controlX + 86, panelY + 79, 92, 18, 11, FontStyleBold,
		temporaryVideoProtection ? C(darkMode, 255, 255, 255, 0, 55, 49) : C(darkMode, 245, 247, 245, 219, 225, 222), StringAlignmentCenter);

	// 左列 · 模式
	Text(g, L"模式", controlX, panelY + 112, 40, 18, 11, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	temporaryVideoImageModeRect = { static_cast<LONG>(controlX + 46), static_cast<LONG>(panelY + 106), static_cast<LONG>(controlX + 110), static_cast<LONG>(panelY + 134) };
	temporaryVideoFileModeRect = { static_cast<LONG>(controlX + 116), static_cast<LONG>(panelY + 106), static_cast<LONG>(controlX + 180), static_cast<LONG>(panelY + 134) };
	auto drawMode = [&](RECT rect, LPCWSTR label, bool selected, int hoverId) {
		const float hover = selected ? 0.0f : hoverProgress[hoverId];
		const float sink = pressTarget == hoverId ? 1.0f : 0.0f;
		FillRound(g, RectF(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top) + sink,
			static_cast<REAL>(rect.right - rect.left), static_cast<REAL>(rect.bottom - rect.top)), 14,
			selected ? C(darkMode, 183, 232, 222, 42, 79, 72)
				: Blend(C(darkMode, 225, 232, 228, 39, 47, 44), C(darkMode, 208, 230, 221, 54, 74, 67), hover));
		Text(g, label, static_cast<float>(rect.left), static_cast<float>(rect.top + 7) + sink, static_cast<float>(rect.right - rect.left), 18, 10 ,
			selected ? FontStyleBold : FontStyleRegular, selected ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);
	};
	drawMode(temporaryVideoImageModeRect, L"图片", imageMode, 75);
	drawMode(temporaryVideoFileModeRect, L"视频", !imageMode, 76);

	// 左列 · 选择媒体 + 循环
	temporaryVideoImagePickerRect = { static_cast<LONG>(controlX), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 100), static_cast<LONG>(panelY + 170) };
	temporaryVideoFilePickerRect = { static_cast<LONG>(controlX + 106), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 206), static_cast<LONG>(panelY + 170) };
	auto drawPicker = [&](RECT rect, LPCWSTR label, bool selected, int hoverId) {
		const float hover = hoverProgress[hoverId];
		const float sink = pressTarget == hoverId ? 1.0f : 0.0f;
		FillRound(g, RectF(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top) + sink,
			static_cast<REAL>(rect.right - rect.left), static_cast<REAL>(rect.bottom - rect.top)), 10,
			Blend(C(darkMode, 219, 238, 232, 43, 76, 69), C(darkMode, 196, 226, 217, 56, 100, 90),
				selected ? 1.0f : hover));
		Text(g, label, static_cast<float>(rect.left), static_cast<float>(rect.top + 7) + sink, static_cast<float>(rect.right - rect.left), 18, 10, FontStyleBold,
			C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	};
	drawPicker(temporaryVideoImagePickerRect, L"选择图片", imageMode, 77);
	drawPicker(temporaryVideoFilePickerRect, L"选择视频", !imageMode, 78);

	temporaryVideoLoopRect = { static_cast<LONG>(controlX + 218), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 288), static_cast<LONG>(panelY + 170) };
	{
		const float sink = pressTarget == 79 ? 1.0f : 0.0f;
		FillRound(g, RectF(controlX + 218, panelY + 140 + sink, 70, 30), 10,
			temporaryVideoLoop ? C(darkMode, 183, 232, 222, 42, 79, 72)
				: Blend(C(darkMode, 225, 232, 228, 39, 47, 44), C(darkMode, 208, 230, 221, 54, 74, 67), hoverProgress[79]));
		Text(g, temporaryVideoLoop ? L"循环：开" : L"循环：关", controlX + 218, panelY + 147 + sink, 70, 18, 10, FontStyleBold,
			temporaryVideoLoop ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);
	}

	// 左列 · 状态 / 警告
	const std::wstring& selectedPath = imageMode ? temporaryVideoImagePath : temporaryVideoFilePath;
	// 没选媒体时开启保护会回退成纯黑。明确写出来，别让用户以为是坏了。
	const bool noMedia = selectedPath.empty();
	const wchar_t* pathText = noMedia
		? (imageMode ? L"尚未选择图片 —— 此时开启保护，教师端会显示纯黑" : L"尚未选择视频 —— 此时开启保护，教师端会显示纯黑")
		: selectedPath.c_str();
	Text(g, pathText, controlX, panelY + 176, 340, 17, 10, FontStyleRegular,
		(noMedia && temporaryVideoProtection) ? C(darkMode, 198, 96, 40, 236, 146, 90) : C(darkMode, 91, 99, 95, 166, 177, 171));

	// 左列 · 一键恢复（遮挡有两个来源，只关一个教师端还是黑的）
	restoreTeacherViewRect = { static_cast<LONG>(controlX), static_cast<LONG>(panelY + 196), static_cast<LONG>(controlX + 152), static_cast<LONG>(panelY + 226) };
	PaintButton(g, RectF(controlX, panelY + 196, 152, 30), L"恢复教师画面", 81, ButtonStyle::Tonal, 10);

	// 右列 · 预览图（从 panelY+72 起，标题行与说明行都留给上方）
	// 原来最大只有 300px，在 860px 宽的卡片里像个小缩略图，右侧留白很别扭。
	const float previewW = (std::min)(384.0f, (std::max)(240.0f, w * 0.44f));
	const float previewH = (std::min)(360.0f, (std::max)(140.0f, panelH - 110.0f));
	temporaryVideoPreviewRect = { static_cast<LONG>(x + w - previewW - 42), static_cast<LONG>(panelY + 72), static_cast<LONG>(x + w - 42), static_cast<LONG>(panelY + 72 + previewH) };
	// 预览图不是按钮，但光标移上去时给一圈描边，说明"这里可以点开选片"。
	{
		const float previewHover = hoverProgress[85];
		FillRound(g, RectF(static_cast<REAL>(temporaryVideoPreviewRect.left), static_cast<REAL>(temporaryVideoPreviewRect.top), previewW, previewH), 12, C(darkMode, 29, 38, 34, 232, 240, 236));
		if (previewHover > 0.002f) {
			StrokeRound(g, RectF(static_cast<REAL>(temporaryVideoPreviewRect.left), static_cast<REAL>(temporaryVideoPreviewRect.top), previewW, previewH), 12,
				Blend(C(darkMode, 232, 238, 234, 40, 47, 44), C(darkMode, 0, 152, 132, 96, 200, 184), previewHover), 1.5f);
		}
	}
	if (temporaryVideoPreview && temporaryVideoPreview->GetLastStatus() == Ok) {
		const float imageW = static_cast<float>(temporaryVideoPreview->GetWidth());
		const float imageH = static_cast<float>(temporaryVideoPreview->GetHeight());
		const float scale = (std::min)((previewW - 12.0f) / imageW, (previewH - 12.0f) / imageH);
		const float drawW = imageW * scale;
		const float drawH = imageH * scale;
		g.DrawImage(temporaryVideoPreview.get(), x + w - previewW - 42 + (previewW - drawW) / 2.0f, panelY + 72 + (previewH - drawH) / 2.0f, drawW, drawH);
	}
	else {
		DrawIcon(g, imageMode ? 3 : 5, x + w - previewW / 2.0f - 52, panelY + 72 + previewH / 2.0f - 3, C(darkMode, 135, 156, 148, 116, 140, 130), 24);
		Text(g, imageMode ? L"图片预览" : L"视频文件", x + w - previewW - 42, panelY + 72 + previewH / 2.0f + 28, previewW, 18, 10, FontStyleRegular,
			C(darkMode, 135, 156, 148, 116, 140, 130), StringAlignmentCenter);
	}
}

void MainWindow::PaintAntivirus(Graphics& g, int width, int height, int offsetY)
{
	std::lock_guard<std::mutex> guard(stateMutex);
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20.0f;
	const bool running = InterlockedCompareExchange(&avScanRunning, 0, 0) != 0;
	const bool driverReady = AvIntegratedIsLoaded() != FALSE;
	const Color primary = C(darkMode, 0, 107, 95, 130, 213, 199);
	const Color titleColor = C(darkMode, 24, 30, 27, 226, 233, 229);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	const Color surface = C(darkMode, 248, 251, 249, 31, 37, 35);

	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	Text(g, L"\u8fdb\u7a0b\u5185\u5b58\u7279\u5f81\u626b\u63cf", x + 24, y + 18, 320, 28, 16, FontStyleBold, titleColor);
	Text(g, L"\u7531 JiYuAvKernel \u626b\u63cf\u6240\u6709\u8fdb\u7a0b\u7684\u53ef\u6267\u884c\u5185\u5b58\u533a\u57df\uff0c\u652f\u6301\u8de8\u7248\u672c\u901a\u914d\u7279\u5f81", x + 24, y + 48, w - 48, 20, 11, FontStyleRegular, secondary);

	FillRound(g, RectF(x + 24, y + 78, w - 48, 86), 12, surface);
	FillRound(g, RectF(x + 42, y + 98, 12, 12), 6,
		driverReady ? primary : C(darkMode, 180, 72, 48, 232, 154, 123));
	Text(g, driverReady ? L"JiYuAvKernel \u5df2\u8fde\u63a5" : L"JiYuAvKernel \u672a\u8fde\u63a5",
		x + 66, y + 91, 240, 24, 12, FontStyleBold, titleColor);
	Text(g, avScanStatus.c_str(), x + 42, y + 124, w - 84, 24, 11, FontStyleRegular, secondary);

	auto drawButton = [&](RECT& target, float buttonX, float buttonW, LPCWSTR label, int hoverId, bool primaryButton) {
		target = { static_cast<LONG>(buttonX), static_cast<LONG>(y + 178),
			static_cast<LONG>(buttonX + buttonW), static_cast<LONG>(y + 220) };
		PaintButton(g, RectF(buttonX, y + 178, buttonW, 42), label, hoverId,
			primaryButton ? ButtonStyle::Primary : ButtonStyle::Tonal, 12);
	};
	// 六个按钮等宽，原来按内容比例给宽度，一排看下来参差不齐。
	const float buttonGap = 10.0f;
	const float buttonW = (w - 48.0f - buttonGap * 5.0f) / 6.0f;
	drawButton(avScanRect, x + 24, buttonW, running ? L"\u626b\u63cf\u4e2d..." : L"\u626b\u63cf\u5168\u90e8\u8fdb\u7a0b", 45, true);
	drawButton(avImportRect, avScanRect.right + buttonGap, buttonW, L"\u8fd0\u884c\u8fdb\u7a0b\u5f55\u5165", 46, false);
	drawButton(avInputRect, avImportRect.right + buttonGap, buttonW, L"\u8f93\u5165\u7279\u5f81\u7801", 50, false);
	drawButton(avCopyRect, avInputRect.right + buttonGap, buttonW, L"\u590d\u5236", 47, false);
	drawButton(avUnloadRect, avCopyRect.right + buttonGap, buttonW, L"\u5378\u8f7d\u626b\u63cf\u9a71\u52a8", 49, false);
	drawButton(avCancelRect, avUnloadRect.right + buttonGap, buttonW, L"\u505c\u6b62", 48, false);

	const float metricsY = y + 238;
	const float metricGap = 10.0f;
	const float metricW = (w - 48.0f - metricGap * 3.0f) / 4.0f;
	const wchar_t* metricLabels[] = { L"\u8fdb\u7a0b", L"\u5df2\u626b\u63cf", L"\u547d\u4e2d", L"\u8df3\u8fc7/\u5931\u8d25" };
	const DWORD metricValues[] = { avScanSummary.processCount, avScanSummary.scannedCount, avScanSummary.detectionCount, avScanSummary.failedCount };
	for (int index = 0; index < 4; ++index) {
		const float cardX = x + 24 + index * (metricW + metricGap);
		FillRound(g, RectF(cardX, metricsY, metricW, 74), 12, surface);
		Text(g, metricLabels[index], cardX + 14, metricsY + 11, metricW - 28, 18, 10, FontStyleRegular, secondary);
		WCHAR value[32] = {};
		swprintf_s(value, L"%lu", static_cast<unsigned long>(metricValues[index]));
		Text(g, value, cardX + 14, metricsY + 34, metricW - 28, 27, 17, FontStyleBold,
			index == 2 && metricValues[index] != 0 ? C(darkMode, 176, 62, 43, 246, 164, 139) : titleColor);
	}

	const float signatureY = metricsY + 88;
	FillRound(g, RectF(x + 24, signatureY, w - 48, 84), 12, surface);
	WCHAR signatureCaption[128] = {};
	swprintf_s(signatureCaption, L"\u6700\u8fd1\u5f55\u5165\u7684\u5185\u5b58\u7279\u5f81  |  %lu \u4e2a\u7248\u672c  |  %lu \u4e2a\u7cbe\u786e\u5b57\u8282",
		static_cast<unsigned long>(avSignatureSampleCount), static_cast<unsigned long>(avSignatureExactBytes));
	Text(g, signatureCaption, x + 40, signatureY + 11, w - 80, 18, 10, FontStyleRegular, secondary);
	Text(g, avSignaturePath.empty() ? L"\u5c1a\u672a\u5f55\u5165\u8fd0\u884c\u65f6\u7279\u5f81" : avSignaturePath.c_str(),
		x + 40, signatureY + 32, w - 80, 18, 10, FontStyleBold, titleColor);
	Text(g, avSignaturePattern.empty() ? L"-" : avSignaturePattern.c_str(),
		x + 40, signatureY + 54, w - 80, 18, 10, FontStyleRegular, primary);

	const float resultY = signatureY + 98;
	const float resultH = std::max(58.0f, y + h - resultY - 16.0f);
	FillRound(g, RectF(x + 24, resultY, w - 48, resultH), 12, surface);
	Text(g, L"\u626b\u63cf\u547d\u4e2d", x + 40, resultY + 12, 160, 20, 11, FontStyleBold, titleColor);
	if (avScanSummary.findingCount == 0) {
		Text(g, running ? L"\u6b63\u5728\u68c0\u67e5\u8fdb\u7a0b\u6620\u50cf..." : L"\u6682\u65e0\u547d\u4e2d\u8bb0\u5f55",
			x + 40, resultY + 42, w - 80, 20, 11, FontStyleRegular, secondary);
	}
	else {
		const DWORD visibleCount = std::min<DWORD>(avScanSummary.findingCount, AV_INTEGRATED_MAX_FINDINGS);
		for (DWORD index = 0; index < visibleCount; ++index) {
			Text(g, avScanSummary.findings[index], x + 40, resultY + 40 + index * 22.0f,
				w - 80, 18, 10, FontStyleRegular, C(darkMode, 150, 52, 37, 244, 158, 132));
		}
	}
}

// 高级配置页的纵向节奏。绘制和原生控件的 MoveWindow 都从这里取，
// 否则两边各写一套坐标，改一处就错位。
// 底部那段（说明条 + 保存按钮）从卡片底边往上排，快捷键卡挂在说明条上方，
// 窗口拉高时多出来的空白落在开关区和快捷键区之间，不会把按钮顶到半空。

// 把 HOTKEY 控件的编码翻成可读文字：HIBYTE 是修饰键位（1 Shift / 2 Ctrl / 4 Alt / 8 Ext），
// LOBYTE 是虚拟键码。
std::wstring HotkeyDisplayText(int value)
{
	if (value == 0) return L"\u672a\u8bbe\u7f6e";
	const int mods = (value >> 8) & 0xFF;
	const int key = value & 0xFF;
	std::wstring text;
	if (mods & 0x02) text += L"Ctrl + ";
	if (mods & 0x04) text += L"Alt + ";
	if (mods & 0x01) text += L"Shift + ";
	if (key >= 'A' && key <= 'Z') text += static_cast<wchar_t>(key);
	else if (key >= '0' && key <= '9') text += static_cast<wchar_t>(key);
	else if (key >= VK_F1 && key <= VK_F24) {
		text += L"F";
		text += std::to_wstring(key - VK_F1 + 1);
	}
	else {
		switch (key) {
		case VK_TAB: text += L"Tab"; break;
		case VK_SPACE: text += L"Space"; break;
		case VK_RETURN: text += L"Enter"; break;
		case VK_BACK: text += L"Backspace"; break;
		case VK_LEFT: text += L"\u2190"; break;
		case VK_RIGHT: text += L"\u2192"; break;
		case VK_UP: text += L"\u2191"; break;
		case VK_DOWN: text += L"\u2193"; break;
		case VK_HOME: text += L"Home"; break;
		case VK_END: text += L"End"; break;
		case VK_INSERT: text += L"Insert"; break;
		case VK_DELETE: text += L"Delete"; break;
		case VK_PRIOR: text += L"PageUp"; break;
		case VK_NEXT: text += L"PageDown"; break;
		default: {
			wchar_t buffer[16] = {};
			swprintf_s(buffer, L"VK %02X", static_cast<unsigned>(key));
			text += buffer;
			break;
		}
		}
	}
	return text;
}

float AdvancedSaveButtonY(float cardHeight) { return cardHeight - 54.0f; }

float AdvancedHintBannerY(float cardHeight) { return AdvancedSaveButtonY(cardHeight) - 90.0f; }

float AdvancedHotkeyCardY(float cardHeight) { return AdvancedHintBannerY(cardHeight) - 114.0f; }

float AdvancedToggleSlot(float cardHeight)
{
	// 三行开关平分 78 到快捷键卡之间的区域。
	return (std::max)(72.0f, (std::min)(104.0f, (AdvancedHotkeyCardY(cardHeight) - 94.0f) / 3.0f));
}

void MainWindow::PaintAdvanced(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	const Color surface = C(darkMode, 248, 251, 249, 31, 37, 35);
	const Color titleColor = C(darkMode, 42, 49, 46, 220, 229, 225);
	const Color secondary = C(darkMode, 98, 106, 102, 159, 171, 165);
	const Color fieldFill = C(darkMode, 255, 255, 255, 26, 33, 30);
	const Color hairline = C(darkMode, 226, 234, 229, 44, 52, 48);
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	const wchar_t* tabs[] = { L"\u5e38\u89c4\u4e0e\u5feb\u6377\u952e", L"\u8c03\u8bd5\u4e0e\u517c\u5bb9\u6027" };
	for (int i = 0; i < 2; ++i) {
		const float tabX = x + 24 + i * 166.0f;
		advancedTabRects[i] = { static_cast<LONG>(tabX), static_cast<LONG>(y + 18), static_cast<LONG>(tabX + 156), static_cast<LONG>(y + 58) };
		const Color idle = Blend(C(darkMode, 235, 241, 238, 36, 43, 40), C(darkMode, 222, 231, 226, 45, 53, 49), hoverProgress[70 + i]);
		FillRound(g, RectF(tabX, y + 18, 156, 40), 20,
			advancedTab == i ? C(darkMode, 183, 232, 222, 42, 79, 72) : idle);
		Text(g, tabs[i], tabX, y + 29, 156, 20, 11, advancedTab == i ? FontStyleBold : FontStyleRegular,
			advancedTab == i ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);
	}
	auto drawToggle = [&](int index, float cardX, float cardY, float cardW, LPCWSTR title, LPCWSTR detail, bool enabled) {
		advancedToggleRects[index] = { static_cast<LONG>(cardX), static_cast<LONG>(cardY), static_cast<LONG>(cardX + cardW), static_cast<LONG>(cardY + 64) };
		FillRound(g, RectF(cardX, cardY, cardW, 64), 12 ,
			Blend(C(darkMode, 248, 251, 249, 31, 37, 35), C(darkMode, 220, 237, 231, 49, 63, 58), hoverProgress[50 + index]));
		Text(g, title, cardX + 16, cardY + 12, cardW - 92, 20, 11, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
		Text(g, detail, cardX + 16, cardY + 36, cardW - 92, 17, 10, FontStyleRegular, C(darkMode, 98, 106, 102, 159, 171, 165));
		const float knobMotion = AnimateToggle(8 + index, enabled);
		FillRound(g, RectF(cardX + cardW - 60, cardY + 19, 44, 26), 13,
			Blend(C(darkMode, 116, 124, 120, 97, 108, 103), C(darkMode, 0, 107, 95, 130, 213, 199), knobMotion));
		SolidBrush knob(Blend(C(darkMode, 245, 247, 245, 219, 225, 222), C(darkMode, 255, 255, 255, 0, 55, 49), knobMotion));
		g.FillEllipse(&knob, RectF(cardX + cardW - (56.0f - knobMotion * 18.0f), cardY + 23, 18, 18));
	};
	// 原生控件自己带方形边框，所以这里先画一块圆角输入框，把控件放在框内 4px，
	// 控件背景色和框内填充色一致（见 WM_CTLCOLOR 里的分支），边缘就看不出来了。
	auto drawField = [&](const RectF& field, HWND control) {
		const bool focused = control && GetFocus() == control;
		FillRound(g, field, 10, fieldFill);
		StrokeRound(g, field, 10, focused ? C(darkMode, 0, 107, 95, 130, 213, 199) : hairline);
	};
	const float gap = 12.0f;
	const float cardW = (w - 60) / 2.0f;
	if (advancedTab == 0) {
		const wchar_t* titles[] = { L"\u7981\u7528\u5185\u6838\u9a71\u52a8", L"\u9a71\u52a8\u5c42\u81ea\u6211\u4fdd\u62a4", L"\u81ea\u52a8\u5f3a\u5236\u6e05\u7406", L"\u4e25\u683c\u7a97\u53e3\u63a7\u5236", L"\u9690\u85cf\u63a7\u5236\u8f93\u51fa", L"\u9690\u85cf\u4efb\u52a1\u680f\u56fe\u6807" };
		const wchar_t* details[] = { L"32 \u4f4d\u7cfb\u7edf\u9009\u9879", L"\u4ec5 32 \u4f4d\u7cfb\u7edf\u6709\u6548", L"\u64cd\u4f5c\u5931\u8d25\u65f6\u7ed3\u675f\u76f8\u5173\u8fdb\u7a0b", L"\u63a7\u5236\u6240\u6709\u6781\u57df\u7a97\u53e3", L"\u9690\u85cf\u5de6\u4e0a\u89d2\u8f93\u51fa\u7a97\u53e3", L"\u4ecd\u53ef\u4f7f\u7528\u5feb\u6377\u952e\u5524\u8d77" };
		bool values[] = { advDisableDriver, advSelfProtect, advAutoForceKill, advStrictWindow, advHideOutput, advHideTaskbar };
		const float slot = AdvancedToggleSlot(h);
		for (int i = 0; i < 6; ++i) {
			const int col = i % 2, row = i / 2;
			// 行卡片在槽位里垂直居中，槽位变大时不会贴着上一行。
			drawToggle(i, x + 24 + col * (cardW + gap),
				y + 78 + row * slot + (slot - 64.0f) / 2.0f, cardW, titles[i], details[i], values[i]);
		}
		const float hotkeyY = y + AdvancedHotkeyCardY(h);
		FillRound(g, RectF(x + 24, hotkeyY - 15.0f, w - 48, 1), 0.5f, hairline);
		const wchar_t* hotkeyTitles[] = { L"\u7d27\u6025\u5168\u5c4f\u5feb\u6377\u952e", L"\u663e\u793a / \u9690\u85cf\u4e3b\u7a97\u53e3" };
		const wchar_t* hotkeyHints[] = { L"\u628a\u6559\u5e08\u7aef\u753b\u9762\u5207\u5230\u66ff\u6362\u753b\u9762", L"\u4ece\u4efb\u610f\u754c\u9762\u5524\u56de\u8fd9\u4e2a\u7a97\u53e3" };
		for (int i = 0; i < 2; ++i) {
			const float cardX = x + 24 + i * (cardW + gap);
			FillRound(g, RectF(cardX, hotkeyY, cardW, 96), 12, surface);
			StrokeRound(g, RectF(cardX, hotkeyY, cardW, 96), 12, hairline);
			Text(g, hotkeyTitles[i], cardX + 16, hotkeyY + 12, cardW - 32, 20, 11, FontStyleBold, titleColor);
			Text(g, hotkeyHints[i], cardX + 16, hotkeyY + 33, cardW - 32, 17, 10, FontStyleRegular, secondary);
			const RectF field(cardX + 16, hotkeyY + 57, cardW - 32, 30);
			advancedHotkeyRect[i] = { static_cast<LONG>(field.X), static_cast<LONG>(field.Y),
				static_cast<LONG>(field.GetRight()), static_cast<LONG>(field.GetBottom()) };
			const bool capturing = hotkeyCaptureIndex == i;
			FillRound(g, field, 10, capturing ? C(darkMode, 232, 247, 242, 30, 56, 49) : fieldFill);
			StrokeRound(g, field, 10, capturing ? C(darkMode, 0, 152, 132, 106, 220, 203)
				: C(darkMode, 216, 226, 220, 46, 55, 51));
			const int value = i == 0 ? hotkeyFakeFullValue : hotkeyShowHideValue;
			const std::wstring label = capturing ? L"\u8bf7\u6309\u4e0b\u7ec4\u5408\u952e" : HotkeyDisplayText(value);
			Text(g, label.c_str(), field.X + 12, field.Y + 6, field.Width - 24, 20, 11,
				capturing ? FontStyleRegular : FontStyleBold,
				capturing ? C(darkMode, 0, 107, 95, 130, 213, 199) : C(darkMode, 38, 46, 43, 226, 234, 230));
		}
	}
	else {
		Text(g, L"\u7ed3\u675f\u8fdb\u7a0b\u65b9\u5f0f", x + 24, y + 78, 180, 22, 11, FontStyleBold, titleColor);
		const wchar_t* killLabels[] = { L"TerminateProcess", L"NtTerminateProcess", L"Kernel Mode" };
		for (int i = 0; i < 3; ++i) {
			const float bx = x + 24 + i * 154.0f;
			const bool pressed = pressTarget == 64 + i;
			const float sink = pressed ? 1.0f : 0.0f;
			advancedKillRects[i] = { static_cast<LONG>(bx), static_cast<LONG>(y + 108), static_cast<LONG>(bx + 144), static_cast<LONG>(y + 146) };
			FillRound(g, RectF(bx, y + 108 + sink, 144, 38), 19,
				advancedKillMode == i ? C(darkMode, 183, 232, 222, 42, 79, 72)
					: Blend(surface, C(darkMode, 226, 238, 232, 46, 58, 53), hoverProgress[64 + i]));
			Text(g, killLabels[i], bx, y + 118 + sink, 144, 18, 10, advancedKillMode == i ? FontStyleBold : FontStyleRegular,
				C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
		}
		Text(g, L"\u6ce8\u5165\u65b9\u5f0f", x + 24, y + 166, 180, 22, 11, FontStyleBold, titleColor);
		const wchar_t* injectLabels[] = { L"RemoteThread", L"HookDllStub" };
		for (int i = 0; i < 2; ++i) {
			const float bx = x + 24 + i * 154.0f;
			const bool pressed = pressTarget == 67 + i;
			const float sink = pressed ? 1.0f : 0.0f;
			advancedInjectRects[i] = { static_cast<LONG>(bx), static_cast<LONG>(y + 196), static_cast<LONG>(bx + 144), static_cast<LONG>(y + 234) };
			FillRound(g, RectF(bx, y + 196 + sink, 144, 38), 19,
				advancedInjectMode == i ? C(darkMode, 183, 232, 222, 42, 79, 72)
					: Blend(surface, C(darkMode, 226, 238, 232, 46, 58, 53), hoverProgress[67 + i]));
			Text(g, injectLabels[i], bx, y + 206 + sink, 144, 18, 10, advancedInjectMode == i ? FontStyleBold : FontStyleRegular,
				C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
		}
		const float intervalX = x + w - 214;
		Text(g, L"\u68c0\u6d4b\u95f4\u9694 (ms)", intervalX, y + 166, 190, 20, 11, FontStyleBold, titleColor);
		drawField(RectF(intervalX, y + 188, 190, 38), advancedControls[2]);
		const wchar_t* titles[] = { L"\u542f\u7528\u63a7\u5236\u5668", L"\u59cb\u7ec8\u68c0\u67e5\u66f4\u65b0", L"\u5f3a\u5236\u5b89\u88c5\u5230\u5f53\u524d\u76ee\u5f55", L"\u7981\u7528 WatchDog", L"\u6ce8\u5165 Master Helper", L"\u6ce8\u5165 64 \u4f4d Helper" };
		bool values[] = { advController, advAlwaysUpdate, advForceCurrentDir, advDisableWatchdog, advInjectMaster, advInject64 };
		// 六个开关铺到说明条上方，别都挤在页面上半截。
		const float compatSlot = (std::max)(68.0f, (std::min)(104.0f, (AdvancedHintBannerY(h) - 278.0f) / 3.0f));
		for (int i = 0; i < 6; ++i) {
			const int col = i % 2, row = i / 2;
			drawToggle(i, x + 24 + col * (cardW + gap),
				y + 260 + row * compatSlot + (compatSlot - 64.0f) / 2.0f,
				cardW, titles[i], L"\u9ad8\u7ea7\u9009\u9879\uff0c\u4fee\u6539\u540e\u8bf7\u4fdd\u5b58", values[i]);
		}
	}
	const float bannerY = y + AdvancedHintBannerY(h);
	FillRound(g, RectF(x + 24, bannerY, w - 48, 66), 12, surface);
	DrawIcon(g, 3, x + 44, bannerY + 24, C(darkMode, 0, 107, 95, 130, 213, 199), 18);
	Text(g, advancedTab == 0
		? L"\u5feb\u6377\u952e\u5728\u4efb\u610f\u754c\u9762\u90fd\u751f\u6548\uff1b\u70b9\u8f93\u5165\u6846\u540e\u76f4\u63a5\u6309\u4e0b\u7ec4\u5408\u952e\u5373\u53ef\u5f55\u5236\uff0cEsc \u6e05\u9664\u3002"
		: L"\u7ed3\u675f\u65b9\u5f0f\u4e0e\u6ce8\u5165\u65b9\u5f0f\u5f71\u54cd\u9a71\u52a8\u5c42\u884c\u4e3a\uff0c\u4fee\u6539\u540e\u4fdd\u5b58\u5e76\u91cd\u542f\u4e3b\u7a0b\u5e8f\u624d\u4f1a\u751f\u6548\u3002",
		x + 76, bannerY + 22, w - 128, 22, 10, FontStyleRegular, secondary);
	const float saveY = y + AdvancedSaveButtonY(h);
	advancedSaveRect = { static_cast<LONG>(x + w - 148), static_cast<LONG>(saveY), static_cast<LONG>(x + w - 24), static_cast<LONG>(saveY + 40) };
	PaintPrimaryButton(g, RectF(x + w - 148, saveY, 124, 40), L"\u4fdd\u5b58\u9ad8\u7ea7\u8bbe\u7f6e", 69);
}

void MainWindow::PaintHelp(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	Text(g, L"\u5feb\u901f\u4e0a\u624b", x + 28, y + 24, 220, 28, 17, FontStyleBold, C(darkMode, 24, 30, 27, 226, 233, 229));
	Text(g, L"Dzjs Trainer \u4f1a\u81ea\u52a8\u68c0\u6d4b StudentMain \u5e76\u5e94\u7528\u5f53\u524d\u9632\u62a4\u7b56\u7565\u3002", x + 28, y + 58, w - 56, 28, 11,
		FontStyleRegular, C(darkMode, 81, 91, 86, 171, 183, 177));
	const wchar_t* stepTitles[] = { L"\u786e\u8ba4\u72b6\u6001", L"\u8c03\u6574\u7b56\u7565", L"\u67e5\u770b\u65e5\u5fd7" };
	const wchar_t* stepDetails[] = {
		L"\u5728\u201c\u72b6\u6001\u603b\u89c8\u201d\u4e2d\u68c0\u67e5\u5b66\u751f\u7aef\u8fdb\u7a0b\u548c\u9632\u62a4\u670d\u52a1\u662f\u5426\u6b63\u5e38\u3002",
		L"\u5728\u201c\u9632\u62a4\u7b56\u7565\u201d\u4e2d\u9009\u62e9\u5141\u8bb8\u7684\u8fdc\u7a0b\u884c\u4e3a\uff0c\u4fee\u6539\u540e\u7acb\u5373\u5e94\u7528\u3002",
		L"\u5982\u679c\u9632\u62a4\u672a\u6309\u9884\u671f\u5de5\u4f5c\uff0c\u8bf7\u5728\u201c\u8fd0\u884c\u65e5\u5fd7\u201d\u4e2d\u67e5\u770b\u539f\u56e0\u3002"
	};
	// 三步固定高度、从上排下来。原来按可用高度拉伸到 112px，
	// 每张卡里只有一行字，卡片反而显得空。
	const float stepsTop = y + 96.0f;
	const float stepH = 88.0f;
	const float stepGap = 16.0f;
	for (int i = 0; i < 3; ++i) {
		const float rowY = stepsTop + i * (stepH + stepGap);
		// 步骤卡原来是白底画在同一张白卡上，边界完全看不见，三步像是直接浮着。
		FillRound(g, RectF(x + 28, rowY, w - 56, stepH), 12, C(darkMode, 246, 250, 247, 30, 36, 34));
		StrokeRound(g, RectF(x + 28, rowY, w - 56, stepH), 12, C(darkMode, 228, 236, 231, 46, 54, 50));
		// 序号改实心主色圆 + 白字，原来淡青底配青字几乎读不出来。
		const float badgeY = rowY + (stepH - 44.0f) / 2.0f;
		FillRound(g, RectF(x + 48, badgeY, 44, 44), 22, C(darkMode, 0, 121, 107, 130, 213, 199));
		wchar_t number[8];
		swprintf_s(number, L"%d", i + 1);
		Text(g, number, x + 48, badgeY + 11, 44, 22, 15, FontStyleBold,
			C(darkMode, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
		// 标题和说明并排：卡片有 860px 宽，堆成两行会让右侧空掉一大片。
		const float lineY = rowY + stepH / 2.0f - 12.0f;
		Text(g, stepTitles[i], x + 112, lineY, 168, 24, 14, FontStyleBold,
			C(darkMode, 26, 32, 29, 224, 231, 227));
		Text(g, stepDetails[i], x + 292, lineY + 3.0f, w - 340, 22, 11, FontStyleRegular,
			C(darkMode, 88, 97, 92, 170, 181, 175));
	}
	// 下半截原来是空白，补一张当前生效的快捷键卡：正好把"高级配置里改过什么"
	// 在帮助页也看得见，不用来回切页。
	const float infoY = stepsTop + 3.0f * stepH + 2.0f * stepGap + 16.0f;
	const float infoH = static_cast<float>(height) - 24.0f - infoY;
	if (infoH >= 96.0f) {
		const Color body = C(darkMode, 88, 97, 92, 170, 181, 175);
		FillRound(g, RectF(x + 28, infoY, w - 56, infoH), 12, C(darkMode, 246, 250, 247, 30, 36, 34));
		StrokeRound(g, RectF(x + 28, infoY, w - 56, infoH), 12, C(darkMode, 228, 236, 231, 46, 54, 50));
		Text(g, L"\u5f53\u524d\u5feb\u6377\u952e", x + 48, infoY + 18, 220, 24, 13, FontStyleBold,
			C(darkMode, 26, 32, 29, 224, 231, 227));
		const wchar_t* hotkeyLabels[] = { L"\u5207\u5230\u66ff\u6362\u753b\u9762", L"\u663e\u793a / \u9690\u85cf\u4e3b\u7a97\u53e3" };
		const int hotkeyValues[] = { hotkeyFakeFullValue, hotkeyShowHideValue };
		for (int i = 0; i < 2; ++i) {
			const float rowY = infoY + 52.0f + i * 30.0f;
			if (rowY + 22.0f > infoY + infoH) break;
			Text(g, hotkeyLabels[i], x + 48, rowY, 240, 20, 11, FontStyleRegular, body);
			const std::wstring value = HotkeyDisplayText(hotkeyValues[i]);
			FillRound(g, RectF(x + 300, rowY - 3, 140, 24), 8, C(darkMode, 255, 255, 255, 34, 41, 38));
			Text(g, value.c_str(), x + 300, rowY, 140, 20, 10, FontStyleBold,
				C(darkMode, 34, 42, 39, 224, 232, 228), StringAlignmentCenter);
		}
		Text(g, L"\u53ef\u5728\u201c\u9ad8\u7ea7\u914d\u7f6e \u00b7 \u5e38\u89c4\u4e0e\u5feb\u6377\u952e\u201d\u91cc\u4fee\u6539\uff0c\u4fdd\u5b58\u540e\u91cd\u542f\u751f\u6548\u3002",
			x + 48, infoY + infoH - 34.0f, w - 96, 20, 10, FontStyleRegular, body);
	}
}

void MainWindow::PaintNetwork(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	// The original Teacher GUI is hosted as a child window inside this shell.
	{
		const Color surface = C(darkMode, 255, 255, 255, 33, 40, 37);
		const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
		const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
		SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
		FillRound(g, RectF(x, y, w, h), 12, surface);
		const int hostX = 264;
		const int hostY = 170;
		// hostX 是卡片左内边距 16 处的 264，右边缘也要留同样的 16，
		// 原来减 24 会让宿主区比卡片宽出 4px，右上角露出一道白缝。
		const int hostW = std::max(560, width - hostX - 44);
		const int hostH = std::max(320, height - hostY - 24);
		teacherLaunchRect = { hostX, 120, hostX + 144, 154 };
		FillRound(g, RectF(static_cast<float>(hostX), static_cast<float>(hostY), static_cast<float>(hostW), static_cast<float>(hostH)), 12 ,
			C(darkMode, 248, 251, 249, 31, 37, 35));
		const bool teacherRunning = teacherProcess != nullptr && teacherChildWindow != nullptr;
		if (teacherRunning) {
			Text(g, L"\u5df2\u5d4c\u5165\u539f\u6559\u5e08\u7aef\u7a97\u53e3", hostX + 168, 126, w - 192, 24, 11, FontStyleBold, title);
			Text(g, L"\u4e0b\u65b9\u533a\u57df\u4e3a\u539f\u7a97\u53e3\u5185\u5bb9\uff0c\u53ef\u76f4\u63a5\u64cd\u4f5c\u3002", hostX + 168, 148, w - 192, 20, 10, FontStyleRegular, secondary);
		}
		else {
			// 宿主区没嵌入窗口时是一大片空白，这里补一个居中的空状态，
			// 否则整个页面看起来像加载失败。按钮旁边不再重复写一遍，
			// 否则同一句话在页面上出现两次。
			const float hostCenterX = hostX + hostW / 2.0f;
			const float hostCenterY = hostY + hostH / 2.0f;
			FillRound(g, RectF(hostCenterX - 34, hostCenterY - 86, 68, 68), 20,
				C(darkMode, 232, 241, 236, 41, 48, 44));
			DrawIcon(g, 5, hostCenterX - 15, hostCenterY - 67,
				C(darkMode, 96, 118, 110, 140, 156, 148), 30);
			Text(g, L"\u539f\u6559\u5e08\u7aef\u5df2\u5c31\u7eea\uff0c\u7b49\u5f85\u542f\u52a8",
				hostCenterX - 220, hostCenterY, 440, 28, 15, FontStyleBold, title, StringAlignmentCenter);
			Text(g, L"\u70b9\u51fb\u5de6\u4e0a\u89d2\u7684\u300c\u542f\u52a8\u6559\u5e08\u670d\u52a1\u300d\uff0c\u539f\u7aef\u7a97\u53e3\u4f1a\u5d4c\u5165\u5230\u8fd9\u4e2a\u533a\u57df",
				hostCenterX - 260, hostCenterY + 32, 520, 22, 11, FontStyleRegular, secondary, StringAlignmentCenter);
		}
		return;
	}
	// The teacher endpoint is rendered by this GDI+ page; its service is only a
	// native backend and never creates a second window.
	{
		const Color surface = C(darkMode, 255, 255, 255, 33, 40, 37);
		const Color field = C(darkMode, 248, 251, 249, 31, 37, 35);
		const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
		const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
		SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
		FillRound(g, RectF(x, y, w, h), 12, surface);
		const int topY = static_cast<int>(y + 78);
		FillRound(g, RectF(x + 16, static_cast<float>(topY), w - 32, 54), 12, field);
		const bool teacherRunning = teacherBackend && teacherBackend->running;
		Text(g, teacherRunning ? L"\u6559\u5e08\u670d\u52a1\u8fd0\u884c\u4e2d" : L"\u6559\u5e08\u670d\u52a1\u672a\u542f\u52a8", x + 32, static_cast<float>(topY + 12), 180, 22, 11, FontStyleBold, title);
		Text(g, teacherRunning ? L"\u5df2\u76d1\u542c 4705 / \u4f1a\u8bdd\u7ec4\u64ad\uff0c\u6b63\u5728\u63a5\u6536\u5b66\u751f\u5217\u8868" : L"\u70b9\u51fb\u542f\u52a8\u6559\u5e08\u670d\u52a1\u5f00\u59cb\u81ea\u52a8\u53d1\u73b0", x + 220, static_cast<float>(topY + 14), w - 360, 20, 10, FontStyleRegular, secondary);
		const float cardY = y + 148;
		const float listW = std::min(520.0f, w * 0.46f);
		FillRound(g, RectF(x + 16, cardY, listW, h - 170), 12, field);
		FillRound(g, RectF(x + 28 + listW, cardY, w - listW - 44, h - 170), 12, field);
		Text(g, L"\u5b66\u751f\u5217\u8868", x + 32, cardY + 14, 180, 22, 11, FontStyleBold, title);
		Text(g, L"\u81ea\u52a8\u53d1\u73b0\u3001\u9884\u89c8\u3001\u4fe1\u606f\u3001\u804a\u5929\u548c\u8fdc\u7a0b\u63a7\u5236", x + 32, cardY + 38, listW - 32, 18, 10, FontStyleRegular, secondary);
		Text(g, L"\u5b66\u751f\u64cd\u4f5c", x + 44 + listW, cardY + 14, 180, 22, 11, FontStyleBold, title);
		Text(g, L"\u9009\u62e9\u5de6\u4fa7\u5b66\u751f\u540e\u53d1\u9001\u64cd\u4f5c", x + 44 + listW, cardY + 38, w - listW - 76, 18, 10, FontStyleRegular, secondary);
		const float previewX = x + 44 + listW;
		const float previewY = cardY + 252;
		const float previewW = std::max(260.0f, w - listW - 76.0f);
		const float previewH = std::max(132.0f, h - 170.0f - (previewY - cardY) - 16.0f);
		FillRound(g, RectF(previewX, previewY, previewW, previewH), 12, C(darkMode, 20, 25, 23, 255, 28, 25));
		Text(g, L"\u5c4f\u5e55\u9884\u89c8", previewX + 14, previewY + 12, 120, 20, 11, FontStyleBold, C(darkMode, 210, 218, 214, 190, 199, 194));
		Bitmap* preview = nullptr;
		if (teacherBackend) {
			if (teacherBackend->remote_bitmap && teacherBackend->remote_ip == teacherBackend->selected_ip) preview = teacherBackend->remote_bitmap.get();
			else if (!teacherBackend->latest_preview.empty() && teacherBackend->latest_preview != teacherBackend->displayed_preview) {
				teacherBackend->preview_bitmap = std::make_unique<Bitmap>(teacherBackend->latest_preview.wstring().c_str());
				teacherBackend->displayed_preview = teacherBackend->latest_preview;
				if (!teacherBackend->preview_bitmap || teacherBackend->preview_bitmap->GetLastStatus() != Ok) teacherBackend->preview_bitmap.reset();
				preview = teacherBackend->preview_bitmap.get();
			}
			else preview = teacherBackend->preview_bitmap.get();
		}
		if (preview && preview->GetWidth() > 0 && preview->GetHeight() > 0) {
			const float scale = std::min((previewW - 20.0f) / preview->GetWidth(), (previewH - 46.0f) / preview->GetHeight());
			const float drawW = preview->GetWidth() * std::max(0.05f, scale);
			const float drawH = preview->GetHeight() * std::max(0.05f, scale);
			g.DrawImage(preview, RectF(previewX + (previewW - drawW) * 0.5f, previewY + 38.0f + (previewH - 38.0f - drawH) * 0.5f, drawW, drawH));
		}
		else Text(g, L"\u9009\u62e9\u5b66\u751f\u540e\u70b9\u51fb\u5237\u65b0\u9884\u89c8", previewX + 16, previewY + previewH * 0.5f - 8, previewW - 32, 24, 11, FontStyleRegular,
			C(darkMode, 142, 151, 146, 150, 161, 155), StringAlignmentCenter);
		return;
	}
#if 0
	const float panelX = x + 16;
	const float panelW = w - 32;
	const float innerX = x + 32;
	const float innerW = w - 64;
	const float fieldW = std::max(174.0f, innerW - 422.0f);
	const Color surface = C(darkMode, 255, 255, 255, 33, 40, 37);
	const Color field = C(darkMode, 248, 251, 249, 31, 37, 35);
	const Color outline = C(darkMode, 116, 124, 120, 101, 113, 107);
	const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, surface);
	Text(g, L"\u7f51\u7edc\u8bca\u65ad\u4e0e\u6d88\u606f", innerX, y + 18, 300, 28, 17, FontStyleBold, title);
	Text(g, L"\u4f7f\u7528\u5c40\u57df\u7f51\u626b\u63cf\u3001\u7aef\u53e3\u68c0\u6d4b\u548c UDP \u5de5\u5177", innerX, y + 48, 420, 18, 10, FontStyleRegular, secondary);

	FillRound(g, RectF(panelX, y + 76, panelW, 72), 12, field);
	Text(g, L"\u76ee\u6807 IP", innerX, y + 100, 64, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 91, fieldW + 8, 38), 12, field);
	StrokeRound(g, RectF(innerX + 70, y + 91, fieldW + 8, 38), 12, outline);
	Text(g, L"\u7aef\u53e3", innerX + 88 + fieldW, y + 100, 42, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(innerX + 132 + fieldW, y + 91, 80, 38), 12, field);
	StrokeRound(g, RectF(innerX + 132 + fieldW, y + 91, 80, 38), 12, outline);

	FillRound(g, RectF(panelX, y + 156, panelW, 58), 12, field);
	Text(g, L"\u6d88\u606f", innerX, y + 177, 64, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 166, innerW - 180, 38), 12, field);
	StrokeRound(g, RectF(innerX + 70, y + 166, innerW - 180, 38), 12, outline);

	FillRound(g, RectF(panelX, y + 222, panelW, 58), 12, field);
	Text(g, L"\u547d\u4ee4", innerX, y + 243, 64, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 232, innerW - 180, 38), 12, field);
	StrokeRound(g, RectF(innerX + 70, y + 232, innerW - 180, 38), 12, outline);

	Text(g, L"\u5feb\u6377\u64cd\u4f5c", innerX, y + 300, 120, 20, 11, FontStyleBold, title);
	Text(g, L"\u8fd0\u884c\u7ed3\u679c", innerX, y + 356, 120, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(panelX, y + 382, panelW, std::max(72.0f, h - 398)), 12, field);
	StrokeRound(g, RectF(panelX, y + 382, panelW, std::max(72.0f, h - 398)), 12, C(darkMode, 199, 205, 201, 67, 75, 71));

	FillRound(g, RectF(x + 8, y + 8, w - 16, h - 16), 12 ,
		darkMode ? Color(232, 17, 20, 19) : Color(232, 246, 248, 246));
	const float centerY = y + h * 0.5f - 82.0f;
	FillRound(g, RectF(x + w * 0.5f - 28, centerY, 56, 56), 12, C(darkMode, 219, 238, 232, 43, 76, 69));
	DrawIcon(g, 6, x + w * 0.5f - 10, centerY + 18, C(darkMode, 0, 81, 72, 177, 239, 228), 20);
	Text(g, L"\u65b0\u7248\u53cd\u63a7\u5de5\u5177\u6b63\u5728\u5f00\u53d1\u4e2d", x + 40, centerY + 76, w - 80, 34, 20, FontStyleBold,
		C(darkMode, 35, 42, 39, 222, 230, 226), StringAlignmentCenter);
	Text(g, L"\u8bf7\u524d\u5f80 GitHub \u9879\u76ee\u67e5\u770b\u5f00\u53d1\u8fdb\u5ea6\u4e0e\u72ec\u7acb\u7248\u672c", x + 40, centerY + 116, w - 80, 24, 11, FontStyleRegular,
		C(darkMode, 91, 99, 95, 166, 177, 171), StringAlignmentCenter);
	const float linkW = std::min(360.0f, w - 80.0f);
	const float linkX = x + (w - linkW) * 0.5f;
	networkProjectRect = { static_cast<LONG>(linkX), static_cast<LONG>(centerY + 158), static_cast<LONG>(linkX + linkW), static_cast<LONG>(centerY + 202) };
	FillRound(g, RectF(linkX, centerY + 158, linkW, 44), 22,
		Blend(C(darkMode, 183, 232, 222, 42, 79, 72), C(darkMode, 157, 220, 207, 55, 96, 87), hoverProgress[72]));
	Text(g, L"GitHub  /  yunsjxh/Third-party-JiYu-Teacher-Endpoint", linkX + 16, centerY + 170, linkW - 32, 20, 11, FontStyleBold,
		C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	const int hostX = 264;
		const int hostY = 170;
	const int hostW = std::max(560, width - hostX - 24);
	const int hostH = std::max(320, height - hostY - 24);
	FillRound(g, RectF(static_cast<float>(hostX), static_cast<float>(hostY), static_cast<float>(hostW), static_cast<float>(hostH)), 12 ,
		C(darkMode, 248, 251, 249, 31, 37, 35));
	teacherLaunchRect = { hostX, 120, hostX + 144, 154 };
	const bool teacherRunning = teacherProcess != nullptr && teacherChildWindow != nullptr;
	Text(g, teacherRunning ? L"\u6559\u5e08\u7aef\u5df2\u5d4c\u5165\u8fd0\u884c" : L"\u6559\u5e08\u7aef\u5c1a\u672a\u542f\u52a8", hostX + 168, 126, w - 192, 24, 11, FontStyleBold,
		C(darkMode, 35, 42, 39, 222, 230, 226));
	Text(g, teacherRunning ? L"\u7a97\u53e3\u548c\u7f51\u7edc\u670d\u52a1\u5728\u6b64\u533a\u57df\u5185\u8fd0\u884c\u3002" : L"\u70b9\u51fb\u4e0a\u65b9\u6309\u94ae\u542f\u52a8\u5e76\u5d4c\u5165 cpp-gui\u3002", hostX + 168, 148, w - 192, 20, 10, FontStyleRegular,
	C(darkMode, 91, 99, 95, 166, 177, 171));
#endif
}

void MainWindow::PaintDiagnostics(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	const Color surface = C(darkMode, 255, 255, 255, 33, 40, 37);
	const Color field = C(darkMode, 248, 251, 249, 31, 37, 35);
	const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, surface);
	Text(g, L"网络与 USB 阻断诊断", x + 20, y + 18, 330, 26, 17, FontStyleBold, title);
	Text(g, L"选择驱动查看位置、签名、服务和筛选依据；手动卸载前会要求确认。", x + 20, y + 50, w - 190, 20, 11, FontStyleRegular, secondary);
	diagnosticsHideOwnRect = { static_cast<LONG>(x + w - 326), static_cast<LONG>(y + 16),
		static_cast<LONG>(x + w - 172), static_cast<LONG>(y + 50) };
	PaintButton(g, RectF(x + w - 326, y + 16, 154, 34),
		diagnosticsHideOwnDrivers ? L"显示本程序条目" : L"隐藏本程序条目", 92,
		diagnosticsHideOwnDrivers ? ButtonStyle::Tonal : ButtonStyle::Ghost, 17);
	diagnosticsScanRect = { static_cast<LONG>(x + w - 152), static_cast<LONG>(y + 16),
		static_cast<LONG>(x + w - 20), static_cast<LONG>(y + 50) };
	PaintButton(g, RectF(x + w - 152, y + 16, 132, 34),
		diagnosticsRunning ? L"正在检查..." : L"开始诊断", 90, ButtonStyle::Primary, 17);

	const float metricY = y + 78;
	const float metricW = (w - 32 - 36) / 4.0f;
	const std::array<std::pair<std::wstring, std::wstring>, 4> metrics = {{
		{ L"网络适配器", std::to_wstring(diagnosticsAdapterUp) + L" / " + std::to_wstring(diagnosticsAdapterTotal) + L" 活动" },
		{ L"USB 设备", std::to_wstring(diagnosticsUsbTotal) + L" 个，异常 " + std::to_wstring(diagnosticsUsbIssueCount) },
		{ L"高风险驱动", std::to_wstring(std::count_if(diagnosticsDrivers.begin(), diagnosticsDrivers.end(), [](const DiagnosticDriver& item) { return item.highRisk; })) + L" 项" },
		{ L"过滤器注册", std::to_wstring(diagnosticsClassFilters.size()) + L" 项" }
	}};
	for (size_t i = 0; i < metrics.size(); ++i) {
		const float cardX = x + 16 + static_cast<float>(i) * (metricW + 12);
		FillRound(g, RectF(cardX, metricY, metricW, 60), 12, field);
		Text(g, metrics[i].first.c_str(), cardX + 14, metricY + 12, metricW - 28, 16, 10, FontStyleRegular, secondary);
		Text(g, metrics[i].second.c_str(), cardX + 14, metricY + 32, metricW - 28, 18, 11, FontStyleBold, title);
	}

	const float panelY = metricY + 76;
	const float listX = x + 16;
	const float listW = std::max(330.0f, (w - 48) * 0.56f);
	const float detailX = listX + listW + 16;
	const float detailW = x + w - 16 - detailX;
	const float panelH = std::max(150.0f, y + h - panelY - 16);
	FillRound(g, RectF(listX, panelY, listW, panelH), 12, field);
	FillRound(g, RectF(detailX, panelY, detailW, panelH), 12, field);
	diagnosticsListRect = { static_cast<LONG>(listX), static_cast<LONG>(panelY),
		static_cast<LONG>(listX + listW), static_cast<LONG>(panelY + panelH) };
	Text(g, L"已加载驱动", listX + 16, panelY + 14, 130, 20, 12, FontStyleBold, title);
	Text(g, std::to_wstring(diagnosticsDrivers.size()).c_str(), listX + listW - 48, panelY + 15, 32, 18, 11, FontStyleBold, secondary, StringAlignmentFar);

	const std::array<const wchar_t*, 4> filterNames = { L"全部", L"高风险", L"已签名", L"待核查" };
	const float filterY = panelY + 42;
	const float filterW = (listW - 32 - 18) / 4.0f;
	for (size_t i = 0; i < filterNames.size(); ++i) {
		const float buttonX = listX + 16 + static_cast<float>(i) * (filterW + 6);
		diagnosticsFilterRects[i] = { static_cast<LONG>(buttonX), static_cast<LONG>(filterY), static_cast<LONG>(buttonX + filterW), static_cast<LONG>(filterY + 28) };
		const bool active = static_cast<int>(diagnosticsFilter) == static_cast<int>(i);
		FillRound(g, RectF(buttonX, filterY + (pressTarget == static_cast<int>(93 + i) ? 1.0f : 0.0f), filterW, 28), 14,
			active ? C(darkMode, 183, 232, 222, 42, 79, 72)
				: Blend(C(darkMode, 235, 240, 237, 48, 56, 52), C(darkMode, 214, 232, 224, 56, 76, 69), hoverProgress[93 + static_cast<int>(i)]));
		Text(g, filterNames[i], buttonX, filterY + 7, filterW, 16, 10, active ? FontStyleBold : FontStyleRegular,
			active ? C(darkMode, 0, 81, 72, 177, 239, 228) : secondary, StringAlignmentCenter);
	}

	diagnosticsVisibleDrivers.clear();
	for (size_t i = 0; i < diagnosticsDrivers.size(); ++i) {
		const DiagnosticDriver& driver = diagnosticsDrivers[i];
		const bool include = diagnosticsFilter == DiagnosticsFilter::All ||
			(diagnosticsFilter == DiagnosticsFilter::HighRisk && driver.highRisk) ||
			(diagnosticsFilter == DiagnosticsFilter::Signed && driver.signedDriver) ||
			(diagnosticsFilter == DiagnosticsFilter::Review && driver.needsReview);
		if (include && !(diagnosticsHideOwnDrivers && driver.ownedByJiYu))
			diagnosticsVisibleDrivers.push_back(i);
	}
	for (RECT& rect : diagnosticsDriverRects) rect = {};
	const float rowY = panelY + 82;
	const int visibleRows = std::max(1, std::min(static_cast<int>(diagnosticsDriverRects.size()), static_cast<int>((panelH - 94) / 54)));
	diagnosticsVisibleRowCount = visibleRows;
	const int maxOffset = std::max(0, static_cast<int>(diagnosticsVisibleDrivers.size()) - visibleRows);
	diagnosticsListOffset = std::max(0, std::min(diagnosticsListOffset, maxOffset));
	if (diagnosticsDrivers.empty()) {
		Text(g, L"点击“开始诊断”收集已加载驱动。", listX + 20, rowY + 16, listW - 40, 22, 11, FontStyleRegular, secondary);
	}
	else if (diagnosticsVisibleDrivers.empty()) {
		Text(g, L"当前筛选没有匹配的驱动。", listX + 20, rowY + 16, listW - 40, 22, 11, FontStyleRegular, secondary);
	}
	else {
		for (int row = 0; row < visibleRows && diagnosticsListOffset + row < static_cast<int>(diagnosticsVisibleDrivers.size()); ++row) {
			const size_t driverIndex = diagnosticsVisibleDrivers[static_cast<size_t>(diagnosticsListOffset + row)];
			const DiagnosticDriver& driver = diagnosticsDrivers[driverIndex];
			const float itemY = rowY + row * 54.0f;
			const bool selected = diagnosticsSelectedDriver == static_cast<int>(driverIndex);
			const Color statusColor = driver.highRisk ? C(darkMode, 213, 83, 83, 244, 149, 149) :
				(driver.needsReview ? C(darkMode, 184, 130, 23, 239, 198, 101) : C(darkMode, 0, 127, 112, 130, 213, 199));
			FillRound(g, RectF(listX + 10, itemY, listW - 20, 46), 6,
				selected ? C(darkMode, 218, 239, 233, 46, 65, 60)
					: Blend(C(darkMode, 245, 248, 246, 43, 49, 46), C(darkMode, 232, 240, 236, 50, 58, 54), hoverProgress[100 + row]));
			FillRound(g, RectF(listX + 22, itemY + 16, 12, 12), 6, statusColor);
			Text(g, driver.name.c_str(), listX + 46, itemY + 8, listW - 170, 16, 11, FontStyleBold, title);
			Text(g, driver.path.c_str(), listX + 46, itemY + 26, listW - 64, 14, 10, FontStyleRegular, secondary);
			Text(g, driver.highRisk ? L"高风险" : (driver.needsReview ? L"核查" : L"已签名"), listX + listW - 90, itemY + 14, 58, 16, 10, FontStyleBold, statusColor, StringAlignmentFar);
			diagnosticsDriverRects[static_cast<size_t>(row)] = { static_cast<LONG>(listX + 10), static_cast<LONG>(itemY), static_cast<LONG>(listX + listW - 10), static_cast<LONG>(itemY + 46) };
		}
		if (maxOffset > 0) {
			const float trackY = rowY + 4;
			const float trackH = visibleRows * 54.0f - 12;
			const float thumbH = std::max(24.0f, trackH * visibleRows / static_cast<float>(diagnosticsVisibleDrivers.size()));
			const float thumbY = trackY + (trackH - thumbH) * diagnosticsListOffset / static_cast<float>(maxOffset);
			FillRound(g, RectF(listX + listW - 17, trackY, 4, trackH), 2, C(darkMode, 212, 220, 216, 69, 78, 74));
			FillRound(g, RectF(listX + listW - 17, thumbY, 4, thumbH), 2, C(darkMode, 0, 107, 95, 130, 213, 199));
			const std::wstring range = std::to_wstring(diagnosticsListOffset + 1) + L"-" +
				std::to_wstring(std::min(static_cast<int>(diagnosticsVisibleDrivers.size()), diagnosticsListOffset + visibleRows)) +
				L" / " + std::to_wstring(diagnosticsVisibleDrivers.size());
			Text(g, range.c_str(), listX + listW - 110, panelY + 15, 56, 18, 10, FontStyleRegular, secondary, StringAlignmentFar);
		}
	}

	Text(g, L"驱动详情", detailX + 18, panelY + 16, detailW - 36, 20, 12, FontStyleBold, title);
	diagnosticsUnloadRect = {};
	const DiagnosticDriver* selectedDriver = diagnosticsSelectedDriver >= 0 && diagnosticsSelectedDriver < static_cast<int>(diagnosticsDrivers.size())
		? &diagnosticsDrivers[static_cast<size_t>(diagnosticsSelectedDriver)] : nullptr;
	if (!selectedDriver) {
		Text(g, L"从左侧选择一项驱动查看证据。", detailX + 18, panelY + 58, detailW - 36, 22, 11, FontStyleRegular, secondary);
		Text(g, L"诊断只给出风险线索，不对驱动功能或意图下结论。", detailX + 18, panelY + 86, detailW - 36, 38, 11, FontStyleRegular, secondary);
	}
	else {
		const Color detailStatus = selectedDriver->highRisk ? C(darkMode, 213, 83, 83, 244, 149, 149) :
			(selectedDriver->needsReview ? C(darkMode, 184, 130, 23, 239, 198, 101) : C(darkMode, 0, 127, 112, 130, 213, 199));
		FillRound(g, RectF(detailX + 18, panelY + 50, detailW - 36, 30), 6, C(darkMode, 245, 248, 246, 43, 49, 46));
		Text(g, selectedDriver->rating.c_str(), detailX + 30, panelY + 58, detailW - 60, 16, 10, FontStyleBold, detailStatus);
		diagnosticsUnloadRect = { static_cast<LONG>(detailX + detailW - 132), static_cast<LONG>(panelY + 88),
			static_cast<LONG>(detailX + detailW - 18), static_cast<LONG>(panelY + 120) };
		PaintButton(g, RectF(detailX + detailW - 132, panelY + 88, 114, 32),
			selectedDriver->canUnload ? L"停止并卸载" : L"不可操作", 91,
			ButtonStyle::Danger, 16, -1, selectedDriver->canUnload);
		const std::array<std::pair<const wchar_t*, const std::wstring*>, 5> fields = {{
			{ L"文件名", &selectedDriver->name }, { L"发布者", &selectedDriver->signer },
			{ L"加载路径", &selectedDriver->path }, { L"服务", &selectedDriver->service },
			{ L"筛选依据", &selectedDriver->evidence }
		}};
		for (size_t i = 0; i < fields.size(); ++i) {
			const float fieldY = panelY + 132 + static_cast<float>(i) * 52;
			Text(g, fields[i].first, detailX + 18, fieldY, detailW - 36, 15, 10, FontStyleBold, secondary);
			Text(g, fields[i].second->c_str(), detailX + 18, fieldY + 19, detailW - 36, 28, 10, FontStyleRegular, title);
		}
	}
}

namespace {

std::wstring FormatWin32Error(DWORD error)
{
	wchar_t message[256] = {};
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
		error, 0, message, static_cast<DWORD>(_countof(message)), nullptr);
	std::wstring result = message;
	while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
	return result;
}

std::wstring GetDevicePropertyString(HDEVINFO info, SP_DEVINFO_DATA* data, DWORD property)
{
	wchar_t buffer[512] = {};
	DWORD type = 0;
	DWORD size = sizeof(buffer);
	if (SetupDiGetDeviceRegistryPropertyW(info, data, property, &type,
		reinterpret_cast<PBYTE>(buffer), size, &size) && buffer[0] != L'\0') return buffer;
	return L"(未知设备)";
}

bool IsCandidateProcessName(const std::wstring& name)
{
	std::wstring lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t value) {
		return static_cast<wchar_t>(towlower(value));
	});
	static const wchar_t* terms[] = { L"filter", L"block", L"usb", L"ndis", L"vpn", L"firewall", L"protect" };
	for (const wchar_t* term : terms) if (lower.find(term) != std::wstring::npos) return true;
	return false;
}

std::wstring Lowercase(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
		return static_cast<wchar_t>(towlower(c));
	});
	return value;
}

bool ContainsAny(const std::wstring& value, std::initializer_list<const wchar_t*> terms)
{
	for (const wchar_t* term : terms)
		if (value.find(term) != std::wstring::npos) return true;
	return false;
}

bool IsNetworkOrUsbDriver(const std::wstring& path)
{
	const std::wstring lower = Lowercase(path);
	return ContainsAny(lower, { L"ndis", L"net", L"tcpip", L"wfp", L"filter", L"vpn", L"usb", L"usbhub", L"usbccgp" });
}

bool IsCurrentUserProfilePath(const std::wstring& path)
{
	WCHAR userProfile[MAX_PATH] = {};
	if (!GetEnvironmentVariableW(L"USERPROFILE", userProfile, _countof(userProfile))) return false;
	return Lowercase(path).rfind(Lowercase(userProfile), 0) == 0;
}

bool IsWindowsImage(const std::wstring& path)
{
	WCHAR windowsDirectory[MAX_PATH] = {};
	if (!GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory))) return true;
	std::wstring root = Lowercase(windowsDirectory);
	std::wstring normalized = Lowercase(path);
	std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
	return normalized.rfind(root + L"\\", 0) == 0;
}

bool IsMicrosoftPublisher(const std::wstring& publisher)
{
	return Lowercase(publisher).find(L"microsoft") != std::wstring::npos;
}

std::wstring ResolveKernelDriverPath(const std::wstring& rawPath)
{
	std::wstring path = rawPath;
	if (path.rfind(L"\\SystemRoot\\", 0) == 0) {
		WCHAR windowsDirectory[MAX_PATH] = {};
		if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)))
			return std::wstring(windowsDirectory) + path.substr(wcslen(L"\\SystemRoot"));
	}
	if (path.rfind(L"\\??\\", 0) == 0) path.erase(0, 4);
	if (path.rfind(L"\\Device\\", 0) != 0) return path;

	WCHAR drives[512] = {};
	const DWORD length = GetLogicalDriveStringsW(_countof(drives), drives);
	for (const WCHAR* drive = drives; drive && *drive; drive += wcslen(drive) + 1) {
		WCHAR deviceName[1024] = {};
		std::wstring driveName(drive, 2);
		if (!QueryDosDeviceW(driveName.c_str(), deviceName, _countof(deviceName))) continue;
		const size_t deviceLength = wcslen(deviceName);
		if (_wcsnicmp(path.c_str(), deviceName, deviceLength) == 0)
			return driveName + path.substr(deviceLength);
	}
	return path;
}

std::wstring GetDriverPathForFileAccess(const std::wstring& displayPath)
{
	BOOL wow64 = FALSE;
	if (!IsWow64Process(GetCurrentProcess(), &wow64) || !wow64) return displayPath;

	WCHAR windowsDirectory[MAX_PATH] = {};
	if (!GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory))) return displayPath;
	const std::wstring system32Prefix = Lowercase(std::wstring(windowsDirectory) + L"\\System32\\");
	const std::wstring lowerPath = Lowercase(displayPath);
	if (lowerPath.rfind(system32Prefix, 0) != 0) return displayPath;
	return std::wstring(windowsDirectory) + L"\\Sysnative\\" + displayPath.substr(system32Prefix.size());
}

std::wstring DriverFileName(const std::wstring& path)
{
	const size_t separator = path.find_last_of(L"\\/");
	return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

enum class SignatureState { Valid, Unsigned, Unknown };

struct SignatureEvidence {
	SignatureState state = SignatureState::Unknown;
	LONG status = 0;
	std::wstring publisher;
};

SignatureEvidence VerifyDriverSignature(const std::wstring& path)
{
	// QueryDriverSignature checks both embedded and Windows catalog signatures.
	// A directory such as System32 is not itself treated as a trust signal.
	wchar_t publisher[256] = {};
	LONG status = E_FAIL;
	const BOOL verified = QueryDriverSignature(path.c_str(), publisher, _countof(publisher), &status);
	if (verified) return { SignatureState::Valid, status, publisher };
	if (status == TRUST_E_NOSIGNATURE && publisher[0] == L'\0')
		return { SignatureState::Unsigned, status, publisher };
	return { SignatureState::Unknown, status, publisher };
}

std::wstring FindMappedDriverServices(const std::wstring& fileName, std::wstring* firstServiceName)
{
	if (firstServiceName) firstServiceName->clear();
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
	if (!manager) return L"服务映射不可读";
	DWORD needed = 0, returned = 0, resume = 0;
	EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
		nullptr, 0, &needed, &returned, &resume, nullptr);
	if (GetLastError() != ERROR_MORE_DATA || needed == 0) {
		CloseServiceHandle(manager);
		return L"未找到匹配服务";
	}
	std::vector<BYTE> buffer(needed);
	resume = 0;
	if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
		buffer.data(), static_cast<DWORD>(buffer.size()), &needed, &returned, &resume, nullptr)) {
		CloseServiceHandle(manager);
		return L"服务枚举失败";
	}
	const auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
	const std::wstring target = Lowercase(fileName);
	std::wstring matches;
	for (DWORD index = 0; index < returned; ++index) {
		SC_HANDLE service = OpenServiceW(manager, entries[index].lpServiceName, SERVICE_QUERY_CONFIG);
		if (!service) continue;
		DWORD configSize = 0;
		QueryServiceConfigW(service, nullptr, 0, &configSize);
		std::vector<BYTE> configBuffer(configSize);
		const bool configured = configSize != 0 && QueryServiceConfigW(service,
			reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data()), configSize, &configSize) != FALSE;
		if (configured) {
			auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
			if (config->lpBinaryPathName && Lowercase(config->lpBinaryPathName).find(target) != std::wstring::npos) {
				if (firstServiceName && firstServiceName->empty())
					*firstServiceName = entries[index].lpServiceName;
				if (!matches.empty()) matches += L", ";
				matches += entries[index].lpServiceName;
				matches += entries[index].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING ? L"(运行)" : L"(未运行)";
			}
		}
		CloseServiceHandle(service);
	}
	CloseServiceHandle(manager);
	return matches.empty() ? L"未找到匹配服务" : matches;
}

void AppendClassFilterEvidence(std::vector<std::wstring>& results, LPCWSTR label, LPCWSTR classGuid)
{
	const std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Class\\") + classGuid;
	for (const wchar_t* valueName : { L"UpperFilters", L"LowerFilters" }) {
		DWORD type = 0, bytes = 0;
		if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), valueName, RRF_RT_REG_MULTI_SZ,
			&type, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) continue;
		std::vector<wchar_t> values(bytes / sizeof(wchar_t) + 1, L'\0');
		if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), valueName, RRF_RT_REG_MULTI_SZ,
			&type, values.data(), &bytes) != ERROR_SUCCESS) continue;
		for (const wchar_t* current = values.data(); *current; current += wcslen(current) + 1)
			results.push_back(std::wstring(L"[") + label + L"过滤器] " + valueName + L"=" + current + L"（已注册，需结合签名和服务路径核验）");
	}
}

}

void MainWindow::RunDeviceDiagnostics()
{
	if (diagnosticsRunning) return;
	diagnosticsRunning = true;
	diagnosticsDrivers.clear();
	diagnosticsVisibleDrivers.clear();
	diagnosticsClassFilters.clear();
	diagnosticsSelectedDriver = -1;
	diagnosticsListOffset = 0;
	diagnosticsVisibleRowCount = 0;
	diagnosticsAdapterTotal = 0;
	diagnosticsAdapterUp = 0;
	diagnosticsUsbTotal = 0;
	diagnosticsUsbIssueCount = 0;
	diagnosticsProcessCandidates = 0;
	InvalidateRect(_hWnd, nullptr, FALSE);

	ULONG addressSize = 0;
	DWORD addressStatus = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &addressSize);
	if (addressStatus == ERROR_BUFFER_OVERFLOW && addressSize != 0) {
		std::vector<BYTE> buffer(addressSize);
		auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
		addressStatus = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &addressSize);
		if (addressStatus == NO_ERROR) {
			for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
				++diagnosticsAdapterTotal;
				if (adapter->OperStatus == IfOperStatusUp) ++diagnosticsAdapterUp;
			}
		}
	}

	HDEVINFO usbInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_USB, nullptr, nullptr, DIGCF_PRESENT);
	if (usbInfo != INVALID_HANDLE_VALUE) {
		SP_DEVINFO_DATA data = {};
		data.cbSize = sizeof(data);
		for (DWORD index = 0; SetupDiEnumDeviceInfo(usbInfo, index, &data); ++index) {
			++diagnosticsUsbTotal;
			ULONG problem = 0;
			ULONG status = 0;
			CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0);
			if (cr != CR_SUCCESS || (status & DN_HAS_PROBLEM) != 0) ++diagnosticsUsbIssueCount;
		}
		SetupDiDestroyDeviceInfoList(usbInfo);
	}
	AppendClassFilterEvidence(diagnosticsClassFilters, L"网络", L"{4d36e972-e325-11ce-bfc1-08002be10318}");
	AppendClassFilterEvidence(diagnosticsClassFilters, L"USB", L"{36fc9e60-c465-11cf-8056-444553540000}");

	LPVOID driverAddresses[1024] = {};
	DWORD driverBytes = 0;
	if (EnumDeviceDrivers(driverAddresses, sizeof(driverAddresses), &driverBytes)) {
		const DWORD count = std::min<DWORD>(driverBytes / sizeof(LPVOID), _countof(driverAddresses));
		for (DWORD index = 0; index < count; ++index) {
			wchar_t path[MAX_PATH] = {};
			if (!GetDeviceDriverFileNameW(driverAddresses[index], path, _countof(path))) continue;
			const std::wstring resolvedPath = ResolveKernelDriverPath(path);
			// Randomized driver names reveal little. A loaded driver from the current user's
			// profile is always worth showing even when its name has no network/USB keyword.
			const bool userWritablePath = IsCurrentUserProfilePath(resolvedPath);
			if (!IsNetworkOrUsbDriver(resolvedPath) && !userWritablePath) continue;
			const std::wstring fileAccessPath = GetDriverPathForFileAccess(resolvedPath);
			const DWORD attributes = GetFileAttributesW(fileAccessPath.c_str());
			const bool readableFile = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
			const SignatureEvidence signature = readableFile ? VerifyDriverSignature(fileAccessPath) : SignatureEvidence{};
			std::wstring rating;
			if (userWritablePath) rating = L"高风险待核查：用户目录加载";
			else if (signature.state == SignatureState::Unsigned) rating = L"高风险待核查：未找到有效签名";
			else if (signature.state == SignatureState::Valid && IsMicrosoftPublisher(signature.publisher)) rating = L"已验证：Microsoft 签名";
			else if (signature.state == SignatureState::Valid) rating = L"高风险待核查：第三方签名";
			else rating = readableFile ? L"需人工核查：签名未能离线确认" : L"需人工核查：无法解析磁盘路径";
			DiagnosticDriver driver;
			driver.name = DriverFileName(resolvedPath);
			driver.path = resolvedPath;
			driver.rating = rating;
			driver.signer = signature.publisher.empty() ? L"未识别" : signature.publisher;
			driver.service = FindMappedDriverServices(driver.name, &driver.serviceName);
		driver.ownedByJiYu =
			(_wcsicmp(driver.serviceName.c_str(), JTAppGetDriverServiceNameDirect()) == 0) ||
			(_wcsicmp(driver.serviceName.c_str(), JTAppGetAvServiceNameDirect()) == 0);
		driver.canUnload = !driver.serviceName.empty() &&
			!driver.ownedByJiYu &&
			!IsWindowsImage(resolvedPath) && !IsMicrosoftPublisher(signature.publisher);
			driver.highRisk = userWritablePath || signature.state == SignatureState::Unsigned ||
				(signature.state == SignatureState::Valid && !IsMicrosoftPublisher(signature.publisher));
			driver.signedDriver = signature.state == SignatureState::Valid;
			driver.needsReview = signature.state == SignatureState::Unknown;
			driver.evidence = userWritablePath ? L"已加载自当前用户目录；路径不作为签名信任依据" :
				(IsNetworkOrUsbDriver(resolvedPath) ? L"文件路径或名称与网络/USB 相关；签名已单独校验" : L"已加载驱动候选；签名已单独校验");
			diagnosticsDrivers.push_back(std::move(driver));
		}
	}

	HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (processSnapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Process32FirstW(processSnapshot, &entry)) do {
			if (IsCandidateProcessName(entry.szExeFile)) {
				++diagnosticsProcessCandidates;
			}
		} while (Process32NextW(processSnapshot, &entry));
		CloseHandle(processSnapshot);
	}

	diagnosticsRunning = false;
	if (currentLogger) currentLogger->LogInfo(L"设备诊断完成：%lu 项候选驱动", static_cast<unsigned long>(diagnosticsDrivers.size()));
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::UnloadSelectedDiagnosticDriver()
{
	if (diagnosticsSelectedDriver < 0 || diagnosticsSelectedDriver >= static_cast<int>(diagnosticsDrivers.size())) {
		ShowFastTip(L"请先选择一个驱动");
		return;
	}
	const DiagnosticDriver& driver = diagnosticsDrivers[static_cast<size_t>(diagnosticsSelectedDriver)];
	if (!driver.canUnload || driver.serviceName.empty()) {
		ShowFastTip(L"该条目不允许执行卸载");
		return;
	}
	if (MessageBoxW(_hWnd, L"将先尝试正常停止并卸载；仅在失败时使用后备卸载。是否继续？",
		L"驱动诊断", MB_YESNO | MB_ICONWARNING) != IDYES) return;
	BOOL forced = FALSE;
	DWORD errorCode = ERROR_SUCCESS;
	const BOOL unloaded = UnloadSelectedDriverService(driver.serviceName.c_str(), &forced, &errorCode);
	if (unloaded) {
		if (currentLogger) currentLogger->LogInfo(L"诊断页卸载完成：service=%s mode=%s",
			driver.serviceName.c_str(), forced ? L"后备" : L"正常");
		ShowFastTip(forced ? L"正常流程失败，后备卸载已完成" : L"已正常停止并卸载");
	}
	else {
		if (currentLogger) currentLogger->LogWarn(L"诊断页卸载失败：service=%s error=%lu",
			driver.serviceName.c_str(), static_cast<unsigned long>(errorCode));
		WCHAR message[160] = {};
		swprintf_s(message, L"卸载失败，错误码 %lu", static_cast<unsigned long>(errorCode));
		ShowFastTip(message);
	}
	RunDeviceDiagnostics();
}

void MainWindow::PaintLogs(Graphics& g, int width, int height, int offsetY)
{
	std::lock_guard<std::mutex> guard(stateMutex);
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	// 日志面板原来是纯深色，浅色主题里像贴了一块黑板。改成浅色"纸面"卡片，
	// 深色主题下才用深底，四档日志颜色按各自主题给。
	FillRound(g, RectF(x, y, w, h), 12, C(darkMode, 246, 250, 247, 25, 30, 28));
	StrokeRound(g, RectF(x, y, w, h), 12, C(darkMode, 228, 236, 231, 44, 52, 48));
	Text(g, L"\u5b9e\u65f6\u8f93\u51fa", x + 20, y + 18, 160, 20, 11, FontStyleBold, C(darkMode, 0, 107, 95, 130, 213, 199));
	exportLogRect = { static_cast<LONG>(x + w - 134), static_cast<LONG>(y + 12), static_cast<LONG>(x + w - 18), static_cast<LONG>(y + 44) };
	PaintButton(g, RectF(x + w - 134, y + 12, 116, 32), L"\u5bfc\u51fa\u65e5\u5fd7", 40, ButtonStyle::Tonal, 16);
	// 一条日志里可能自带换行（例如 "… (2)\r\n[In] xxx.cpp (704) : 函数名"）。
	// 原来整条丢给 DrawString，22px 行高里会被压成两行，和下一行叠在一起。
	// 这里先摊平成「显示行」再按固定行高排版。缓存只在条目数变化时重建。
	static std::vector<std::wstring> rowText;
	static std::vector<int> rowLevel;
	static size_t builtCount = static_cast<size_t>(-1);
	if (builtCount != logs.size()) {
		rowText.clear();
		rowLevel.clear();
		rowText.reserve(logs.size() + 8);
		rowLevel.reserve(logs.size() + 8);
		for (const auto& entry : logs) {
			const std::wstring& line = entry.text;
			size_t begin = 0;
			while (true) {
				size_t end = line.find(L'\n', begin);
				const bool last = end == std::wstring::npos;
				if (last) end = line.size();
				size_t stop = end;
				if (stop > begin && line[stop - 1] == L'\r') --stop;
				rowText.push_back(line.substr(begin, stop - begin));
				rowLevel.push_back(static_cast<int>(entry.level));
				if (last) break;
				begin = end + 1;
			}
		}
		builtCount = logs.size();
	}
	const int rowCount = static_cast<int>(rowText.size());
	const int visible = std::max(1, static_cast<int>((h - 58) / 20));
	const int start = std::max(0, rowCount - visible);
	for (int i = start; i < rowCount; ++i) {
		const float rowY = y + 50 + (i - start) * 20.0f;
		Color color = C(darkMode, 74, 84, 80, 197, 207, 202);
		if (rowLevel[i] == static_cast<int>(LogLevelInfo)) color = C(darkMode, 0, 118, 104, 130, 213, 199);
		else if (rowLevel[i] == static_cast<int>(LogLevelWarn)) color = C(darkMode, 146, 102, 0, 241, 194, 98);
		else if (rowLevel[i] == static_cast<int>(LogLevelError)) color = C(darkMode, 186, 34, 30, 255, 180, 171);
		TextSingleLine(g, rowText[i].c_str(), x + 20, rowY, w - 40, 20, 11, FontStyleRegular, color);
	}
}

void MainWindow::PaintAbout(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	const Color titleColor = C(darkMode, 30, 36, 33, 225, 233, 229);
	const Color bodyColor = C(darkMode, 81, 91, 86, 171, 183, 177);
	const Color surface = C(darkMode, 248, 251, 249, 31, 37, 35);
	SoftShadow(g, RectF(x, y, w, h), 12, darkMode);
	FillRound(g, RectF(x, y, w, h), 12, C(darkMode, 255, 255, 255, 33, 40, 37));
	FillRound(g, RectF(x + 42, y + 42, 84, 84), 12, C(darkMode, 0, 107, 95, 130, 213, 199));
	Text(g, L"J", x + 42, y + 56, 84, 62, 40, FontStyleBold, C(darkMode, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
	Text(g, L"Dzjs Trainer", x + 154, y + 46, 420, 40, 27, FontStyleBold, titleColor);
	Text(g, L"Windows \u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3", x + 154, y + 89, 420, 24, 12, FontStyleRegular, C(darkMode, 84, 93, 88, 171, 183, 177));
	Text(g, L"\u7531\u8001 JiYu Trainer \u5347\u7ea7\u800c\u6765\u3002\u4f5c\u8005\uff1a\u4e91\u6563\u7686\u661f\u6cb3 & \u5feb\u4e50\u7684\u68a6\u9c7c\uff08\u539f\u4f5c\u8005\uff09", x + 154, y + 116, 520, 22, 11, FontStyleRegular, bodyColor);
	FillRound(g, RectF(x + 42, y + 158, 142, 30), 15, C(darkMode, 219, 238, 232, 43, 76, 69));
	Text(g, AboutVersionLine().c_str(), x + 42, y + 166, 142, 18, 11, FontStyleBold, C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	aboutUpdateRect = { static_cast<LONG>(x + 204), static_cast<LONG>(y + 153), static_cast<LONG>(x + 354), static_cast<LONG>(y + 193) };
	// 探测期间按钮改成「检查中…」并置灰。浮层只活 4 秒，而一次探测最坏要走一遍
	// 子进程；中间这段时间如果没有持续可见的状态，看起来就是点了没反应。
	const bool probing = updateProbeInFlight.load();
	PaintButton(g, RectF(x + 204, y + 153, 150, 40),
		probing ? L"\u68c0\u67e5\u4e2d\u2026" : L"\u68c0\u67e5\u66f4\u65b0",
		74, ButtonStyle::Tonal, 20, 2, !probing);
	Text(g, L"\u9762\u5411 Windows \u8bbe\u5907\u7684\u672c\u5730\u9632\u62a4\u4e0e\u8fd0\u7ef4\u5de5\u5177\uff0c\u96c6\u4e2d\u63d0\u4f9b\u72b6\u6001\u76d1\u63a7\u3001\u8fdc\u7a0b\u884c\u4e3a\u63a7\u5236\u548c\u8bbe\u5907\u8bca\u65ad\u3002", x + 42, y + 215, w - 84, 38, 11, FontStyleRegular, bodyColor);

	const float columnGap = 14.0f;
	const float columnW = (w - 84.0f - columnGap) * 0.5f;
	const float cardsY = y + 274.0f;
	FillRound(g, RectF(x + 42, cardsY, columnW, 154), 12, surface);
	FillRound(g, RectF(x + 42 + columnW + columnGap, cardsY, columnW, 154), 12, surface);
	Text(g, L"\u6838\u5fc3\u529f\u80fd", x + 62, cardsY + 18, columnW - 40, 22, 12, FontStyleBold, titleColor);
	Text(g, L"\u2022 \u5b9e\u65f6\u663e\u793a\u5b66\u751f\u7aef\u8fd0\u884c\u72b6\u6001", x + 62, cardsY + 53, columnW - 40, 20, 11, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u8fdc\u7a0b\u64cd\u4f5c\u3001\u7a97\u53e3\u548c\u8f93\u5165\u9632\u62a4", x + 62, cardsY + 82, columnW - 40, 20, 11, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u89c6\u9891\u6d41\u7167\u7247 / \u89c6\u9891\u66ff\u6362\u6a21\u5f0f", x + 62, cardsY + 111, columnW - 40, 20, 11, FontStyleRegular, bodyColor);
	const float rightX = x + 42 + columnW + columnGap;
	Text(g, L"\u8fd0\u884c\u65b9\u5f0f", rightX + 20, cardsY + 18, columnW - 40, 22, 12, FontStyleBold, titleColor);
	Text(g, L"\u2022 \u539f\u751f Win32 \u754c\u9762\uff0c\u4f4e\u5360\u7528\u3001\u5feb\u901f\u54cd\u5e94", rightX + 20, cardsY + 53, columnW - 40, 20, 11, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 JiYuAvKernel \u63d0\u4f9b\u8fdb\u7a0b\u7279\u5f81\u626b\u63cf\u4e0e\u8bbe\u5907\u8bca\u65ad", rightX + 20, cardsY + 82, columnW - 40, 20, 11, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u9632\u62a4\u7b56\u7565\u53ef\u5728\u8fd0\u884c\u4e2d\u4fdd\u5b58\u5e76\u5e94\u7528", rightX + 20, cardsY + 111, columnW - 40, 20, 11, FontStyleRegular, bodyColor);

	FillRound(g, RectF(x + 42, y + 452, w - 84, 74), 12, C(darkMode, 255, 243, 205, 64, 53, 28));
	Text(g, L"\u4f7f\u7528\u63d0\u793a", x + 64, y + 468, 120, 22, 12, FontStyleBold, C(darkMode, 91, 66, 0, 255, 220, 141));
	Text(g, L"\u8bf7\u4ec5\u5728\u5408\u6cd5\u3001\u6388\u6743\u7684\u73af\u5883\u4e2d\u4f7f\u7528\uff0c\u64cd\u4f5c\u524d\u8bf7\u9605\u8bfb\u5e2e\u52a9\u548c\u5f53\u524d\u65e5\u5fd7\u3002", x + 64, y + 500, w - 128, 22, 11, FontStyleRegular, C(darkMode, 91, 70, 20, 232, 211, 162));

	aboutExitRect = { static_cast<LONG>(x + w - 184), static_cast<LONG>(y + h - 56), static_cast<LONG>(x + w - 42), static_cast<LONG>(y + h - 14) };
	PaintButton(g, RectF(x + w - 184, y + h - 56, 142, 42), L"\u9000\u51fa\u8f6f\u4ef6", 73, ButtonStyle::Danger, 21, 5);
}

void MainWindow::PaintToast(Graphics& g, int width, int height)
{
	const ULONGLONG now = GetTickCount64();
	if (toastText.empty() || now > toastUntil) return;
	// 淡入 130 ms、最后 260 ms 淡出，同时从上方 8px 处滑下来。
	const float fadeIn = static_cast<float>((std::min)(1.0, static_cast<double>(now - toastStart) / 130.0));
	const float fadeOut = static_cast<float>((std::min)(1.0,
		static_cast<double>(toastUntil > now ? toastUntil - now : 0) / 260.0));
	const float alpha = (std::max)(0.0f, (std::min)(fadeIn, fadeOut));
	if (alpha <= 0.0f) return;
	auto faded = [alpha](const Color& color) {
		return Color(static_cast<BYTE>(color.GetA() * alpha), color.GetR(), color.GetG(), color.GetB());
	};
	// 提示条放底部居中（MD3 的 snackbar 位置）。原来贴在顶部，
	// 正好压住高级配置页的两个标签，提示一出现就没法点标签。
	const float w = (std::min)(520.0f, static_cast<float>(width - 320));
	const float x = 220.0f + (static_cast<float>(width - 220) - w) / 2.0f;
	const float y = static_cast<float>(height) - 62.0f + (1.0f - fadeIn) * 10.0f;
	FillRound(g, RectF(x, y, w, 44), 22, faded(C(darkMode, 44, 51, 48, 214, 222, 218, 246)));
	Text(g, toastText.c_str(), x + 18, y + 12, w - 36, 22, 11, FontStyleRegular,
		faded(C(darkMode, 255, 255, 255, 36, 43, 40)), StringAlignmentCenter);
}

// 更新对话框 —— 按 MD3 规范重做：
// 容器 surfaceContainerHigh / 28 圆角 / 24 内边距 / 全窗 32% scrim /
// headlineSmall 标题 + bodyMedium 正文 / 动作区右对齐（主操作 Filled，次操作 Text）。
// 四种形态共用同一套骨架，只换正文区。
void MainWindow::PaintUpdateDialog(Graphics& g, int width, int height)
{
	if (!UpdateDialogVisible()) return;
	const bool d = darkMode;
	const float appear = (std::max)(0.0f, (std::min)(1.0f, updateDialog.appear));
	auto fade = [appear](const Color& color) {
		return Color(static_cast<BYTE>(color.GetA() * appear), color.GetR(), color.GetG(), color.GetB());
	};
	const UpdateDialogMode mode = updateDialog.mode;

	// M3 颜色角色，跟主界面同一套 teal 种子色。
	const Color onSurface = C(d, 26, 30, 28, 232, 238, 235);
	const Color onSurfaceVariant = C(d, 96, 104, 100, 168, 179, 174);
	const Color outline = C(d, 130, 139, 134, 138, 149, 144);
	const Color primary = C(d, 0, 107, 95, 130, 213, 199);
	const Color container = C(d, 243, 247, 245, 38, 45, 42);

	// MD3 scrim：整窗一层，不再分区压暗。
	{
		SolidBrush veil(Color(static_cast<BYTE>((d ? 115.0f : 82.0f) * appear), 0, 0, 0));
		g.FillRectangle(&veil, 0, 0, width, height);
	}

	const bool hasActions = mode == UpdateDialogMode::Confirm || mode == UpdateDialogMode::Failed;
	float bodyH = 66.0f;
	if (mode == UpdateDialogMode::Confirm) bodyH = 18.0f + 60.0f + 12.0f + 16.0f;
	else if (mode == UpdateDialogMode::Download) bodyH = 36.0f + 20.0f + 16.0f + 22.0f + 16.0f;
	else if (mode == UpdateDialogMode::Done) bodyH = 36.0f + 20.0f + 16.0f;

	const float cardH = 24.0f + 26.0f + 4.0f + 16.0f + 16.0f + bodyH +
		(hasActions ? 20.0f + 40.0f : 0.0f) + 24.0f;
	float cardW = (std::min)(560.0f, static_cast<float>(width) - 96.0f);
	cardW = (std::max)(cardW, 360.0f);
	const float cardX = (static_cast<float>(width) - cardW) / 2.0f;
	const float cardY = (static_cast<float>(height) - cardH) / 2.0f + (1.0f - appear) * 10.0f;

	// elevation level 3：靠多层低透明度外扩模拟。
	SoftShadow(g, RectF(cardX, cardY, cardW, cardH), 28.0f, d, 1.6f * appear);
	FillRound(g, RectF(cardX, cardY, cardW, cardH), 28.0f, fade(container));

	// 头部：24px 图标 + 标题 + 副标题。
	const int icon = mode == UpdateDialogMode::Failed ? 12 : (mode == UpdateDialogMode::Done ? 11 : 10);
	DrawIcon(g, icon, cardX + 24.0f, cardY + 26.0f, fade(primary), 24);
	LPCWSTR title = L"发现新版本";
	if (mode == UpdateDialogMode::Download) title = L"正在下载更新";
	else if (mode == UpdateDialogMode::Done) title = L"更新完成";
	else if (mode == UpdateDialogMode::Failed) title = L"更新失败";
	const std::wstring current = updateDialog.currentVersion.empty() ? L"未知" : updateDialog.currentVersion;
	std::wstring subtitle;
	if (mode == UpdateDialogMode::Confirm || mode == UpdateDialogMode::Download) {
		subtitle = current + L"  →  " + updateDialog.version;
	}
	else if (mode == UpdateDialogMode::Done) {
		subtitle = L"已切换到 " + updateDialog.version + L"，正在重启";
	}
	else {
		subtitle = L"下载或替换没有完成";
	}
	Text(g, title, cardX + 58.0f, cardY + 24.0f, cardW - 82.0f, 26.0f, 20, FontStyleBold, fade(onSurface));
	Text(g, subtitle.c_str(), cardX + 24.0f, cardY + 54.0f, cardW - 48.0f, 18.0f, 12, FontStyleRegular,
		fade(onSurfaceVariant));

	const float contentX = cardX + 24.0f;
	const float contentW = cardW - 48.0f;
	const float contentTop = cardY + 86.0f;

	if (mode == UpdateDialogMode::Confirm) {
		Text(g, L"更新说明", contentX, contentTop, 200.0f, 14.0f, 11, FontStyleBold, fade(onSurfaceVariant));
		// 说明可能很长，只留三行位置；多的裁掉，别顶到动作区。
		Region clip(Rect(static_cast<INT>(contentX), static_cast<INT>(contentTop + 18.0f),
			static_cast<INT>(contentW), 60));
		g.SetClip(&clip, CombineModeIntersect);
		const wchar_t* notes = updateDialog.notes.empty() ? L"这次更新没有附带说明。" : updateDialog.notes.c_str();
		Text(g, notes, contentX, contentTop + 18.0f, contentW, 60.0f, 12, FontStyleRegular,
			fade(onSurfaceVariant));
		g.ResetClip();
		Text(g, L"下载完成后会自动替换并重启程序", contentX, contentTop + 90.0f, contentW, 16.0f, 11,
			FontStyleRegular, fade(outline));
	}
	else if (mode == UpdateDialogMode::Download || mode == UpdateDialogMode::Done) {
		const bool indeterminate = mode == UpdateDialogMode::Download && updateDialog.percent < 0;
		const float ratio = (std::max)(0.0f, (std::min)(1.0f, updateDialog.progress));
		if (mode == UpdateDialogMode::Done || updateDialog.percent >= 0) {
			wchar_t percentText[16] = {};
			swprintf_s(percentText, L"%d%%", mode == UpdateDialogMode::Done ? 100 : updateDialog.percent);
			Text(g, percentText, contentX, contentTop - 2.0f, 160.0f, 34.0f, 24, FontStyleBold, fade(onSurface));
			if (updateDialog.total > 0) {
				const unsigned long long shown = mode == UpdateDialogMode::Done
					? updateDialog.total : updateDialog.done;
				const std::wstring bytes = FormatByteSize(shown) + L" / " + FormatByteSize(updateDialog.total);
				Text(g, bytes.c_str(), cardX + cardW - 24.0f - 220.0f, contentTop + 10.0f, 220.0f, 16.0f, 11,
					FontStyleRegular, fade(onSurfaceVariant), StringAlignmentFar);
			}
		}
		else {
			Text(g, L"准备中…", contentX, contentTop, 160.0f, 30.0f, 18, FontStyleBold, fade(outline));
			Text(g, L"总大小未知", cardX + cardW - 24.0f - 220.0f, contentTop + 10.0f, 220.0f, 16.0f, 11,
				FontStyleRegular, fade(onSurfaceVariant), StringAlignmentFar);
		}

		// MD3 linear progress indicator：track 与 indicator 同高，两端全圆角。
		const float barY = contentTop + 36.0f;
		const float barH = 6.0f;
		FillRound(g, RectF(contentX, barY, contentW, barH), 3.0f, fade(C(d, 226, 232, 229, 58, 68, 63)));
		if (indeterminate) {
			// 总长度未知时一段高光来回走，段宽 34%。
			const float piece = contentW * 0.34f;
			const float origin = contentX + (contentW - piece) * updateDialog.sweep;
			FillRound(g, RectF(origin, barY, piece, barH), 3.0f, fade(primary));
		}
		else if (ratio > 0.0f) {
			const float piece = (std::max)(barH, contentW * ratio);
			FillRound(g, RectF(contentX, barY, piece, barH), 3.0f, fade(primary));
		}

		if (mode == UpdateDialogMode::Done) {
			Text(g, L"安装包已校验，正在切换到新版本", contentX, barY + 20.0f, contentW, 16.0f, 12,
				FontStyleRegular, fade(onSurfaceVariant));
		}
		else {
			Text(g, updateDialog.status.c_str(), contentX, barY + 20.0f, contentW, 16.0f, 12,
				FontStyleRegular, fade(onSurfaceVariant));
			Text(g, L"期间请不要关闭程序", contentX, barY + 44.0f, contentW, 16.0f, 11,
				FontStyleRegular, fade(outline));
		}
	}
	else {
		Region clip(Rect(static_cast<INT>(contentX), static_cast<INT>(contentTop),
			static_cast<INT>(contentW), 66));
		g.SetClip(&clip, CombineModeIntersect);
		Text(g, updateDialog.status.c_str(), contentX, contentTop, contentW, 66.0f, 12, FontStyleRegular,
			fade(onSurfaceVariant));
		g.ResetClip();
	}

	// 动作区：右对齐，主操作在最右。120/121 两种 id 已在 hoverProgress 里留位。
	updateDialogPrimaryRect = {};
	updateDialogSecondaryRect = {};
	const float buttonH = 40.0f;
	const float buttonY = cardY + cardH - 24.0f - buttonH;
	auto setRect = [&](int id, const RectF& rect) {
		const RECT hit = { static_cast<LONG>(rect.X), static_cast<LONG>(rect.Y),
			static_cast<LONG>(rect.X + rect.Width), static_cast<LONG>(rect.Y + rect.Height) };
		if (id == kUpdateDialogPrimary) updateDialogPrimaryRect = hit;
		else updateDialogSecondaryRect = hit;
	};
	// 次操作按 MD3 text button：无底色，悬停 8%、按下 12% 主色蒙层。
	auto placeTextButton = [&](float right, float w, LPCWSTR label, int id) {
		const RectF rect(right - w, buttonY, w, buttonH);
		const float press = pressTarget == id ? 1.0f : 0.0f;
		const float overlay = press > 0.0f ? 0.12f : 0.08f * hoverProgress[id];
		if (overlay > 0.003f) {
			FillRound(g, rect, 20.0f, Color(static_cast<BYTE>(255.0f * overlay * appear),
				primary.GetR(), primary.GetG(), primary.GetB()));
		}
		Text(g, label, rect.X, rect.Y + (buttonH - 20.0f) / 2.0f, rect.Width, 20.0f, 12, FontStyleBold,
			fade(primary), StringAlignmentCenter);
		setRect(id, rect);
		return right - w - 8.0f;
	};
	auto placeFilledButton = [&](float right, float w, LPCWSTR label, int id) {
		const RectF rect(right - w, buttonY, w, buttonH);
		PaintButton(g, rect, label, id, ButtonStyle::Primary, 20.0f);
		setRect(id, rect);
		return right - w - 8.0f;
	};

	if (mode == UpdateDialogMode::Confirm) {
		const float right = placeFilledButton(cardX + cardW - 24.0f, 108.0f, L"立即更新", kUpdateDialogPrimary);
		placeTextButton(right, 76.0f, L"稍后", kUpdateDialogSecondary);
	}
	else if (mode == UpdateDialogMode::Failed) {
		const float right = placeFilledButton(cardX + cardW - 24.0f, 148.0f, L"改用独立更新器", kUpdateDialogPrimary);
		placeTextButton(right, 76.0f, L"关闭", kUpdateDialogSecondary);
	}

	if (focusTarget == kUpdateDialogPrimary || focusTarget == kUpdateDialogSecondary) {
		const RECT& ringSource = focusTarget == kUpdateDialogPrimary
			? updateDialogPrimaryRect : updateDialogSecondaryRect;
		if (!IsRectEmpty(&ringSource)) {
			StrokeRound(g, RectF(static_cast<REAL>(ringSource.left) - 3.0f, static_cast<REAL>(ringSource.top) - 3.0f,
				static_cast<REAL>(ringSource.right - ringSource.left) + 6.0f,
				static_cast<REAL>(ringSource.bottom - ringSource.top) + 6.0f), 23.0f,
				fade(primary), 2.0f);
		}
	}
}

DWORD WINAPI MainWindow::AvScanThreadProc(LPVOID parameter)
{
	MainWindow* self = static_cast<MainWindow*>(parameter);
	AV_PROCESS_SCAN_SUMMARY summary = {};
	const BOOL succeeded = AvIntegratedScanAllProcessMemory(self->avScanCancelEvent, &summary);
	{
		std::lock_guard<std::mutex> guard(self->stateMutex);
		self->avScanSummary = summary;
		if (!succeeded) {
			self->avScanStatus = L"\u626b\u63cf\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5 AV \u9a71\u52a8\u8fde\u63a5\u548c\u7cfb\u7edf\u6743\u9650";
		}
		else if (summary.cancelled) {
			self->avScanStatus = L"\u626b\u63cf\u5df2\u505c\u6b62";
		}
		else if (summary.detectionCount != 0) {
			self->avScanStatus = L"\u626b\u63cf\u5b8c\u6210\uff0c\u53d1\u73b0\u7279\u5f81\u547d\u4e2d";
		}
		else {
			self->avScanStatus = L"\u626b\u63cf\u5b8c\u6210\uff0c\u672a\u53d1\u73b0\u5df2\u5f55\u5165\u7279\u5f81";
		}
	}
	InterlockedExchange(&self->avScanRunning, 0);
	HWND window = self->_hWnd;
	if (window) PostMessageW(window, WM_AV_SCAN_FINISHED, succeeded, 0);
	return succeeded ? 0UL : 1UL;
}

void MainWindow::StartAvProcessScan()
{
	if (InterlockedCompareExchange(&avScanRunning, 0, 0) != 0) {
		ShowFastTip(L"\u5168\u8fdb\u7a0b\u626b\u63cf\u6b63\u5728\u8fd0\u884c");
		return;
	}
	StopAvProcessScan();
	if (!AvIntegratedEnsureLoaded(JTAppGetAvDriverPathDirect())) {
		ShowFastTip(L"JiYuAvKernel \u52a0\u8f7d\u5931\u8d25");
		return;
	}
	avScanCancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!avScanCancelEvent) {
		ShowFastTip(L"\u65e0\u6cd5\u521b\u5efa\u626b\u63cf\u4efb\u52a1");
		return;
	}
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		ZeroMemory(&avScanSummary, sizeof(avScanSummary));
		avScanStatus = L"\u6b63\u5728\u679a\u4e3e\u5e76\u626b\u63cf\u8fdb\u7a0b\u6620\u50cf...";
	}
	InterlockedExchange(&avScanRunning, 1);
	avScanThread = CreateThread(nullptr, 0, AvScanThreadProc, this, 0, nullptr);
	if (!avScanThread) {
		InterlockedExchange(&avScanRunning, 0);
		CloseHandle(avScanCancelEvent);
		avScanCancelEvent = nullptr;
		ShowFastTip(L"\u65e0\u6cd5\u542f\u52a8\u626b\u63cf\u7ebf\u7a0b");
		return;
	}
	if (currentLogger) currentLogger->LogInfo(L"AV process memory scan started");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::StopAvProcessScan()
{
	if (avScanCancelEvent) SetEvent(avScanCancelEvent);
	if (avScanThread) {
		WaitForSingleObject(avScanThread, INFINITE);
		CloseHandle(avScanThread);
		avScanThread = nullptr;
	}
	if (avScanCancelEvent) {
		CloseHandle(avScanCancelEvent);
		avScanCancelEvent = nullptr;
	}
	InterlockedExchange(&avScanRunning, 0);
}

void MainWindow::FinishAvProcessScan()
{
	StopAvProcessScan();
	AV_PROCESS_SCAN_SUMMARY summary = {};
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		summary = avScanSummary;
	}
	if (currentLogger) {
		currentLogger->LogInfo(
			L"AV process memory scan finished: processes=%lu scanned=%lu detections=%lu failed=%lu cancelled=%d",
			static_cast<unsigned long>(summary.processCount),
			static_cast<unsigned long>(summary.scannedCount),
			static_cast<unsigned long>(summary.detectionCount),
			static_cast<unsigned long>(summary.failedCount),
			summary.cancelled);
		for (DWORD index = 0; index < summary.findingCount; ++index)
			currentLogger->LogWarn(L"AV detection: %s", summary.findings[index]);
	}
	ShowFastTip(summary.detectionCount != 0 ? L"\u626b\u63cf\u5b8c\u6210\uff0c\u53d1\u73b0\u7279\u5f81\u547d\u4e2d" : L"\u626b\u63cf\u5b8c\u6210");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::ImportAvExeSignature()
{
	DWORD selectedProcessId = 0;
	std::wstring selectedProcessName;
	if (!ChooseAvProcess(_hWnd, &selectedProcessId, &selectedProcessName)) return;
	if (!AvIntegratedEnsureLoaded(JTAppGetAvDriverPathDirect())) { ShowFastTip(L"JiYuAvKernel 加载失败"); return; }
	WCHAR pattern[256] = {};
	ULONG signatureId = 0;
	DWORD exactBytes = 0;
	if (!AvIntegratedAddProcessPatternSignature(selectedProcessId, pattern, _countof(pattern), &signatureId, &exactBytes)) {
		WCHAR message[160] = {};
		swprintf_s(message, L"运行进程采样失败，错误码 %lu", static_cast<unsigned long>(GetLastError()));
		ShowFastTip(message);
		return;
	}
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		WCHAR source[160] = {};
		swprintf_s(source, L"%s  |  PID %lu（主模块内存）", selectedProcessName.c_str(), static_cast<unsigned long>(selectedProcessId));
		avSignaturePath = source;
		avSignaturePattern = pattern;
		avSignatureId = signatureId;
		avSignatureSampleCount = 1;
		avSignatureExactBytes = exactBytes;
		avScanStatus = L"已从运行进程主模块内存录入特征，已注册到 JiYuAvKernel";
	}
	ShowFastTip(L"运行进程内存特征已录入");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::InputAvSignature()
{
	std::wstring text;
	if (!PromptAvText(_hWnd, L"输入特征码", L"输入特征码（空格分隔，支持 ??、?A、A?）：", &text) || text.empty()) return;
	if (!AvIntegratedEnsureLoaded(JTAppGetAvDriverPathDirect())) { ShowFastTip(L"JiYuAvKernel 加载失败"); return; }
	ULONG signatureId = 0;
	DWORD registeredCount = 0;
	DWORD exactBytes = 0;
	if (!AvIntegratedAddPatternSignatureTextBatch(text.c_str(), L"手工输入特征", &signatureId, &registeredCount, &exactBytes)) { ShowFastTip(L"特征码格式无效或注册失败"); return; }
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		WCHAR source[96] = {};
		swprintf_s(source, L"手工输入特征码（%lu 条）", static_cast<unsigned long>(registeredCount));
		avSignaturePath = source;
		const size_t firstLineEnd = text.find_first_of(L"\r\n");
		avSignaturePattern = text.substr(0, firstLineEnd == std::wstring::npos ? text.size() : firstLineEnd);
		avSignatureId = signatureId;
		avSignatureSampleCount = registeredCount;
		avSignatureExactBytes = exactBytes;
		avScanStatus = registeredCount > 1 ? L"多条手工特征已一次注册到 JiYuAvKernel" : L"手工特征已注册到 JiYuAvKernel";
	}
	ShowFastTip(registeredCount > 1 ? L"多条特征码已录入" : L"手工特征码已录入");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

/* Legacy file-sample implementation retained below for compatibility. */
/*
	if (InterlockedCompareExchange(&avScanRunning, 0, 0) != 0) {
		ShowFastTip(L"\u8bf7\u5148\u505c\u6b62\u5168\u8fdb\u7a0b\u626b\u63cf");
		return;
	}
	std::vector<std::wstring> paths;
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		avScanStatus = L"正在打开样本选择器...";
	}
	InvalidateRect(_hWnd, nullptr, FALSE);
	UpdateWindow(_hWnd);
	if (currentLogger) currentLogger->LogInfo(L"AV sample import button clicked; opening executable picker");

	DWORD dialogFailure = ERROR_SUCCESS;
	const SampleDialogResult dialogResult = ChooseExecutableSamplesOnStaThread(
		_hWnd, &paths, &dialogFailure);
	if (dialogResult == SampleDialogResult::Cancelled) {
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = L"已取消录入版本样本";
		}
		ShowFastTip(L"已取消录入版本样本");
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	if (dialogResult == SampleDialogResult::Failed) {
		WCHAR message[160] = {};
		swprintf_s(message, L"无法启动自建样本选择器，错误码 0x%08lX",
			static_cast<unsigned long>(dialogFailure));
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = message;
		}
		if (currentLogger) currentLogger->LogError(
			L"AV custom executable picker failed: 0x%08lX",
			static_cast<unsigned long>(dialogFailure));
		MessageBoxW(_hWnd, message, L"录入版本样本", MB_OK | MB_ICONERROR);
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		WCHAR status[96] = {};
		swprintf_s(status, L"正在分析 %lu 个 EXE 样本...", static_cast<unsigned long>(paths.size()));
		avScanStatus = status;
	}
	InvalidateRect(_hWnd, nullptr, FALSE);
	UpdateWindow(_hWnd);

	if (!AvIntegratedEnsureLoaded(JTAppGetAvDriverPathDirect())) {
		const DWORD error = GetLastError();
		WCHAR message[160] = {};
		swprintf_s(message, L"JiYuAvKernel 加载失败，错误码 %lu", static_cast<unsigned long>(error));
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = message;
		}
		if (currentLogger) currentLogger->LogError(L"AV sample import could not connect driver: %lu", static_cast<unsigned long>(error));
		ShowFastTip(message);
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	std::vector<LPCWSTR> pathPointers;
	for (const std::wstring& path : paths) pathPointers.push_back(path.c_str());
	WCHAR pattern[256] = {};
	ULONG signatureId = 0;
	DWORD exactBytes = 0;
	BOOL packedSample = FALSE;
	if (!AvIntegratedAddExePatternSignature(
			pathPointers.data(), static_cast<DWORD>(pathPointers.size()),
			pattern, _countof(pattern), &signatureId, &exactBytes, &packedSample)) {
		const DWORD error = GetLastError();
		WCHAR message[192] = {};
		swprintf_s(message, L"样本分析失败或没有稳定代码特征，错误码 %lu", static_cast<unsigned long>(error));
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = message;
		}
		if (currentLogger) currentLogger->LogError(L"AV sample pattern generation failed: %lu", static_cast<unsigned long>(error));
		ShowFastTip(message);
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		avSignaturePath = paths.front();
		if (paths.size() > 1) {
			WCHAR suffix[64] = {};
			swprintf_s(suffix, L"  (+%lu \u4e2a\u7248\u672c)", static_cast<unsigned long>(paths.size() - 1));
			avSignaturePath += suffix;
		}
		avSignaturePattern = pattern;
		avSignatureId = signatureId;
		avSignatureSampleCount = static_cast<DWORD>(paths.size());
		avSignatureExactBytes = exactBytes;
		avScanStatus = packedSample
			? L"\u5df2\u5f55\u5165 UPX \u58f3\u5165\u53e3\u7279\u5f81\uff1b\u4ec5\u9002\u7528\u540c\u58f3\u7248\u672c\uff0c\u4e0d\u4ee3\u8868\u89e3\u5305\u540e\u7a0b\u5e8f\u4e3b\u4f53"
			: paths.size() > 1
				? L"\u8de8\u7248\u672c\u5185\u5b58\u7279\u5f81\u5df2\u5f55\u5165 JiYuAvKernel"
				: L"\u5355\u6837\u672c\u5185\u5b58\u7279\u5f81\u5df2\u5f55\u5165\uff0c\u5efa\u8bae\u8865\u5145\u5176\u4ed6\u7248\u672c";
	}
	if (currentLogger) {
		currentLogger->LogInfo(
			L"AV memory pattern registered: id=%lu samples=%lu exactBytes=%lu pattern=%s",
			static_cast<unsigned long>(signatureId),
			static_cast<unsigned long>(paths.size()),
			static_cast<unsigned long>(exactBytes),
			pattern);
	}
	ShowFastTip(packedSample
		? L"\u68c0\u6d4b\u5230 UPX \u58f3\uff0c\u5df2\u5f55\u5165\u58f3\u5165\u53e3\u7279\u5f81"
		: paths.size() > 1 ? L"\u8de8\u7248\u672c\u5185\u5b58\u7279\u5f81\u5df2\u5f55\u5165" : L"\u5355\u6837\u672c\u7279\u5f81\u5df2\u5f55\u5165");
	InvalidateRect(_hWnd, nullptr, FALSE);
}
*/

void MainWindow::CopyAvSignature()
{
	std::wstring pattern;
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		pattern = avSignaturePattern;
	}
	if (pattern.empty()) {
		ShowFastTip(L"\u5c1a\u65e0\u53ef\u590d\u5236\u7684\u7279\u5f81\u7801");
		return;
	}
	if (!OpenClipboard(_hWnd)) {
		ShowFastTip(L"\u526a\u8d34\u677f\u6253\u5f00\u5931\u8d25");
		return;
	}
	EmptyClipboard();
	const SIZE_T bytes = (pattern.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (memory) {
		void* destination = GlobalLock(memory);
		if (destination) {
			memcpy(destination, pattern.c_str(), bytes);
			GlobalUnlock(memory);
			if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
		}
		else GlobalFree(memory);
	}
	CloseClipboard();
	ShowFastTip(L"\u5185\u5b58\u7279\u5f81\u7801\u5df2\u590d\u5236");
}

void MainWindow::UnloadAvDriver()
{
	StopAvProcessScan();
	if (!AvIntegratedUnload()) {
		const DWORD error = GetLastError();
		WCHAR message[128] = {};
		swprintf_s(message, L"JiYuAvKernel \u5378\u8f7d\u5931\u8d25\uff0c\u9519\u8bef\u7801 %lu",
			static_cast<unsigned long>(error));
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = message;
		}
		if (currentLogger) currentLogger->LogError(
			L"AV kernel unload failed: %lu", static_cast<unsigned long>(error));
		ShowFastTip(message);
	}
	else {
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			avScanStatus = L"JiYuAvKernel \u5df2\u5378\u8f7d\uff0c\u4e0b\u6b21\u626b\u63cf\u65f6\u4f1a\u81ea\u52a8\u52a0\u8f7d";
		}
		if (currentLogger) currentLogger->LogInfo(L"AV kernel unloaded from scan page");
		ShowFastTip(L"JiYuAvKernel \u5df2\u5378\u8f7d");
	}
	InvalidateRect(_hWnd, nullptr, FALSE);
}

int MainWindow::HitTargetAt(POINT point)
{
	int target = -1;
	for (int i = 0; i < 10; ++i) if (Hit(navRects[i], point)) target = i;
	if (page == Page::Overview) for (int i = 0; i < 5; ++i) if (Hit(actionRects[i], point)) target = 10 + i;
	if (Hit(superTopMostRect, point)) target = 80;
	if (page == Page::Protection || page == Page::Replacement) {
		for (int i = 0; i < 7; ++i) if (Hit(toggleRects[i], point)) target = 20 + i;
		if (Hit(temporaryVideoEnableRect, point)) target = 74;
		if (Hit(temporaryVideoImageModeRect, point)) target = 75;
		if (Hit(temporaryVideoFileModeRect, point)) target = 76;
		if (Hit(temporaryVideoImagePickerRect, point)) target = 77;
		if (Hit(temporaryVideoFilePickerRect, point)) target = 78;
		if (Hit(temporaryVideoLoopRect, point)) target = 79;
		if (Hit(restoreTeacherViewRect, point)) target = 81;
	}
	if (page == Page::Logs && Hit(exportLogRect, point)) target = 40;
	if (Hit(themeRect, point)) target = 41;
	if ((page == Page::Protection || page == Page::Replacement) && Hit(advancedSettingsRect, point)) target = 42;
	if (page == Page::Protection && Hit(driverLoadRect, point)) target = 43;
	if (page == Page::Protection && Hit(driverUnloadRect, point)) target = 44;
	if (page == Page::Antivirus) {
		if (Hit(avScanRect, point)) target = 45;
		if (Hit(avImportRect, point)) target = 46;
		if (Hit(avInputRect, point)) target = 50;
		if (Hit(avCopyRect, point)) target = 47;
		if (Hit(avCancelRect, point)) target = 48;
		if (Hit(avUnloadRect, point)) target = 49;
	}
	if (page == Page::Advanced) {
		for (int i = 0; i < 12; ++i) if (Hit(advancedToggleRects[i], point)) target = 50 + i;
		for (int i = 0; i < 3; ++i) if (Hit(advancedKillRects[i], point)) target = 64 + i;
		for (int i = 0; i < 2; ++i) if (Hit(advancedInjectRects[i], point)) target = 67 + i;
		if (Hit(advancedTabRects[0], point)) target = 70;
		if (Hit(advancedTabRects[1], point)) target = 71;
		if (Hit(advancedSaveRect, point)) target = 69;
	}
	if (page == Page::Network && Hit(networkProjectRect, point)) target = 72;
	if (page == Page::About) {
		if (Hit(aboutUpdateRect, point)) target = 74;
		if (Hit(aboutExitRect, point)) target = 73;
	}
	if (page == Page::Diagnostics) {
		if (Hit(diagnosticsScanRect, point)) target = 90;
		if (Hit(diagnosticsUnloadRect, point)) target = 91;
		if (Hit(diagnosticsHideOwnRect, point)) target = 92;
		for (int i = 0; i < 4; ++i) if (Hit(diagnosticsFilterRects[i], point)) target = 93 + i;
		for (int i = 0; i < 8; ++i) if (Hit(diagnosticsDriverRects[i], point)) target = 100 + i;
	}
	if ((page == Page::Protection || page == Page::Replacement) && Hit(temporaryVideoPreviewRect, point)) target = 85;
	return target;
}

bool MainWindow::TargetRect(int target, RECT* rect) const
{
	RECT found{};
	switch (target) {
	case 80: found = superTopMostRect; break;
	// 更新弹窗的两个按钮。Tab/Enter 走的是同一套命中判断，加进来就自动可用。
	case kUpdateDialogPrimary: found = updateDialogPrimaryRect; break;
	case kUpdateDialogSecondary: found = updateDialogSecondaryRect; break;
	case 0: case 1: case 2: case 3: case 4:
	case 5: case 6: case 7: case 8: case 9:
		found = navRects[target];
		break;
	case 10: case 11: case 12: case 13: case 14:
		found = actionRects[target - 10];
		break;
	case 20: case 21: case 22: case 23: case 24: case 25: case 26:
		found = toggleRects[target - 20];
		break;
	case 40: found = exportLogRect; break;
	case 41: found = themeRect; break;
	case 42: found = advancedSettingsRect; break;
	case 43: found = driverLoadRect; break;
	case 44: found = driverUnloadRect; break;
	case 45: found = avScanRect; break;
	case 46: found = avImportRect; break;
	case 47: found = avCopyRect; break;
	case 48: found = avCancelRect; break;
	case 49: found = avUnloadRect; break;
	case 50:
		// 50 在特征扫描页是"输入特征码"，在高级配置页是第一个开关。
		found = page == Page::Advanced ? advancedToggleRects[0] : avInputRect;
		break;
	case 51: case 52: case 53: case 54: case 55: case 56:
	case 57: case 58: case 59: case 60: case 61:
		found = advancedToggleRects[target - 50];
		break;
	case 64: case 65: case 66: found = advancedKillRects[target - 64]; break;
	case 67: case 68: found = advancedInjectRects[target - 67]; break;
	case 69: found = advancedSaveRect; break;
	case 70: case 71: found = advancedTabRects[target - 70]; break;
	case 72: found = networkProjectRect; break;
	case 73: found = aboutExitRect; break;
	case 74: found = aboutUpdateRect; break;
	case 75: found = temporaryVideoEnableRect; break;
	case 76: found = temporaryVideoImageModeRect; break;
	case 77: found = temporaryVideoFileModeRect; break;
	case 78: found = temporaryVideoImagePickerRect; break;
	case 79: found = temporaryVideoFilePickerRect; break;
	case 81: found = restoreTeacherViewRect; break;
	case 85: found = temporaryVideoPreviewRect; break;
	case 90: found = diagnosticsScanRect; break;
	case 91: found = diagnosticsUnloadRect; break;
	case 92: found = diagnosticsHideOwnRect; break;
	case 93: case 94: case 95: case 96: found = diagnosticsFilterRects[target - 93]; break;
	case 100: case 101: case 102: case 103:
	case 104: case 105: case 106: case 107:
		found = diagnosticsDriverRects[target - 100];
		break;
	default: return false;
	}
	if (IsRectEmpty(&found)) return false;
	if (rect) *rect = found;
	return true;
}

std::vector<int> MainWindow::FocusableTargets() const
{
	std::vector<int> list;
	// 弹窗是模态的：Tab 只在它自己的按钮之间转。
	if (UpdateDialogVisible()) {
		if (TargetRect(kUpdateDialogPrimary, nullptr)) list.push_back(kUpdateDialogPrimary);
		if (TargetRect(kUpdateDialogSecondary, nullptr)) list.push_back(kUpdateDialogSecondary);
		return list;
	}
	for (int i = 0; i < 10; ++i) list.push_back(i);
	list.push_back(80);   // 超级置顶
	list.push_back(41);   // 明暗主题
	auto push = [this, &list](int id) {
		if (TargetRect(id, nullptr)) list.push_back(id);
	};
	switch (page) {
	case Page::Overview:
		for (int i = 0; i < 5; ++i) push(10 + i);
		break;
	case Page::Protection:
	case Page::Replacement:
		for (int i = 0; i < 7; ++i) push(20 + i);
		push(42);
		push(44);
		push(75);
		push(76);
		push(77);
		push(78);
		push(79);
		push(81);
		push(85);
		break;
	case Page::Antivirus:
		push(45); push(46); push(50); push(47); push(49); push(48);
		break;
	case Page::Advanced:
		push(70); push(71);
		if (advancedTab == 0) {
			for (int i = 0; i < 6; ++i) push(50 + i);
		}
		else {
			for (int i = 0; i < 3; ++i) push(64 + i);
			for (int i = 0; i < 2; ++i) push(67 + i);
			for (int i = 0; i < 6; ++i) push(50 + i);
		}
		push(69);
		break;
	case Page::Network:
		push(72);
		break;
	case Page::Diagnostics:
		push(92); push(90);
		for (int i = 0; i < 4; ++i) push(93 + i);
		push(91);
		for (int i = 0; i < 8; ++i) push(100 + i);
		break;
	case Page::Logs:
		push(40);
		break;
	case Page::About:
		push(74); push(73);
		break;
	default:
		break;
	}
	return list;
}

void MainWindow::MoveFocus(bool backwards)
{
	const std::vector<int> list = FocusableTargets();
	if (list.empty()) return;
	int index = -1;
	for (size_t i = 0; i < list.size(); ++i) if (list[i] == focusTarget) { index = static_cast<int>(i); break; }
	if (index < 0) index = backwards ? 0 : -1;
	const int count = static_cast<int>(list.size());
	index = backwards ? (index - 1 + count) % count : (index + 1) % count;
	focusTarget = list[static_cast<size_t>(index)];
	SetFocus(_hWnd);
	InvalidateRect(_hWnd, nullptr, FALSE);
}

bool MainWindow::ActivateFocus()
{
	if (focusTarget < 0) return false;
	RECT rect{};
	if (!TargetRect(focusTarget, &rect)) return false;
	// 直接复用鼠标点击那条路径：命中判断本来就是按坐标做的。
	POINT point{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
	HandleClick(point);
	return true;
}

float MainWindow::AnimateToggle(int index, bool on)
{
	if (index < 0 || index >= static_cast<int>(toggleMotion.size())) return on ? 1.0f : 0.0f;
	float& value = toggleMotion[static_cast<size_t>(index)];
	const float target = on ? 1.0f : 0.0f;
	if (std::fabs(value - target) > 0.004f) {
		// 时间常数 70ms：约 150ms 走完，和网页里的开关一个手感。
		const float k = static_cast<float>(1.0 - std::exp(-frameDelta / 0.07));
		value += (target - value) * k;
		if (std::fabs(value - target) <= 0.004f) value = target;
		toggleAnimating = true;
		EnsureAnimationTimer();
	}
	else {
		value = target;
	}
	return value;
}

void MainWindow::UpdateHover(POINT point)
{
	// 弹窗期间只认弹窗里的两个按钮，底下的界面不该再跟着鼠标亮。
	if (UpdateDialogVisible()) {
		const int dialogTarget = HitUpdateDialogAt(point);
		SetCursor(LoadCursorW(nullptr, dialogTarget >= 0 ? IDC_HAND : IDC_ARROW));
		if (dialogTarget != hoverTarget) {
			hoverTarget = dialogTarget;
			EnsureAnimationTimer();
			InvalidateRect(_hWnd, nullptr, FALSE);
		}
		return;
	}
	const int target = HitTargetAt(point);
	// 输入类区域给 I 形光标，其余可点区域给手形 —— 和浏览器里的习惯一致。
	const bool textTarget = page == Page::Advanced && advancedTab == 0 &&
		(Hit(advancedHotkeyRect[0], point) || Hit(advancedHotkeyRect[1], point));
	SetCursor(LoadCursorW(nullptr, textTarget ? IDC_IBEAM : (target >= 0 ? IDC_HAND : IDC_ARROW)));
	if (target != hoverTarget) {
		hoverTarget = target;
		EnsureAnimationTimer();
		InvalidateRect(_hWnd, nullptr, FALSE);
	}
}

void MainWindow::HandleClick(POINT point)
{
	// 模态弹窗吃掉所有点击，连空白处也不往下传。
	if (UpdateDialogVisible()) {
		HandleUpdateDialogClick(point);
		return;
	}
	// 点到别处就结束快捷键录制，避免键盘一直被这个框吃掉。
	if (hotkeyCaptureIndex >= 0) {
		hotkeyCaptureIndex = -1;
		InvalidateRect(_hWnd, nullptr, FALSE);
	}
	if (Hit(superTopMostRect, point)) {
		RequestSuperTopmost();
		return;
	}
	if (Hit(themeRect, point)) {
		darkMode = !darkMode;
		if (controlSurfaceBrush) DeleteObject(controlSurfaceBrush);
		controlSurfaceBrush = CreateSolidBrush(darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
		if (advancedFieldBrush) DeleteObject(advancedFieldBrush);
		advancedFieldBrush = CreateSolidBrush(darkMode ? RGB(26, 33, 30) : RGB(255, 255, 255));
		for (HWND control : networkControls) if (control) InvalidateRect(control, nullptr, TRUE);
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	for (int i = 0; i < 10; ++i) if (Hit(navRects[i], point)) { SelectPage(static_cast<Page>(i)); return; }
	if (page == Page::About && Hit(aboutUpdateRect, point)) {
		CheckForUpdate(true);
		return;
	}
	if (page == Page::About && Hit(aboutExitRect, point)) {
		OnWmCommand(CMD_POWER_EXIT);
		return;
	}
	if (page == Page::Overview) {
		for (int i = 0; i < 5; ++i) if (Hit(actionRects[i], point)) {
			switch (i) {
			case 0: ShowFastTip(currentControlled ? L"\u5f53\u524d\u5df2\u542f\u7528\u5b8c\u6574\u9632\u62a4\u3002" : L"\u9632\u62a4\u670d\u52a1\u6b63\u5728\u76d1\u63a7\u3002"); break;
			case 1: RequestSuperTopmost(); break;
			case 2: if (currentWorker) { if (studentRunning) currentWorker->Kill(); else currentWorker->Rerun(); } break;
			case 3: SelectPage(Page::Protection); break;
			case 4: ShowPowerMenu(); break;
			}
			return;
		}
	}
	if (page == Page::Protection || page == Page::Replacement) {
		if (Hit(temporaryVideoEnableRect, point)) {
			temporaryVideoProtection = !temporaryVideoProtection;
			ApplyTemporaryVideoProtection();
			ShowFastTip(temporaryVideoProtection ? L"信息流保护：本次运行已启用" : L"信息流保护：本次运行已关闭");
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(temporaryVideoImageModeRect, point)) {
			temporaryVideoMode = L"image";
			LoadTemporaryVideoPreview();
			ApplyTemporaryVideoProtection();
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(temporaryVideoFileModeRect, point)) {
			temporaryVideoMode = L"video";
			LoadTemporaryVideoPreview();
			ApplyTemporaryVideoProtection();
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(temporaryVideoImagePickerRect, point)) {
			ChooseTemporaryVideoMedia(true);
			return;
		}
		if (Hit(temporaryVideoFilePickerRect, point)) {
			ChooseTemporaryVideoMedia(false);
			return;
		}
		if (Hit(temporaryVideoLoopRect, point)) {
			temporaryVideoLoop = !temporaryVideoLoop;
			ApplyTemporaryVideoProtection();
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(restoreTeacherViewRect, point)) {
			// 遮挡有两个来源，而且是"或"的关系：
			//   replaceFrame = !allowMonitor || m_EnableModifyVideoStream
			// 只关其中一个教师端还是黑的，所以这里一次把两个都解除。
			temporaryVideoProtection = false;
			setAllowMonitor = true;
			SaveSettings();
			ApplyTemporaryVideoProtection();
			ShowFastTip(L"已恢复：教师端可以正常看到画面");
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(driverLoadRect, point)) {
			JTAppRunOperationDirect(AppOperationForceLoadDriver);
			ShowFastTip(XDriverLoaded() ? L"\u9a71\u52a8\u5df2\u52a0\u8f7d" : L"\u9a71\u52a8\u52a0\u8f7d\u5931\u8d25");
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(driverUnloadRect, point)) {
			const bool unloaded = JTAppRunOperationDirect(AppOperationUnLoadDriver) != nullptr;
			ShowFastTip(unloaded ? L"\u9a71\u52a8\u5df2\u5378\u8f7d" : L"\u9a71\u52a8\u5378\u8f7d\u5931\u8d25");
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		for (int i = 0; i < 7; ++i) if (Hit(toggleRects[i], point)) {
			switch (i) {
			case 0: setAllowAllRunOp = !setAllowAllRunOp; break;
			case 1: setBandAllRunOp = !setBandAllRunOp; break;
			case 2: setAllowGbTop = !setAllowGbTop; break;
			case 3: setProhibitKillProcess = !setProhibitKillProcess; break;
			case 4: setAllowMonitor = !setAllowMonitor; break;
			case 5: setProhibitCloseWindow = !setProhibitCloseWindow; break;
			case 6: setAllowControl = !setAllowControl; break;
			}
			SaveSettings();
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(saveSettingsRect, point)) { SaveSettings(); ShowFastTip(L"\u8bbe\u7f6e\u5df2\u4fdd\u5b58"); }
		if (Hit(advancedSettingsRect, point)) {
			SelectPage(Page::Advanced);
			return;
		}
	}
	if (page == Page::Antivirus) {
		if (Hit(avScanRect, point)) { StartAvProcessScan(); return; }
		if (Hit(avImportRect, point)) { ImportAvExeSignature(); return; }
		if (Hit(avInputRect, point)) { InputAvSignature(); return; }
		if (Hit(avCopyRect, point)) { CopyAvSignature(); return; }
		if (Hit(avUnloadRect, point)) { UnloadAvDriver(); return; }
		if (Hit(avCancelRect, point)) {
			if (InterlockedCompareExchange(&avScanRunning, 0, 0) != 0) StopAvProcessScan();
			else ShowFastTip(L"\u5f53\u524d\u6ca1\u6709\u8fd0\u884c\u4e2d\u7684\u626b\u63cf");
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
	}
	if (page == Page::Advanced) {
		for (int i = 0; i < 2; ++i) if (Hit(advancedTabRects[i], point)) {
			advancedTab = i;
			LayoutAdvancedControls();
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (advancedTab == 0) {
			for (int i = 0; i < 2; ++i) if (Hit(advancedHotkeyRect[i], point)) {
				hotkeyCaptureIndex = i;
				SetFocus(_hWnd);
				ShowFastTip(L"\u76f4\u63a5\u6309\u4e0b\u7ec4\u5408\u952e\uff0cEsc \u6e05\u9664");
				InvalidateRect(_hWnd, nullptr, FALSE);
				return;
			}
		}
		const int toggleCount = 6;
		for (int i = 0; i < toggleCount; ++i) if (Hit(advancedToggleRects[i], point)) {
			if (advancedTab == 0) {
				switch (i) {
				case 0: advDisableDriver = !advDisableDriver; break;
				case 1: advSelfProtect = !advSelfProtect; break;
				case 2: advAutoForceKill = !advAutoForceKill; break;
				case 3: advStrictWindow = !advStrictWindow; break;
				case 4: advHideOutput = !advHideOutput; break;
				case 5: advHideTaskbar = !advHideTaskbar; break;
				}
			}
			else {
				switch (i) {
				case 0: advController = !advController; break;
				case 1: advAlwaysUpdate = !advAlwaysUpdate; break;
				case 2: advForceCurrentDir = !advForceCurrentDir; break;
				case 3: advDisableWatchdog = !advDisableWatchdog; break;
				case 4: advInjectMaster = !advInjectMaster; break;
				case 5: advInject64 = !advInject64; break;
				}
			}
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (advancedTab == 1) {
			for (int i = 0; i < 3; ++i) if (Hit(advancedKillRects[i], point)) { advancedKillMode = i; InvalidateRect(_hWnd, nullptr, FALSE); return; }
			for (int i = 0; i < 2; ++i) if (Hit(advancedInjectRects[i], point)) { advancedInjectMode = i; InvalidateRect(_hWnd, nullptr, FALSE); return; }
		}
		if (Hit(advancedSaveRect, point)) { SaveAdvancedSettings(); InvalidateRect(_hWnd, nullptr, FALSE); return; }
	}
	if (page == Page::Network && Hit(networkProjectRect, point)) {
		SysHlp::OpenUrl(L"https://github.com/yunsjxh/Third-party-JiYu-Teacher-Endpoint");
		return;
	}
	if (page == Page::Diagnostics) {
		if (Hit(diagnosticsHideOwnRect, point)) {
			diagnosticsHideOwnDrivers = !diagnosticsHideOwnDrivers;
			diagnosticsListOffset = 0;
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		if (Hit(diagnosticsUnloadRect, point)) {
			UnloadSelectedDiagnosticDriver();
			return;
		}
		if (Hit(diagnosticsScanRect, point)) {
			RunDeviceDiagnostics();
			return;
		}
		for (size_t i = 0; i < diagnosticsFilterRects.size(); ++i) {
			if (!Hit(diagnosticsFilterRects[i], point)) continue;
			diagnosticsFilter = static_cast<DiagnosticsFilter>(i);
			diagnosticsListOffset = 0;
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
		for (size_t row = 0; row < diagnosticsDriverRects.size(); ++row) {
			const size_t visibleIndex = static_cast<size_t>(diagnosticsListOffset) + row;
			if (!Hit(diagnosticsDriverRects[row], point) || visibleIndex >= diagnosticsVisibleDrivers.size()) continue;
			diagnosticsSelectedDriver = static_cast<int>(diagnosticsVisibleDrivers[visibleIndex]);
			InvalidateRect(_hWnd, nullptr, FALSE);
			return;
		}
	}
	if (page == Page::Logs && Hit(exportLogRect, point)) ExportLogs();
}

void MainWindow::CheckForUpdate(bool byUser)
{
	// 并发门。原子交换保证只有第一次点击会起线程：第二次点击如果探测还在跑，
	// 直接提示并返回，而不是再起一个线程 —— 两个线程各自弹一次结果，先后无法
	// 保证，表现出来就是「点了没反应，再点一下忽然说已是最新版本」。
	if (updateProbeInFlight.exchange(true)) {
		if (byUser) ShowFastTip(L"\u6b63\u5728\u68c0\u67e5\u66f4\u65b0\uff0c\u8bf7\u7a0d\u5019");
		return;
	}

	// 联网预检曾经放在这里，走的是 WinINet；本进程里 WinINet 是坏的，它可能卡住
	// UI 线程好几秒。现在挪进探测线程（见 UpdateProbeThread），点下去立刻有反应。

	// 探测放到**工作线程**里跑，原因有两条：
	//
	// 1) 正确性。WinINet 在 GUI 线程上依赖消息泵（内部会创建隐藏窗口、用消息做
	//    异步通知）。如果直接在 UI 线程里同步调用，消息循环被阻塞，连接会直接
	//    失败 —— 实测报 ERROR_INTERNET_CANNOT_CONNECT，而且 PRECONFIG / DIRECT
	//    两档都一样。控制台程序（没有消息循环）同样代码却能正常连通，可佐证。
	// 2) 体验。探测最坏要等几秒，放在 UI 线程会把界面冻住。
	UpdateProbeContext* context = new (std::nothrow) UpdateProbeContext();
	if (!context) {
		updateProbeInFlight = false;
		if (byUser) ShowFastTip(L"\u5185\u5b58\u4e0d\u8db3\uff0c\u65e0\u6cd5\u68c0\u67e5\u66f4\u65b0");
		return;
	}
	context->window = _hWnd;
	context->byUser = byUser;

	HANDLE thread = CreateThread(nullptr, 0, UpdateProbeThread, context, 0, nullptr);
	if (!thread) {
		delete context;
		updateProbeInFlight = false;
		if (byUser) ShowFastTip(L"\u65e0\u6cd5\u542f\u52a8\u66f4\u65b0\u68c0\u67e5");
		return;
	}
	CloseHandle(thread);
	// 按钮立刻换成「检查中…」并置灰，这是唯一能一直撑到结果回来的视觉反馈：
	// 浮层只有 4 秒，而一次探测最坏要走一遍子进程，中间那段原本是一片空白，
	// 看上去就是卡死。
	InvalidateRect(_hWnd, nullptr, FALSE);
	if (byUser) ShowFastTip(L"\u6b63\u5728\u68c0\u67e5\u66f4\u65b0\u2026");
}

// 探测结果回到 UI 线程后在这里处理：比对版本、弹确认框、必要时启动更新器。
DWORD WINAPI MainWindow::UpdateDownloadThreadProc(LPVOID parameter)
{
	std::unique_ptr<UpdateDownloadContext> context(static_cast<UpdateDownloadContext*>(parameter));
	if (!context) return 1;

	auto fail = [&](const std::wstring& reason) {
		auto* message = new std::wstring(reason);
		PostMessageW(context->window, WM_UPDATE_DOWNLOAD_STATE, 3, reinterpret_cast<LPARAM>(message));
		return 1UL;
	};

	bool downloaded = false;
	if (!g_httpInProcessBroken) {
		downloaded = JUpdater_DownloadPackage(context->url.c_str(), context->packagePath.c_str(),
			UpdateProgressCallback, context.get()) != FALSE;
		if (!downloaded) g_httpInProcessBroken = true;
	}
	if (!downloaded) {
		// 进程内 TLS 不通时的退路：交给子进程下载。子进程不给进度，
		// 这边只报一个"总长度未知"（percent = -1），进度条改画来回走的高光。
		// 注意这里同样要传堆上的结构体：UI 线程统一按指针接管并释放。
		auto* pending = new (std::nothrow) UpdateProgressPayload{ -1, 0, 0 };
		if (pending && !PostMessageW(context->window, WM_UPDATE_DOWNLOAD_STATE, 1,
			reinterpret_cast<LPARAM>(pending))) {
			delete pending;
		}
		downloaded = JUpdater_LaunchDownload(context->url.c_str(), context->packagePath.c_str(),
			context->sha256.c_str(), 300000, context->window) != FALSE;
	}
	if (!downloaded) {
		return fail(L"下载失败，请检查网络或稍后重试");
	}

	if (!context->sha256.empty()) {
		WCHAR actual[65] = {};
		if (!JUpdater_FileSha256(context->packagePath.c_str(), actual, _countof(actual))) {
			// 把现场写进提示里：文件在不在、多少字节、Win32 错误码，
			// 省得下次只能靠猜。
			const DWORD openError = GetLastError();
			WIN32_FILE_ATTRIBUTE_DATA info{};
			const BOOL exists = GetFileAttributesExW(context->packagePath.c_str(),
				GetFileExInfoStandard, &info);
			const unsigned long long size = exists
				? (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow : 0ULL;
			DeleteFileW(context->packagePath.c_str());
			wchar_t detail[160] = {};
			swprintf_s(detail, L"无法计算更新包校验值（错误 %lu，文件%s，%llu 字节）",
				static_cast<unsigned long>(openError), exists ? L"存在" : L"不存在", size);
			return fail(detail);
		}
		std::wstring expected = context->sha256;
		std::wstring got = actual;
		std::transform(expected.begin(), expected.end(), expected.begin(), towlower);
		std::transform(got.begin(), got.end(), got.begin(), towlower);
		if (expected != got) {
			DeleteFileW(context->packagePath.c_str());
			return fail(L"更新包校验失败（SHA-256 不匹配），已丢弃");
		}
	}

	// 先在本进程里换：只需要两次改名，不用拉第二个进程、也不用等谁退出。
	// 失败（文件被扫描器/调试器占着、目录不可写）再退回独立更新器 —— 那条路
	// 会先等本进程退出，退出后占用自然消失，所以两条路是互补的。
	WCHAR inPlaceError[512] = {};
	if (JUpdater_ApplyPackageInPlace(context->packagePath.c_str(), context->sha256.c_str(),
		inPlaceError, _countof(inPlaceError))) {
		DeleteFileW(context->packagePath.c_str());
		// 新版本已经起来了，本进程收尾退出即可。
		PostMessageW(context->window, WM_UPDATE_DOWNLOAD_STATE, 2, 0);
		return 0;
	}
	if (!JUpdater_LaunchApply(context->packagePath.c_str(), context->sha256.c_str())) {
		return fail(std::wstring(L"替换新版本失败：") +
			(inPlaceError[0] ? inPlaceError : L"未知原因"));
	}
	// 交给更新器等本进程退出；一退它就能动手。
	PostMessageW(context->window, WM_UPDATE_DOWNLOAD_STATE, 2, 0);
	return 0;
}

void MainWindow::OnUpdateDownloadState(WPARAM wParam, LPARAM lParam)
{
	switch (wParam) {
	case 1: {
		// 防呆：lParam 必须是堆上的载荷指针。历史上有一版把百分比直接塞在
		// lParam 里（-1），照指针接管就会去 delete 0xffffffff —— 直接 AV。
		std::unique_ptr<UpdateProgressPayload> payload;
		if (lParam > 0x10000) payload.reset(reinterpret_cast<UpdateProgressPayload*>(lParam));
		// 失败或已经收尾之后才到的进度消息直接丢掉（它只负责画进度条）。
		if (updateDialog.mode != UpdateDialogMode::Download) break;
		if (payload) {
			updateDialog.percent = payload->percent;
			updateDialog.done = payload->done;
			updateDialog.total = payload->total;
		}
		updateDialog.status = updateDialog.percent >= 100
			? L"下载完成，正在校验更新包…" : L"正在下载更新包…";
		EnsureAnimationTimer();
		InvalidateRect(_hWnd, nullptr, FALSE);
		break;
	}
	case 2:
		// 文件已经换好，新实例正在等互斥体。留一帧「更新完成」再退出，
		// 否则窗口突然消失，用户会以为程序崩了。
		updateDialog.mode = UpdateDialogMode::Done;
		updateDialog.percent = 100;
		updateDialog.progress = 1.0f;
		focusTarget = -1;
		hoverTarget = -1;
		EnsureAnimationTimer();
		InvalidateRect(_hWnd, nullptr, FALSE);
		SetTimer(_hWnd, TIMER_UPDATE_RESTART, 900, nullptr);
		break;
	case 3: {
		std::unique_ptr<std::wstring> reason(reinterpret_cast<std::wstring*>(lParam));
		std::wstring text = L"未知原因";
		if (reason && !reason->empty()) text = *reason;
		FailUpdateDialog(text);
		break;
	}
	default:
		break;
	}
}

void MainWindow::OnChildDownloadProgress(WPARAM wParam, LPARAM lParam)
{
	// 弹窗已经关掉／收尾之后再来的进度直接丢掉。
	if (updateDialog.mode != UpdateDialogMode::Download) return;
	const unsigned long long total = static_cast<unsigned long long>(wParam);
	const unsigned long long done = static_cast<unsigned long long>(lParam);
	updateDialog.total = total;
	updateDialog.done = done;
	updateDialog.percent = total ? static_cast<int>(done * 100 / total) : -1;
	updateDialog.status = updateDialog.percent >= 100
		? L"下载完成，正在校验更新包…" : L"正在下载更新包…";
	EnsureAnimationTimer();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::ShowUpdateConfirm(const UpdateProbeContext& probe, const std::wstring& currentVersion)
{
	const JUpdaterManifestInfo& manifest = probe.manifest;
	updateDialog.mode = UpdateDialogMode::Confirm;
	updateDialog.url = manifest.url;
	updateDialog.version = manifest.version;
	updateDialog.currentVersion = currentVersion;
	updateDialog.notes = manifest.notes;
	updateDialog.sha256 = manifest.sha256;
	updateDialog.status.clear();
	updateDialog.percent = -1;
	updateDialog.done = 0;
	updateDialog.total = 0;
	updateDialog.progress = 0.0f;
	updateDialog.appear = 0.0f;
	// 焦点默认落在「立即更新」上：回车继续，Esc 等同「稍后」。
	focusTarget = kUpdateDialogPrimary;
	hoverTarget = -1;
	pressTarget = -1;
	ApplyDialogOcclusion();
	SetForegroundWindow(_hWnd);
	SetFocus(_hWnd);
	EnsureAnimationTimer();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::CloseUpdateDialog()
{
	updateDialog.mode = UpdateDialogMode::None;
	updateDialog.appear = 0.0f;
	updateDialog.progress = 0.0f;
	focusTarget = -1;
	hoverTarget = -1;
	pressTarget = -1;
	ApplyDialogOcclusion();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::FailUpdateDialog(const std::wstring& reason)
{
	updateDialog.mode = UpdateDialogMode::Failed;
	updateDialog.status = reason;
	updateDialog.percent = -1;
	focusTarget = kUpdateDialogPrimary;
	hoverTarget = -1;
	EnsureAnimationTimer();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

int MainWindow::HitUpdateDialogAt(POINT point) const
{
	if (!UpdateDialogVisible()) return -1;
	if (!IsRectEmpty(&updateDialogPrimaryRect) && Hit(updateDialogPrimaryRect, point)) return kUpdateDialogPrimary;
	if (!IsRectEmpty(&updateDialogSecondaryRect) && Hit(updateDialogSecondaryRect, point)) return kUpdateDialogSecondary;
	return -1;
}

bool MainWindow::HandleUpdateDialogClick(POINT point)
{
	if (!UpdateDialogVisible()) return false;
	const int target = HitUpdateDialogAt(point);
	if (updateDialog.mode == UpdateDialogMode::Confirm) {
		if (target == kUpdateDialogPrimary) {
			StartUpdateDownload();
			return true;
		}
		if (target == kUpdateDialogSecondary) {
			CloseUpdateDialog();
			ShowFastTip(L"已取消更新");
			return true;
		}
	}
	else if (updateDialog.mode == UpdateDialogMode::Failed) {
		if (target == kUpdateDialogPrimary) {
			// 内置这条路走不通了：交给独立更新器从下载开始重做一遍。
			CloseUpdateDialog();
			if (JUpdater_LaunchUpdater()) ShowFastTip(L"已启动独立更新程序");
			else ShowFastTip(L"无法启动更新程序");
			return true;
		}
		if (target == kUpdateDialogSecondary) {
			CloseUpdateDialog();
			return true;
		}
	}
	// 下载中没有任何可点的东西，但点击也不能漏到底下的页面去 —— 弹窗是模态的。
	return true;
}

void MainWindow::StartUpdateDownload()
{
	WCHAR resolvedUrl[1024] = {};
	if (!JUpdater_ResolveUrl(updateDialog.url.c_str(), resolvedUrl, _countof(resolvedUrl))) {
		FailUpdateDialog(L"更新地址不被信任（只接受 https 和官方域名）");
		return;
	}
	WCHAR tempDir[MAX_PATH] = {};
	if (!GetTempPathW(_countof(tempDir), tempDir)) {
		FailUpdateDialog(L"无法取得临时目录");
		return;
	}
	const std::wstring packagePath = std::wstring(tempDir) +
		L"DzjsTrainer-" + updateDialog.version + L"-update.exe";
	DeleteFileW(packagePath.c_str());

	auto* context = new (std::nothrow) UpdateDownloadContext();
	if (!context) {
		FailUpdateDialog(L"内存不足，无法开始下载");
		return;
	}
	context->window = _hWnd;
	context->url = resolvedUrl;
	context->packagePath = packagePath;
	context->sha256 = updateDialog.sha256;

	updateDialog.mode = UpdateDialogMode::Download;
	updateDialog.percent = -1;
	updateDialog.done = 0;
	updateDialog.total = 0;
	updateDialog.progress = 0.0f;
	updateDialog.status = L"正在建立连接…";
	focusTarget = -1;
	hoverTarget = -1;
	pressTarget = -1;

	HANDLE thread = CreateThread(nullptr, 0, UpdateDownloadThreadProc, context, 0, nullptr);
	if (!thread) {
		delete context;
		FailUpdateDialog(L"无法启动下载线程");
		return;
	}
	CloseHandle(thread);
	EnsureAnimationTimer();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

// 原生子窗口不会跟着主窗口的位图一起被遮住：它们各自有 HWND，系统单独合成，
// 所以弹窗期间先全部藏起来，关掉后再让各页面按自己的状态恢复可见性。
void MainWindow::ApplyDialogOcclusion()
{
	if (!UpdateDialogVisible()) {
		LayoutEmbeddedWindows();
		return;
	}
	for (HWND control : advancedControls) if (control) ShowWindow(control, SW_HIDE);
	for (HWND control : networkControls) if (control) ShowWindow(control, SW_HIDE);
	for (HWND control : { teacherHostWindow, teacherLaunchButton, teacherRefreshButton, teacherViewButton,
		teacherStudentList, teacherChatInput, teacherChatButton, teacherBlackButton, teacherUnlockButton,
		teacherShutdownButton, teacherRebootButton }) {
		if (control) ShowWindow(control, SW_HIDE);
	}
}

void MainWindow::FinishUpdateProbe(const UpdateProbeContext& probe)
{
	// 第一件事永远是解锁并发门并重绘按钮。放在最前面是因为下面每条路径都可能
	// 提前 return —— 漏掉一处，按钮就会永久停在「检查中…」，那比没有状态更糟。
	updateProbeInFlight = false;
	InvalidateRect(_hWnd, nullptr, FALSE);

	// 把这一轮的结果和耗时落进日志。之前这条链路只有失败才写日志，成功静默；
	// 于是「第一次点没反应」只能靠猜是握手慢、子进程慢还是读文件慢。
	if (currentLogger) {
		currentLogger->LogInfo(L"更新探测完成：ok=%d 路径=%s version=\"%s\" 耗时=%llu ms",
			probe.ok ? 1 : 0,
			probe.offline ? L"未联网" : (probe.viaSubprocess ? L"子进程" : L"进程内"),
			probe.manifest.version,
			static_cast<unsigned long long>(probe.startedAt ? GetTickCount64() - probe.startedAt : 0));
	}

	if (probe.offline) {
		if (probe.byUser) ShowFastTip(L"\u5f53\u524d\u6ca1\u6709\u53ef\u7528\u7684\u7f51\u7edc\u8fde\u63a5");
		return;
	}
	if (!probe.ok) {
		// 一句"无法获取更新信息"会把三种完全不同的失败混成同一个现象：
		// 网络不通、服务器没给时效票据、本机时间和服务器差太多。
		// 有具体原因就直接摆在卡片上，用户才好判断是等一会儿再试还是去对时。
		// 用户主动点的这次一律用卡片：浮层 4 秒就没了，失败容易看着像没反应。
		const std::wstring reason = probe.manifest.error;
		if (!reason.empty()) {
			FailUpdateDialog(L"无法获取更新信息：" + reason);
		}
		else if (probe.byUser) {
			FailUpdateDialog(probe.viaSubprocess
				? L"无法获取更新信息：内置更新程序没有返回有效清单"
				: L"无法获取更新信息");
		}
		return;
	}
	const JUpdaterManifestInfo& manifest = probe.manifest;

	const std::wstring current = ReadOwnFileVersion();
	if (!current.empty() && JUpdater_CompareVersion(manifest.version, current.c_str()) <= 0) {
		if (probe.byUser) ShowFastTip(L"\u5f53\u524d\u5df2\u662f\u6700\u65b0\u7248\u672c");
		return;
	}

	// 没有哈希就不更新：清单只要漏掉 sha256，等于「下载什么就装什么」。
	if (manifest.sha256[0] == L'\0') {
		FailUpdateDialog(L"更新清单缺少 sha256 校验值，已拒绝本次更新");
		return;
	}

	// 确认框也自己画：版本、更新说明、两个按钮都在窗口里的卡片上，
	// 用户点「立即更新」之后同一张卡片原地变成下载进度。
	ShowUpdateConfirm(probe, current);
}

void MainWindow::SelectPage(Page target)
{
	if (page == target) return;
	page = target;
	// 网络页里有真实的子窗口（教师端宿主），高级配置页里有原生热键控件：
	// 这些 HWND 不会跟着自绘内容一起平移，所以这两页直接跳过入场位移。
	pageMotion = (target == Page::Network || target == Page::Advanced) ? 1.0 : 0.0;
	pressTarget = -1;
	EnsureAnimationTimer();
	LayoutEmbeddedWindows();
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::LayoutEmbeddedWindows()
{
	if (!_hWnd) return;
	LayoutNetworkControls();
	LayoutAdvancedControls();
}

void MainWindow::CreateAdvancedControls()
{
	HINSTANCE instance = JTAppGetInstanceDirect();
	// 两个快捷键框已经改成自绘（见 PaintAdvanced / HandleHotkeyKey），
	// 原生 HOTKEY 控件会画自己的直角黑边，和圆角风格冲突，这里不再创建。
	advancedControls[2] = CreateWindowExW(0, L"EDIT", L"3100",
		WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 10, 10, _hWnd,
		reinterpret_cast<HMENU>(IDC_ADV_INTERVAL), instance, nullptr);
	for (HWND control : advancedControls) {
		if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
	}
	LoadAdvancedSettings();
	LayoutAdvancedControls();
}

void MainWindow::LayoutAdvancedControls()
{
	if (!advancedControls[2]) return;
	RECT client{};
	GetClientRect(_hWnd, &client);
	const bool showCompat = page == Page::Advanced && advancedTab == 1;
	// 和 PaintAdvanced 用同一组公式；输入框缩进圆角框内 4px。
	MoveWindow(advancedControls[2], client.right - 238, 286, 182, 30, TRUE);
	ShowWindow(advancedControls[2], showCompat ? SW_SHOW : SW_HIDE);
}

void MainWindow::SetHotkeyValue(int index, int value)
{
	if (index == 0) hotkeyFakeFullValue = value;
	else hotkeyShowHideValue = value;
}

void MainWindow::HandleHotkeyKey(UINT virtualKey)
{
	if (hotkeyCaptureIndex < 0) return;
	const int index = hotkeyCaptureIndex;
	if (virtualKey == VK_ESCAPE) {
		SetHotkeyValue(index, 0);
		hotkeyCaptureIndex = -1;
		ShowFastTip(L"\u5df2\u6e05\u9664\u8be5\u5feb\u6377\u952e");
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	// 只按下修饰键不算一个组合，继续等主键；IME 的 ProcessKey 也忽略。
	if (virtualKey == VK_SHIFT || virtualKey == VK_CONTROL || virtualKey == VK_MENU ||
		virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT || virtualKey == VK_LCONTROL ||
		virtualKey == VK_RCONTROL || virtualKey == VK_LMENU || virtualKey == VK_RMENU ||
		virtualKey == VK_LWIN || virtualKey == VK_RWIN || virtualKey == VK_PROCESSKEY) {
		return;
	}
	int mods = 0;
	if (GetKeyState(VK_SHIFT) & 0x8000) mods |= 0x01;
	if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 0x02;
	if (GetKeyState(VK_MENU) & 0x8000) mods |= 0x04;
	const int value = (mods << 8) | static_cast<int>(virtualKey & 0xFF);
	SetHotkeyValue(index, value);
	hotkeyCaptureIndex = -1;
	const std::wstring message = L"\u5feb\u6377\u952e\u5df2\u8bbe\u4e3a " + HotkeyDisplayText(value);
	ShowFastTip(message.c_str());
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::LoadAdvancedSettings()
{
	auto settings = JTAppGetSettingsDirect();
	advDisableDriver = settings->GetSettingBool(L"DisableDriver", false);
	advSelfProtect = settings->GetSettingBool(L"SelfProtect", true);
	// The hook consumes AutoForceKill; BandAllRunOp is a separate protection-policy flag.
	advAutoForceKill = settings->GetSettingBool(L"AutoForceKill", false);
	advStrictWindow = settings->GetSettingBool(L"AutoIncludeFullWindow", false);
	advHideOutput = settings->GetSettingBool(L"DoNotShowVirusWindow", true);
	advHideTaskbar = settings->GetSettingBool(L"DoNotShowTrayIcon", false);
	advController = currentWorker ? currentWorker->Running() : true;
	advAlwaysUpdate = settings->GetSettingBool(L"AlwaysCheckUpdate", false);
	advForceCurrentDir = settings->GetSettingBool(L"ForceInstallInCurrentDir", false);
	advDisableWatchdog = settings->GetSettingBool(L"ForceDisableWatchDog", false);
	advInjectMaster = settings->GetSettingBool(L"InjectMasterHelper", false);
	advInject64 = settings->GetSettingBool(L"InjectProcHelper64", false);
	hotkeyFakeFullValue = settings->GetSettingInt(L"HotKeyFakeFull", 1606);
	hotkeyShowHideValue = settings->GetSettingInt(L"HotKeyShowHide", 1604);
	auto killMode = settings->GetSettingStrPtr(L"KillProcess", L"NtTerminateProcess");
	advancedKillMode = *killMode == L"TerminateProcess" ? 0 : (*killMode == L"KernelMode" ? 2 : 1);
	FreeStringPtr(killMode);
	auto injectMode = settings->GetSettingStrPtr(L"InjectMode", L"RemoteThread");
	advancedInjectMode = *injectMode == L"HookDllStub" ? 1 : 0;
	FreeStringPtr(injectMode);
	auto interval = settings->GetSettingStrPtr(L"CKInterval", L"3100");
	SetWindowTextW(advancedControls[2], interval->c_str());
	FreeStringPtr(interval);
}

void MainWindow::SaveAdvancedSettings()
{
	auto settings = JTAppGetSettingsDirect();
	// These are startup settings, but they must be persisted on both x86 and x64.
	settings->SetSettingBool(L"DisableDriver", advDisableDriver);
	settings->SetSettingBool(L"SelfProtect", advSelfProtect);
	settings->SetSettingBool(L"AutoForceKill", advAutoForceKill);
	settings->SetSettingBool(L"AutoIncludeFullWindow", advStrictWindow);
	settings->SetSettingBool(L"DoNotShowVirusWindow", advHideOutput);
	settings->SetSettingBool(L"DoNotShowTrayIcon", advHideTaskbar);
	settings->SetSettingBool(L"AlwaysCheckUpdate", advAlwaysUpdate);
	settings->SetSettingBool(L"ForceInstallInCurrentDir", advForceCurrentDir);
	settings->SetSettingBool(L"ForceDisableWatchDog", advDisableWatchdog);
	settings->SetSettingBool(L"InjectMasterHelper", advInjectMaster);
	settings->SetSettingBool(L"InjectProcHelper64", advInject64);
	// Apply driver/self-protection switches immediately instead of leaving the
	// already-running process on its previous startup state.
	JTAppRunOperationDirect(AppOperationForceLoadDriver);
	settings->SetSettingInt(L"HotKeyFakeFull", hotkeyFakeFullValue);
	settings->SetSettingInt(L"HotKeyShowHide", hotkeyShowHideValue);
	settings->SetSettingStr(L"KillProcess", advancedKillMode == 0 ? L"TerminateProcess" : advancedKillMode == 2 ? L"KernelMode" : L"NtTerminateProcess");
	settings->SetSettingStr(L"InjectMode", advancedInjectMode == 1 ? L"HookDllStub" : L"RemoteThread");
	wchar_t intervalText[16]{};
	GetWindowTextW(advancedControls[2], intervalText, _countof(intervalText));
	int interval = _wtoi(intervalText);
	if (interval < 1000 || interval > 10000) { interval = 3100; SetWindowTextW(advancedControls[2], L"3100"); }
	wchar_t normalized[16]{};
	swprintf_s(normalized, L"%d", interval);
	settings->SetSettingStr(L"CKInterval", normalized);
	if (currentWorker) {
		if (currentWorker) {
			if (advController && !currentWorker->Running()) currentWorker->Start();
			else if (!advController && currentWorker->Running()) currentWorker->Stop();
			currentWorker->InitSettings();
		}
	}
	if (setDoNotShowTrayIcon != advHideTaskbar) {
		setDoNotShowTrayIcon = advHideTaskbar;
		if (setDoNotShowTrayIcon) {
			Shell_NotifyIconW(NIM_DELETE, &nid);
			trayIconRegistered = false;
		}
		else CreateTrayIcon();
	}
	LoadSettings();
	ShowFastTip(L"\u9ad8\u7ea7\u8bbe\u7f6e\u5df2\u4fdd\u5b58");
}

void MainWindow::CreateNetworkControls()
{
	controlSurfaceBrush = CreateSolidBrush(RGB(255, 255, 255));
	advancedFieldBrush = CreateSolidBrush(RGB(255, 255, 255));
	controlFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	auto make = [this](int index, LPCWSTR cls, LPCWSTR text, DWORD style, int id, DWORD exStyle = 0) {
		networkControls[index] = CreateWindowExW(exStyle, cls, text, WS_CHILD | style,
			0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), JTAppGetInstanceDirect(), nullptr);
		SendMessageW(networkControls[index], WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
		if (style & BS_OWNERDRAW) EnableNativeButtonHover(networkControls[index]);
	};
	make(0, L"STATIC", L"\u76ee\u6807 IP", SS_LEFT, 0);
	make(1, L"EDIT", L"127.0.0.1", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NET_IP);
	make(2, L"STATIC", L"\u7aef\u53e3", SS_LEFT, 0);
	make(3, L"EDIT", L"4705", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_NET_PORT);
	make(4, L"BUTTON", L"\u68c0\u6d4b\u7aef\u53e3", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_DETECT_PORT);
	make(5, L"STATIC", L"\u6d88\u606f\u5185\u5bb9", SS_LEFT, 0);
	make(6, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NET_MESSAGE);
	make(7, L"BUTTON", L"\u53d1\u9001\u6d88\u606f", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_SEND_MESSAGE);
	make(8, L"STATIC", L"\u8fdc\u7a0b\u547d\u4ee4", SS_LEFT, 0);
	make(9, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NET_COMMAND);
	make(10, L"BUTTON", L"\u6267\u884c\u547d\u4ee4", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_SEND_COMMAND);
	make(11, L"BUTTON", L"\u626b\u63cf\u5c40\u57df\u7f51", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_SCAN);
	make(12, L"BUTTON", L"\u5173\u673a", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_SHUTDOWN);
	make(13, L"BUTTON", L"\u91cd\u542f", BS_OWNERDRAW | WS_TABSTOP, IDC_NET_REBOOT);
	make(14, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, IDC_NET_RESULTS);
	teacherHostWindow = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, _hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_TEACHER + 1)), JTAppGetInstanceDirect(), nullptr);
	teacherLaunchButton = CreateWindowExW(0, L"BUTTON", L"\u542f\u52a8\u6559\u5e08\u670d\u52a1",
		WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, _hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NET_TEACHER)), JTAppGetInstanceDirect(), nullptr);
	SendMessageW(teacherLaunchButton, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
	EnableNativeButtonHover(teacherLaunchButton);
	teacherRefreshButton = CreateWindowExW(0, L"BUTTON", L"\u5237\u65b0\u5217\u8868", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_REFRESH)), JTAppGetInstanceDirect(), nullptr);
	teacherViewButton = CreateWindowExW(0, L"BUTTON", L"\u5b9e\u65f6\u753b\u9762", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_VIEW)), JTAppGetInstanceDirect(), nullptr);
	teacherStudentList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_STUDENTS)), JTAppGetInstanceDirect(), nullptr);
	teacherChatInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"\u8bf7\u8ba4\u771f\u542c\u8bfe", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_CHAT_INPUT)), JTAppGetInstanceDirect(), nullptr);
	teacherChatButton = CreateWindowExW(0, L"BUTTON", L"\u53d1\u9001\u804a\u5929", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_CHAT_SEND)), JTAppGetInstanceDirect(), nullptr);
	teacherBlackButton = CreateWindowExW(0, L"BUTTON", L"\u9ed1\u5c4f 10 \u79d2", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_BLACK)), JTAppGetInstanceDirect(), nullptr);
	teacherUnlockButton = CreateWindowExW(0, L"BUTTON", L"\u89e3\u9501", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_UNLOCK)), JTAppGetInstanceDirect(), nullptr);
	teacherShutdownButton = CreateWindowExW(0, L"BUTTON", L"\u5173\u673a", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_SHUTDOWN)), JTAppGetInstanceDirect(), nullptr);
	teacherRebootButton = CreateWindowExW(0, L"BUTTON", L"\u91cd\u542f", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEACHER_REBOOT)), JTAppGetInstanceDirect(), nullptr);
	for (HWND control : { teacherRefreshButton, teacherViewButton, teacherStudentList, teacherChatInput, teacherChatButton, teacherBlackButton,
		teacherUnlockButton, teacherShutdownButton, teacherRebootButton })
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
	for (HWND button : { teacherRefreshButton, teacherViewButton, teacherChatButton, teacherBlackButton,
		teacherUnlockButton, teacherShutdownButton, teacherRebootButton })
		EnableNativeButtonHover(button);
	SendMessageW(teacherChatInput, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
	for (int index : { 1, 3, 6, 9 })
		SendMessageW(networkControls[index], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
	if (JyUdpAttack::currentJyUdpAttack)
		JyUdpAttack::currentJyUdpAttack->sendResultReceivehWnd = _hWnd;
	LayoutNetworkControls();
}

void MainWindow::LayoutNetworkControls()
{
	if (!networkControls[0]) return;
	RECT client{};
	GetClientRect(_hWnd, &client);
	const bool visible = false;
	const int x = 280;
	const int y = 185;
	const int w = client.right > 308 ? static_cast<int>(client.right - 308) : 596;
	const int fieldW = std::max(174, w - 422);
	auto place = [&](int index, int px, int py, int pw, int ph) {
		MoveWindow(networkControls[index], px, py, pw, ph, TRUE);
		ShowWindow(networkControls[index], visible ? SW_SHOW : SW_HIDE);
	};
	for (int index : { 0, 2, 5, 8 }) ShowWindow(networkControls[index], SW_HIDE);
	place(1, x + 74, y, fieldW, 30);
	place(3, x + 136 + fieldW, y, 72, 30);
	place(4, x + 218 + fieldW, y - 2, 104, 34);
	place(6, x + 74, y + 75, w - 184, 30);
	place(7, x + w - 100, y + 73, 100, 34);
	place(9, x + 74, y + 141, w - 184, 30);
	place(10, x + w - 100, y + 139, 100, 34);
	place(11, x, y + 199, 126, 36);
	place(12, x + 136, y + 199, 84, 36);
	place(13, x + 230, y + 199, 84, 36);
	place(14, x, y + 291, w, std::max(72, static_cast<int>(client.bottom) - y - 319));
	LayoutTeacherControls();
}

void MainWindow::LayoutTeacherControls()
{
	if (!teacherLaunchButton) return;
	{
	RECT client{};
	GetClientRect(_hWnd, &client);
	const bool visible = page == Page::Network;
	const int hostX = 264;
	const int hostY = 170;
	const int hostW = std::max(560, static_cast<int>(client.right) - hostX - 24);
	const int hostH = std::max(320, static_cast<int>(client.bottom) - hostY - 24);
	if (teacherHostWindow) {
		MoveWindow(teacherHostWindow, hostX, hostY, hostW, hostH, TRUE);
		// 只在真正嵌入原教师端窗口时才显示宿主框。没有嵌入时它是一个空的
		// WS_EX_CLIENTEDGE STATIC，会盖住 PaintNetwork 画好的圆角卡片。
		const bool hostVisible = visible && teacherChildWindow != nullptr && IsWindow(teacherChildWindow);
		ShowWindow(teacherHostWindow, hostVisible ? SW_SHOW : SW_HIDE);
	}
	MoveWindow(teacherLaunchButton, hostX, 120, 144, 34, TRUE);
	ShowWindow(teacherLaunchButton, visible ? SW_SHOW : SW_HIDE);
	for (HWND control : { teacherRefreshButton, teacherViewButton, teacherStudentList, teacherChatInput, teacherChatButton,
		teacherBlackButton, teacherUnlockButton, teacherShutdownButton, teacherRebootButton })
		if (control) ShowWindow(control, SW_HIDE);
	LayoutEmbeddedTeacherGui();
	return;
	}
	RECT client{};
	GetClientRect(_hWnd, &client);
	const bool visible = page == Page::Network;
	const int left = 280;
	const int top = 184;
	const int right = std::max(left + 520, static_cast<int>(client.right) - 28);
	const int listRight = left + std::min(520, std::max(420, (right - left) * 46 / 100));
	const int listBottom = std::max(top + 240, static_cast<int>(client.bottom) - 36);
	auto show = [visible](HWND window, int x, int y, int w, int h) {
		if (!window) return;
		MoveWindow(window, x, y, w, h, TRUE);
		ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
	};
	show(teacherLaunchButton, left, 120, 170, 34);
	show(teacherRefreshButton, left + 182, 120, 120, 34);
	show(teacherViewButton, left + 314, 120, 120, 34);
	show(teacherStudentList, left, top + 48, listRight - left, listBottom - top - 48);
	const int actionLeft = listRight + 28;
	const int actionWidth = std::max(300, right - actionLeft);
	show(teacherChatInput, actionLeft, top + 52, actionWidth - 116, 32);
	show(teacherChatButton, actionLeft + actionWidth - 108, top + 50, 108, 36);
	show(teacherBlackButton, actionLeft, top + 106, 126, 36);
	show(teacherUnlockButton, actionLeft + 138, top + 106, 100, 36);
	show(teacherShutdownButton, actionLeft, top + 162, 100, 36);
	show(teacherRebootButton, actionLeft + 112, top + 162, 100, 36);
	const auto viewState = teacherBackend && teacherBackend->running && !teacherBackend->selected_ip.empty()
		? teacherBackend->service.remoteViewState(teacherBackend->selected_ip) : jiyu::RemoteViewState{};
	if (teacherViewButton) SetWindowTextW(teacherViewButton, viewState.active ? L"\u505c\u6b62\u753b\u9762" : L"\u5b9e\u65f6\u753b\u9762");
}

static std::wstring teacherUtf8ToWide(const std::string& text)
{
	if (text.empty()) return {};
	const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring result(static_cast<size_t>(std::max(0, size)), L'\0');
	if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
	return result;
}

static std::string teacherWideToUtf8(const std::wstring& text)
{
	if (text.empty()) return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(std::max(0, size)), '\0');
	if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
	return result;
}

void MainWindow::StartTeacherService()
{
	if (!teacherBackend) teacherBackend = std::make_unique<TeacherBackend>();
	if (teacherBackend->running) {
		StopTeacherService();
		return;
	}
	teacherBackend->options.preview_dir = std::filesystem::current_path() / "teacher_remote_view";
	teacherBackend->options.teacher_name = "1";
	teacherBackend->options.channel = 1;
	teacherBackend->options.tcp_mode = 1;
	teacherBackend->options.tcp_port = 4806;
	teacherBackend->options.remote_control_enabled = true;
	std::string error;
	if (!teacherBackend->service.start(teacherBackend->options, &error)) {
		const auto message = teacherUtf8ToWide(error.empty() ? "teacher service start failed" : error);
		AppendNetworkResult(message.c_str());
		return;
	}
	teacherBackend->running = true;
	SetWindowTextW(teacherLaunchButton, L"\u505c\u6b62\u6559\u5e08\u670d\u52a1");
	AppendNetworkResult(L"\u6559\u5e08\u670d\u52a1\u5df2\u542f\u52a8\uff0c\u6b63\u5728\u53d1\u73b0\u5b66\u751f");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::StopTeacherService()
{
	if (!teacherBackend) return;
	if (teacherBackend->running) teacherBackend->service.stop();
	teacherBackend->running = false;
	teacherBackend->students.clear();
	teacherBackend->list_signature.clear();
	teacherDisplayedIps.clear();
	teacherBackend->selected_ip.clear();
	teacherBackend->latest_preview.clear();
	teacherBackend->displayed_preview.clear();
	teacherBackend->preview_bitmap.reset();
	teacherBackend->remote_bitmap.reset();
	teacherBackend->remote_ip.clear();
	teacherBackend->remote_sequence = 0;
	if (teacherLaunchButton) SetWindowTextW(teacherLaunchButton, L"\u542f\u52a8\u6559\u5e08\u670d\u52a1");
}

void MainWindow::PollTeacherService()
{
	if (!teacherBackend || !teacherBackend->running) return;
	bool visualChanged = false;
	const int oldSelected = teacherStudentList ? static_cast<int>(SendMessageW(teacherStudentList, LB_GETCURSEL, 0, 0)) : -1;
	teacherBackend->students = teacherBackend->service.studentsSnapshot();
	std::string listSignature;
	for (const auto& student : teacherBackend->students) {
		listSignature += student.ip;
		listSignature.push_back('|');
		listSignature += student.logged_in ? "1" : "0";
		listSignature.push_back('|');
		listSignature += student.preview_status;
		listSignature.push_back('|');
		listSignature += student.fixed_preview.u8string();
		listSignature.push_back('\n');
	}
	if (teacherStudentList) {
		if (listSignature != teacherBackend->list_signature) {
			SendMessageW(teacherStudentList, LB_RESETCONTENT, 0, 0);
			teacherDisplayedIps.clear();
			for (const auto& student : teacherBackend->students) {
				std::string line = student.ip + (student.logged_in ? "  | online | " : "  | seen | ") + student.preview_status;
				const auto wide = teacherUtf8ToWide(line);
				SendMessageW(teacherStudentList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
				teacherDisplayedIps.push_back(student.ip);
			}
			teacherBackend->list_signature = listSignature;
			visualChanged = true;
		}
		if (oldSelected >= 0 && oldSelected < static_cast<int>(teacherDisplayedIps.size()))
			SendMessageW(teacherStudentList, LB_SETCURSEL, oldSelected, 0);
	}
	const int selected = teacherStudentList ? static_cast<int>(SendMessageW(teacherStudentList, LB_GETCURSEL, 0, 0)) : -1;
	const std::string previousIp = teacherBackend->selected_ip;
	teacherBackend->selected_ip = selected >= 0 && selected < static_cast<int>(teacherDisplayedIps.size()) ? teacherDisplayedIps[static_cast<size_t>(selected)] : std::string{};
	if (teacherBackend->selected_ip != previousIp) {
		teacherBackend->latest_preview.clear();
		teacherBackend->displayed_preview.clear();
		teacherBackend->preview_bitmap.reset();
		teacherBackend->remote_bitmap.reset();
		teacherBackend->remote_sequence = 0;
		visualChanged = true;
	}
	if (!teacherBackend->selected_ip.empty()) {
		for (const auto& student : teacherBackend->students) {
			if (student.ip == teacherBackend->selected_ip && !student.fixed_preview.empty()) {
				teacherBackend->latest_preview = student.fixed_preview;
				break;
			}
		}
	}
	for (const auto& event : teacherBackend->service.drainEvents()) {
		if (!event.fixed_preview_path.empty()) teacherBackend->latest_preview = event.fixed_preview_path;
		const auto wide = teacherUtf8ToWide(event.level + " " + event.message);
		AppendNetworkResult(wide.c_str());
		visualChanged = true;
	}
	teacherBackend->remote_frames = teacherBackend->service.drainRemoteFrames();
	for (const auto& frame : teacherBackend->remote_frames) {
		if (frame.student_ip == teacherBackend->selected_ip && frame.sequence != teacherBackend->remote_sequence) {
			teacherBackend->remote_bitmap = TeacherBitmapFromFrame(frame);
			teacherBackend->remote_ip = frame.student_ip;
			teacherBackend->remote_sequence = frame.sequence;
			visualChanged = true;
		}
	}
	if (visualChanged) {
		LayoutTeacherControls();
		InvalidateRect(_hWnd, nullptr, FALSE);
	}
}

void MainWindow::HandleTeacherCommand(int command)
{
	if (command == IDC_NET_TEACHER) { StartTeacherService(); return; }
	if (!teacherBackend || !teacherBackend->running) {
		AppendNetworkResult(L"\u8bf7\u5148\u542f\u52a8\u6559\u5e08\u670d\u52a1");
		return;
	}
	if (command == IDC_TEACHER_REFRESH) { teacherBackend->service.requestPreviewAll(); return; }
	const int selected = teacherStudentList ? static_cast<int>(SendMessageW(teacherStudentList, LB_GETCURSEL, 0, 0)) : -1;
	if (selected < 0 || selected >= static_cast<int>(teacherDisplayedIps.size())) {
		AppendNetworkResult(L"\u8bf7\u5148\u5728\u5b66\u751f\u5217\u8868\u4e2d\u9009\u62e9\u8bbe\u5907");
		return;
	}
	const std::string ip = teacherDisplayedIps[static_cast<size_t>(selected)];
	teacherBackend->selected_ip = ip;
	if (command == IDC_TEACHER_VIEW) {
		const auto state = teacherBackend->service.remoteViewState(ip);
		if (state.active) {
			teacherBackend->service.stopRemoteView(ip);
			teacherBackend->remote_bitmap.reset();
			AppendNetworkResult(L"\u5df2\u505c\u6b62\u5b9e\u65f6\u753b\u9762");
		}
		else {
			std::string error;
			if (!teacherBackend->service.startRemoteView(ip, teacherBackend->options.tcp_port, &error))
				AppendNetworkResult(teacherUtf8ToWide(error.empty() ? "remote view start failed" : error).c_str());
			else AppendNetworkResult(L"\u5df2\u8bf7\u6c42\u5b9e\u65f6\u753b\u9762");
		}
		LayoutTeacherControls();
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	if (command == IDC_TEACHER_CHAT_SEND) {
		wchar_t buffer[512]{};
		GetWindowTextW(teacherChatInput, buffer, _countof(buffer));
		teacherBackend->service.sendChat(ip, teacherWideToUtf8(buffer));
	} else if (command == IDC_TEACHER_BLACK) {
		teacherBackend->service.sendBlackscreen(ip, true, 10, "\u8bf7\u8ba4\u771f\u542c\u8bfe");
	} else if (command == IDC_TEACHER_UNLOCK) {
		teacherBackend->service.sendUnlock(ip);
	} else if (command == IDC_TEACHER_SHUTDOWN || command == IDC_TEACHER_REBOOT) {
		teacherBackend->service.sendShutdown(ip, command == IDC_TEACHER_REBOOT, 0, true, "\u8bf7\u4fdd\u5b58\u4f5c\u4e1a");
	}
}

void MainWindow::LayoutEmbeddedTeacherGui()
{
	if (!teacherChildWindow || !teacherHostWindow) return;
	RECT client{};
	GetClientRect(teacherHostWindow, &client);
	SetWindowPos(teacherChildWindow, HWND_TOP, 0, 0,
		std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top),
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	// 宿主框默认是隐藏的（见 LayoutTeacherControls），嵌入成功后要显式打开，
	// 且只在网络页打开，避免轮询把它在别的页面上又翻出来。
	if (page == Page::Network) ShowWindow(teacherHostWindow, SW_SHOW);
}

bool MainWindow::AttachEmbeddedTeacherGui()
{
	if (!teacherProcess || !teacherProcessId || !teacherHostWindow) return false;
	if (!teacherChildWindow || !IsWindow(teacherChildWindow)) {
		TeacherWindowSearch search{ teacherProcessId, nullptr };
		EnumWindows(FindTeacherWindow, reinterpret_cast<LPARAM>(&search));
		teacherChildWindow = search.window;
	}
	if (!teacherChildWindow) return false;

	LONG_PTR style = GetWindowLongPtrW(teacherChildWindow, GWL_STYLE);
	style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
		WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER);
	style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
	SetWindowLongPtrW(teacherChildWindow, GWL_STYLE, style);

	LONG_PTR exStyle = GetWindowLongPtrW(teacherChildWindow, GWL_EXSTYLE);
	exStyle &= ~(WS_EX_APPWINDOW | WS_EX_TOOLWINDOW);
	exStyle |= WS_EX_CONTROLPARENT;
	SetWindowLongPtrW(teacherChildWindow, GWL_EXSTYLE, exStyle);

	SetLastError(ERROR_SUCCESS);
	SetParent(teacherChildWindow, teacherHostWindow);
	if (GetParent(teacherChildWindow) != teacherHostWindow) {
		teacherChildWindow = nullptr;
		return false;
	}
	SetWindowPos(teacherChildWindow, HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	ShowWindow(teacherChildWindow, SW_SHOW);
	SetWindowTextW(teacherLaunchButton, L"\u5173\u95ed\u6559\u5e08\u7aef");
	teacherLaunchPending = false;
	teacherLaunchDeadline = 0;
	LayoutEmbeddedTeacherGui();
	AppendNetworkResult(L"\u6559\u5e08\u7aef\u5df2\u5d4c\u5165\u5230\u7f51\u7edc\u5de5\u5177");
	InvalidateRect(_hWnd, nullptr, FALSE);
	return true;
}

void MainWindow::LaunchEmbeddedTeacherGui()
{
	if (teacherProcess) {
		StopEmbeddedTeacherGui();
		return;
	}
	wchar_t modulePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
	std::wstring base = modulePath;
	const size_t slash = base.find_last_of(L"\\/");
	base = slash == std::wstring::npos ? L"." : base.substr(0, slash);
	std::vector<std::wstring> candidates = {
		base + L"\\TeacherEndpoint\\JiYuTeacherGui.exe",
		base + L"\\JiYuTeacherGui.exe"
	};
	std::wstring executable;
	for (const auto& candidate : candidates) {
		if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
			executable = candidate;
			break;
		}
	}
	if (executable.empty()) {
		MessageBoxW(_hWnd, L"\u672a\u627e\u5230 TeacherEndpoint\\JiYuTeacherGui.exe\uff0c\u8bf7\u5148\u6784\u5efa\u5e76\u653e\u5165\u4e3b\u7a0b\u5e8f\u76ee\u5f55\u3002", L"\u7f51\u7edc\u5de5\u5177", MB_OK | MB_ICONERROR);
		return;
	}
	std::wstring commandLine = L"\"" + executable + L"\"";
	std::wstring workingDirectory = executable;
	const size_t executableSlash = workingDirectory.find_last_of(L"\\/");
	workingDirectory = executableSlash == std::wstring::npos ? base : workingDirectory.substr(0, executableSlash);
	std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
	command.push_back(L'\0');
	STARTUPINFOW startup{ sizeof(startup) };
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
		CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process)) {
		MessageBoxW(_hWnd, L"\u542f\u52a8\u6559\u5e08\u7aef\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5 TeacherEndpoint \u6587\u4ef6\u548c\u4f9d\u8d56\u5e93\u3002", L"\u7f51\u7edc\u5de5\u5177", MB_OK | MB_ICONERROR);
		return;
	}
	CloseHandle(process.hThread);
	teacherProcess = process.hProcess;
	teacherProcessId = process.dwProcessId;
	teacherLaunchPending = true;
	teacherLaunchDeadline = GetTickCount64() + 10000;
	SetWindowTextW(teacherLaunchButton, L"\u53d6\u6d88\u542f\u52a8");
	WaitForInputIdle(teacherProcess, 100);
	AttachEmbeddedTeacherGui();
}

void MainWindow::StopEmbeddedTeacherGui()
{
	if (teacherChildWindow && IsWindow(teacherChildWindow)) PostMessageW(teacherChildWindow, WM_CLOSE, 0, 0);
	if (teacherProcess) {
		if (WaitForSingleObject(teacherProcess, 1200) == WAIT_TIMEOUT)
			TerminateProcess(teacherProcess, 0);
		WaitForSingleObject(teacherProcess, 1000);
		CloseHandle(teacherProcess);
	}
	teacherProcess = nullptr;
	teacherProcessId = 0;
	teacherChildWindow = nullptr;
	teacherLaunchPending = false;
	teacherLaunchDeadline = 0;
	if (teacherLaunchButton) SetWindowTextW(teacherLaunchButton, L"\u542f\u52a8\u6559\u5e08\u7aef");
	InvalidateRect(_hWnd, nullptr, FALSE);
}

void MainWindow::PollEmbeddedTeacherGui()
{
	if (!teacherProcess) return;
	DWORD exitCode = STILL_ACTIVE;
	if (!GetExitCodeProcess(teacherProcess, &exitCode) || exitCode != STILL_ACTIVE) {
		CloseHandle(teacherProcess);
		teacherProcess = nullptr;
		teacherProcessId = 0;
		teacherChildWindow = nullptr;
		teacherLaunchPending = false;
		teacherLaunchDeadline = 0;
		if (teacherLaunchButton) SetWindowTextW(teacherLaunchButton, L"\u542f\u52a8\u6559\u5e08\u7aef");
		AppendNetworkResult(L"\u6559\u5e08\u7aef\u5df2\u9000\u51fa");
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	// The endpoint can create its console after the FLTK window. Sweep on every
	// timer tick so a late console cannot remain visible after embedding.
	TeacherWindowSearch consoleSweep{ teacherProcessId, nullptr };
	EnumWindows(FindTeacherWindow, reinterpret_cast<LPARAM>(&consoleSweep));
	if (teacherChildWindow && !IsWindow(teacherChildWindow)) {
		teacherChildWindow = nullptr;
		teacherLaunchPending = true;
		teacherLaunchDeadline = GetTickCount64() + 5000;
	}
	if (!teacherChildWindow) {
		if (AttachEmbeddedTeacherGui()) return;
		if (teacherLaunchPending && teacherLaunchDeadline && GetTickCount64() >= teacherLaunchDeadline) {
			StopEmbeddedTeacherGui();
			AppendNetworkResult(L"\u6559\u5e08\u7aef\u7a97\u53e3\u521b\u5efa\u8d85\u65f6");
			ShowFastTip(L"\u6559\u5e08\u7aef\u542f\u52a8\u8d85\u65f6");
		}
		return;
	}
	if (GetParent(teacherChildWindow) != teacherHostWindow) {
		teacherLaunchPending = true;
		AttachEmbeddedTeacherGui();
	}
	else LayoutEmbeddedTeacherGui();
}

void MainWindow::AppendNetworkResult(LPCWSTR text)
{
	HWND list = GetDlgItem(_hWnd, IDC_NET_RESULTS);
	if (!list) return;
	const LRESULT index = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text ? text : L""));
	SendMessageW(list, LB_SETTOPINDEX, index, 0);
}

void MainWindow::HandleNetworkCommand(int command)
{
	auto attack = JyUdpAttack::currentJyUdpAttack;
	if (!attack) { AppendNetworkResult(L"\u7f51\u7edc\u5de5\u5177\u672a\u521d\u59cb\u5316"); return; }
	if (command == IDC_NET_DETECT_PORT) {
		attack->CheckStudentMainTCPPort(_hWnd);
		AppendNetworkResult(L"\u6b63\u5728\u68c0\u6d4b StudentMain \u7aef\u53e3...");
		return;
	}
	if (command == IDC_NET_SCAN) {
		std::wstring all = NetUtils::GetIP(AF_INET);
		std::vector<std::wstring> localAddresses;
		SplitString(all, localAddresses, L"\n");
		if (localAddresses.empty()) { AppendNetworkResult(L"\u672a\u627e\u5230\u53ef\u7528\u7684 IPv4 \u5730\u5740"); return; }
		std::wstring current = localAddresses.front();
		const size_t dot = current.find_last_of(L'.');
		if (dot == std::wstring::npos) { AppendNetworkResult(L"IPv4 \u5730\u5740\u683c\u5f0f\u65e0\u6548"); return; }
		std::wstring host = current.substr(0, dot);
		std::wstring start = host + L".1";
		std::wstring end = host + L".254";
		SendMessageW(networkControls[14], LB_RESETCONTENT, 0, 0);
		AppendNetworkResult(L"\u6b63\u5728\u626b\u63cf\u5c40\u57df\u7f51...");
		attack->ScanNetworkIP(_hWnd, start, end, host, current);
		return;
	}
	auto read = [this](int id) {
		wchar_t buffer[1024]{};
		GetWindowTextW(GetDlgItem(_hWnd, id), buffer, _countof(buffer));
		return std::wstring(buffer);
	};
	std::wstring ipText = read(IDC_NET_IP);
	std::wstring portText = read(IDC_NET_PORT);
	std::vector<std::wstring> ips;
	SplitString(ipText, ips, L",");
	std::wregex ipv4(L"^(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3}$");
	if (ips.empty()) { AppendNetworkResult(L"\u8bf7\u8f93\u5165\u76ee\u6807 IP"); return; }
	for (const auto& ip : ips) if (!std::regex_match(ip, ipv4)) { AppendNetworkResult(L"\u76ee\u6807 IPv4 \u5730\u5740\u683c\u5f0f\u4e0d\u6b63\u786e"); return; }
	const int portValue = _wtoi(portText.c_str());
	if (portValue < 1 || portValue > 65535) { AppendNetworkResult(L"\u7aef\u53e3\u5fc5\u987b\u5728 1-65535 \u4e4b\u95f4"); return; }
	DWORD port = static_cast<DWORD>(portValue);
	std::wstring payload;
	if (command == IDC_NET_SEND_MESSAGE) payload = read(IDC_NET_MESSAGE);
	else if (command == IDC_NET_SEND_COMMAND) payload = read(IDC_NET_COMMAND);
	if ((command == IDC_NET_SEND_MESSAGE || command == IDC_NET_SEND_COMMAND) && payload.empty()) {
		AppendNetworkResult(L"\u8bf7\u5148\u8f93\u5165\u5185\u5bb9"); return;
	}
	if ((command == IDC_NET_SHUTDOWN || command == IDC_NET_REBOOT) &&
		MessageBoxW(_hWnd, L"\u786e\u5b9a\u5411\u76ee\u6807\u8bbe\u5907\u53d1\u9001\u8be5\u64cd\u4f5c\uff1f", L"Dzjs Trainer", MB_YESNO | MB_ICONWARNING) != IDYES) return;
	for (auto ip : ips) {
		if (command == IDC_NET_SEND_MESSAGE) attack->SendText(ip, port, payload);
		else if (command == IDC_NET_SEND_COMMAND) attack->SendCommand(ip, port, payload);
		else if (command == IDC_NET_SHUTDOWN) attack->SendShutdown(ip, port);
		else if (command == IDC_NET_REBOOT) attack->SendReboot(ip, port);
	}
	AppendNetworkResult(L"\u64cd\u4f5c\u5df2\u63d0\u4ea4");
}

void MainWindow::ShowPowerMenu()
{
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, CMD_POWER_SHUTDOWN, L"\u5173\u95ed\u8ba1\u7b97\u673a");
	AppendMenuW(menu, MF_STRING, CMD_POWER_RESTART, L"\u91cd\u65b0\u542f\u52a8\u8ba1\u7b97\u673a");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CMD_POWER_EXIT, L"\u9000\u51fa Dzjs Trainer");
	POINT point{};
	GetCursorPos(&point);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, _hWnd, nullptr);
	DestroyMenu(menu);
}

void MainWindow::ShowHelp()
{
	MessageBoxW(_hWnd,
		L"Dzjs Trainer \u4f1a\u81ea\u52a8\u68c0\u6d4b\u5e76\u63a7\u5236\u6781\u57df\u8fdb\u7a0b\u3002\n\n"
		L"\u53ef\u5728\u201c\u9632\u62a4\u7b56\u7565\u201d\u4e2d\u8c03\u6574\u8fdc\u7a0b\u64cd\u4f5c\u6743\u9650\uff0c\u5728\u201c\u8fd0\u884c\u65e5\u5fd7\u201d\u4e2d\u67e5\u770b\u5b9e\u65f6\u72b6\u6001\u3002",
		L"Dzjs Trainer \u5e2e\u52a9", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::ExportLogs()
{
	SYSTEMTIME now{};
	GetLocalTime(&now);
	wchar_t defaultName[128]{};
	swprintf_s(defaultName, L"DzjsTrainer-log-%04d%02d%02d-%02d%02d%02d.txt",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	std::wstring fileName = defaultName;
	if (!ChooseLogSavePath(_hWnd, &fileName)) return;

	std::wstring content = L"Dzjs Trainer log\r\n";
	content += L"============================================================\r\n";
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		for (const auto& entry : logs) {
			content += entry.text;
			content += L"\r\n";
		}
	}

	const int byteCount = WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
		static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
	std::vector<char> utf8(static_cast<size_t>(std::max(0, byteCount)));
	if (byteCount > 0) {
		WideCharToMultiByte(CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()),
			utf8.data(), byteCount, nullptr, nullptr);
	}

	HANDLE file = CreateFileW(fileName.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		ShowFastTip(L"\u5bfc\u51fa\u65e5\u5fd7\u5931\u8d25");
		return;
	}
	const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
	DWORD written = 0;
	BOOL success = WriteFile(file, bom, sizeof(bom), &written, nullptr);
	if (success && !utf8.empty()) {
		success = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
	}
	CloseHandle(file);
	ShowFastTip(success ? L"\u65e5\u5fd7\u5df2\u5bfc\u51fa" : L"\u5bfc\u51fa\u65e5\u5fd7\u5931\u8d25");
}

void MainWindow::LoadTemporaryVideoPreview()
{
	temporaryVideoPreview.reset();
	if (temporaryVideoMode == L"image") {
		if (temporaryVideoImagePath.empty()) return;
		std::unique_ptr<Image> image(Image::FromFile(temporaryVideoImagePath.c_str(), FALSE));
		if (image && image->GetLastStatus() == Ok && image->GetWidth() > 0 && image->GetHeight() > 0)
			temporaryVideoPreview = std::move(image);
		return;
	}
	if (temporaryVideoFilePath.empty()) return;
	const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	IShellItemImageFactory* imageFactory = nullptr;
	HBITMAP thumbnail = nullptr;
	if (SUCCEEDED(SHCreateItemFromParsingName(temporaryVideoFilePath.c_str(), nullptr,
		IID_PPV_ARGS(&imageFactory))) && imageFactory) {
		SIZE requestedSize{ 320, 180 };
		if (SUCCEEDED(imageFactory->GetImage(requestedSize,
			SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &thumbnail)) && thumbnail) {
			std::unique_ptr<Bitmap> image(Bitmap::FromHBITMAP(thumbnail, nullptr));
			if (image && image->GetLastStatus() == Ok) temporaryVideoPreview = std::move(image);
			DeleteObject(thumbnail);
		}
		imageFactory->Release();
	}
	if (SUCCEEDED(initialized)) CoUninitialize();
}

void MainWindow::ApplyTemporaryVideoProtection()
{
	// Persist media locally; the hook reloads this file and does not depend on
	// Windows-drive-colon command parsing.
	//
	// Use the ini the settings object actually uses. Rebuilding the path here from
	// a hard-coded file name diverges whenever the exe is renamed or started with
	// -f, and the keys would land in a file the rest of the app never reads.
	//
	// The write must not be skipped when the worker is not running: these keys are
	// the only record of the choice, and the hooks read them on their next load.
	LPCWSTR iniPath = JTAppGetIniPathDirect();
	if (iniPath && iniPath[0] != L'\0') {
		WritePrivateProfileStringW(L"JTSettings", L"EnableVideoModify", temporaryVideoProtection ? L"TRUE" : L"FALSE", iniPath);
		WritePrivateProfileStringW(L"JTSettings", L"VideoModifyImage", temporaryVideoImagePath.c_str(), iniPath);
		WritePrivateProfileStringW(L"JTSettings", L"VideoModifyVideo", temporaryVideoFilePath.c_str(), iniPath);
		WritePrivateProfileStringW(L"JTSettings", L"VideoModifyMode", temporaryVideoMode.c_str(), iniPath);
		WritePrivateProfileStringW(L"JTSettings", L"VideoModifyLoop", temporaryVideoLoop ? L"TRUE" : L"FALSE", iniPath);
	}
	if (!currentWorker) return;
	if (!temporaryVideoImagePath.empty()) {
		const std::wstring message = L"hk:setvideoimage:" + temporaryVideoImagePath;
		currentWorker->SendMessageToVirus(message.c_str());
	}
	if (!temporaryVideoFilePath.empty()) {
		const std::wstring message = L"hk:setvideofile:" + temporaryVideoFilePath;
		currentWorker->SendMessageToVirus(message.c_str());
	}
	currentWorker->SendMessageToVirus(temporaryVideoLoop ? L"hk:setvideoloop:1" : L"hk:setvideoloop:0");
	const std::wstring mode = L"hk:setvideomode:" + temporaryVideoMode;
	currentWorker->SendMessageToVirus(mode.c_str());
	currentWorker->SendMessageToVirus(temporaryVideoProtection ? L"hk:setvideo:1" : L"hk:setvideo:0");
}

bool MainWindow::ChooseTemporaryVideoMedia(bool image)
{
	std::wstring path;
	if (!ChooseMediaPath(_hWnd, image, &path)) return false;
	if (image) {
		temporaryVideoImagePath = std::move(path);
		temporaryVideoMode = L"image";
		temporaryVideoProtection = true;
		LoadTemporaryVideoPreview();
	}
	else {
		temporaryVideoFilePath = std::move(path);
		temporaryVideoMode = L"video";
		temporaryVideoProtection = true;
		LoadTemporaryVideoPreview();
	}
	ApplyTemporaryVideoProtection();
	ShowFastTip(image ? L"图片已选中，当前会话立即可用" : L"视频已选中，当前会话立即可用");
	InvalidateRect(_hWnd, nullptr, FALSE);
	return true;
}

void MainWindow::ShowFastTip(LPCWSTR text)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		toastText = StripMarkup(text);
		toastStart = GetTickCount64();
		toastUntil = toastStart + 4000;
	}
	EnsureAnimationTimer();
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

void MainWindow::OnUpdateStudentMainInfo(bool running, LPCWSTR fullPath, DWORD pid, bool)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		studentRunning = running;
		studentPid = running ? pid : 0;
		studentPath = running && fullPath ? fullPath : L"未检测到";
	}
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

void MainWindow::OnUpdateState(TrainerStatus status, LPCWSTR textMain, LPCWSTR textMore)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		currentStatus = status;
		currentControlled = status == TrainerStatusControlled || status == TrainerStatusControlledAndUnLocked;
		statusTitle = textMain ? textMain : L"未知状态";
		statusDetail = StripMarkup(textMore);
	}
	if (status == TrainerStatusControlled || status == TrainerStatusControlledAndUnLocked)
		ApplyTemporaryVideoProtection();
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

void MainWindow::OnResolveBlackScreenWindow() { ShowFastTip(L"\u5df2\u5173\u95ed\u6781\u57df\u9ed1\u5c4f\u7a97\u53e3"); }
void MainWindow::OnBeforeSendStartConf() { SetTimer(_hWnd, TIMER_RERUN, 1500, nullptr); }
void MainWindow::OnSimpleMessageCallback(LPCWSTR text) { ShowFastTip(text); }
void MainWindow::OnAllowGbTop() { setAllowGbTop = true; SaveSettings(); }
void MainWindow::OnShowHelp() { ShowHelp(); }

void MainWindow::LoadSettings()
{
	auto settings = JTAppGetSettingsDirect();
	// Start with the strongest supported topmost mode unless the user changes it.
	setTopMost = settings->GetSettingBool(L"TopMost", true);
	setAllowAllRunOp = settings->GetSettingBool(L"AllowAllRunOp", true);
	setAllowMonitor = settings->GetSettingBool(L"AllowMonitor", true);
	setAllowControl = settings->GetSettingBool(L"AllowControl", false);
	setAllowGbTop = settings->GetSettingBool(L"AllowGbTop", false);
	setProhibitKillProcess = settings->GetSettingBool(L"ProhibitKillProcess", true);
	setProhibitCloseWindow = settings->GetSettingBool(L"ProhibitCloseWindow", true);
	setBandAllRunOp = settings->GetSettingBool(L"BandAllRunOp", false);
	setDoNotShowTrayIcon = settings->GetSettingBool(L"DoNotShowTrayIcon", false);
	// These keys are written by ApplyTemporaryVideoProtection and are also what the
	// injected hooks read out of the same ini, so they have to be read back here.
	// Leaving them hard-coded made the page show defaults on every start while the
	// ini still held the saved values.
	temporaryVideoProtection = settings->GetSettingBool(L"EnableVideoModify", false);
	temporaryVideoMode = settings->GetSettingStr(L"VideoModifyMode", L"image", MAX_PATH);
	temporaryVideoImagePath = settings->GetSettingStr(L"VideoModifyImage", L"", MAX_PATH);
	temporaryVideoFilePath = settings->GetSettingStr(L"VideoModifyVideo", L"", MAX_PATH);
	temporaryVideoLoop = settings->GetSettingBool(L"VideoModifyLoop", true);
	temporaryVideoPreview.reset();
}

void MainWindow::SaveSettings()
{
	auto settings = JTAppGetSettingsDirect();
	settings->SetSettingBool(L"TopMost", setTopMost);
	settings->SetSettingBool(L"AllowAllRunOp", setAllowAllRunOp);
	settings->SetSettingBool(L"BandAllRunOp", setBandAllRunOp);
	settings->SetSettingBool(L"AllowGbTop", setAllowGbTop);
	settings->SetSettingBool(L"ProhibitKillProcess", setProhibitKillProcess);
	settings->SetSettingBool(L"ProhibitCloseWindow", setProhibitCloseWindow);
	settings->SetSettingBool(L"AllowMonitor", setAllowMonitor);
	settings->SetSettingBool(L"AllowControl", setAllowControl);
	if (currentWorker) {
		currentWorker->InitSettings();
		ApplyTemporaryVideoProtection();
	}
}

void MainWindow::SaveSettingsOnQuit()
{
	JTAppGetSettingsDirect()->SetSettingBool(L"TopMost", setTopMost);
	JTAppGetSettingsDirect()->SetSettingBool(L"AllowGbTop", setAllowGbTop);
}

void MainWindow::OnFirstShow()
{
	if (!firstShow) return;
	firstShow = false;
	hotkeyShowHide = GlobalAddAtomW(L"DzjsTrainerShowHide");
	hotkeySwFull = GlobalAddAtomW(L"DzjsTrainerFakeFull");
	UINT mod = 0, key = 0;
	auto settings = JTAppGetSettingsDirect();
	SysHlp::HotKeyCtlToKeyCode(settings->GetSettingInt(L"HotKeyShowHide", 1604), &mod, &key);
	RegisterHotKey(_hWnd, hotkeyShowHide, mod, key);
	SysHlp::HotKeyCtlToKeyCode(settings->GetSettingInt(L"HotKeyFakeFull", 1606), &mod, &key);
	RegisterHotKey(_hWnd, hotkeySwFull, mod, key);
	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	CreateTrayIcon();
	SetTimer(_hWnd, TIMER_TRAY_REPAIR, 2000, nullptr);
	hMenuTray = LoadMenuW(JTAppGetInstanceDirect(), MAKEINTRESOURCEW(IDR_MAINMENU));
	if (hMenuTray) {
		HMENU trayPopup = GetSubMenu(hMenuTray, 0);
		if (trayPopup) {
			// The legacy RC menu text was saved with the wrong code page. Replace
			// those labels at runtime with real wide strings before the menu is used.
			ModifyMenuW(trayPopup, IDM_SHOWMAIN, MF_BYCOMMAND | MF_STRING, IDM_SHOWMAIN, L"显示主窗口");
			ModifyMenuW(trayPopup, IDM_HELP, MF_BYCOMMAND | MF_STRING, IDM_HELP, L"帮助");
			ModifyMenuW(trayPopup, IDM_EXIT, MF_BYCOMMAND | MF_STRING, IDM_EXIT, L"退出程序");
		}
	}
	if (setTopMost) {
		bool bandApplied = false;
		if (!ApplyHighestPermittedTopmost(_hWnd, true, &bandApplied)) {
			setTopMost = false;
			topMostUiAccessBand = false;
		}
		else {
			topMostUiAccessBand = bandApplied;
		}
	}
	if (currentWorker) {
		currentWorker->Init();
		currentWorker->Start();
		advController = currentWorker->Running();
		currentLogger->LogInfo(L"Native controller started");
	}
	else {
		// Config/update windows can enter the UI without creating the worker.
		advController = false;
	}
}

void MainWindow::OnWmCommand(WPARAM wParam)
{
	const int command = LOWORD(wParam);
	if (command == IDC_TEACHER_STUDENTS && HIWORD(wParam) == LBN_SELCHANGE) {
		if (teacherBackend) {
			const int selected = static_cast<int>(SendMessageW(teacherStudentList, LB_GETCURSEL, 0, 0));
			teacherBackend->selected_ip = selected >= 0 && selected < static_cast<int>(teacherDisplayedIps.size())
				? teacherDisplayedIps[static_cast<size_t>(selected)] : std::string{};
			teacherBackend->latest_preview.clear();
			teacherBackend->displayed_preview.clear();
			teacherBackend->preview_bitmap.reset();
			teacherBackend->remote_bitmap.reset();
			teacherBackend->remote_sequence = 0;
		}
		LayoutTeacherControls();
		InvalidateRect(_hWnd, nullptr, FALSE);
		return;
	}
	if (command == IDC_NET_TEACHER) {
		LaunchEmbeddedTeacherGui();
		return;
	}
	if ((command >= IDC_TEACHER_REFRESH && command <= IDC_TEACHER_REBOOT) || command == IDC_TEACHER_VIEW || command == IDC_TEACHER_CHAT_SEND) {
		HandleTeacherCommand(command);
		return;
	}
	switch (command) {
	case IDM_SHOWMAIN:
		ShowWindow(_hWnd, IsWindowVisible(_hWnd) ? SW_HIDE : SW_SHOW);
		if (IsWindowVisible(_hWnd)) SetForegroundWindow(_hWnd);
		break;
	case IDM_HELP:
		ShowWindow(_hWnd, SW_SHOW);
		SelectPage(Page::Help);
		SetForegroundWindow(_hWnd);
		break;
	case IDM_EXIT:
	case CMD_POWER_EXIT: {
		if (!ConfirmExit(_hWnd, darkMode)) break;
		if (currentLogger) currentLogger->LogInfo(L"Exit requested from tray or power menu");
		isUserCancel = true;
		const BOOL cleanupCompleted = JTAppRequestRepositoryCleanupDirect();
		if (!cleanupCompleted && currentLogger)
			currentLogger->LogWarn(L"RepositoryCleanup: 用户退出清理未完全成功");
		DestroyWindow(_hWnd);
		break; }
	case CMD_POWER_SHUTDOWN:
		if (MessageBoxW(_hWnd, L"\u786e\u5b9a\u5173\u95ed\u8ba1\u7b97\u673a\uff1f", L"Dzjs Trainer", MB_YESNO | MB_ICONWARNING) == IDYES)
			ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0);
		break;
	case CMD_POWER_RESTART:
		if (MessageBoxW(_hWnd, L"\u786e\u5b9a\u91cd\u65b0\u542f\u52a8\u8ba1\u7b97\u673a\uff1f", L"Dzjs Trainer", MB_YESNO | MB_ICONWARNING) == IDYES)
			ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
		break;
	default:
		if (command >= IDC_NET_DETECT_PORT && command <= IDC_NET_SCAN)
			HandleNetworkCommand(command);
		break;
	}
}

void MainWindow::EnsureAnimationTimer()
{
	if (!_hWnd || animationTimerRunning) return;
	lastAnimationTick = GetTickCount64();
	animationTimerRunning = SetTimer(_hWnd, TIMER_ANIMATION, 16, nullptr) != 0;
}

void MainWindow::OnWmTimer(WPARAM timer)
{
	if (timer == TIMER_SERVICE_POLL) {
		PollEmbeddedTeacherGui();
		PollTeacherService();
		return;
	}
	if (timer == TIMER_ANIMATION) {
		// 按真实时间推进，而不是每 tick 固定加一个量：定时器抖动时动画速度不变，
		// 从别的窗口切回来也不会突然"跳"一大步。
		const ULONGLONG now = GetTickCount64();
		const double delta = static_cast<double>(now - lastAnimationTick) / 1000.0;
		lastAnimationTick = now;
		const double dt = (std::max)(0.001, (std::min)(0.05, delta));
		bool needsPaint = false;

		auto approach = [dt](float& value, float target, double timeConstant) {
			const float k = static_cast<float>(1.0 - std::exp(-dt / timeConstant));
			const float diff = target - value;
			if (std::fabs(diff) <= 0.002f) {
				const bool changed = value != target;
				value = target;
				return changed;
			}
			value += diff * k;
			return true;
		};

		if (pageMotion < 1.0) {
			pageMotion = (std::min)(1.0, pageMotion + dt / 0.26);
			needsPaint = true;
		}
		if (approach(navIndicator, static_cast<float>(static_cast<int>(page)), 0.075)) needsPaint = true;

		// 开关滑块还在路上就继续重绘；具体步进由 AnimateToggle 用 frameDelta 算。
		if (toggleAnimating) {
			toggleAnimating = false;
			needsPaint = true;
		}

		for (int i = 0; i < static_cast<int>(hoverProgress.size()); ++i) {
			const float target = hoverTarget == i ? 1.0f : 0.0f;
			if (approach(hoverProgress[i], target, 0.055)) needsPaint = true;
		}

		// 更新弹窗：入场淡入 + 进度条追赶目标值。目标由下载线程报回来，
		// 这里只做插值，进度条就不会一格一格地跳。
		if (UpdateDialogVisible()) {
			if (approach(updateDialog.appear, 1.0f, 0.06)) needsPaint = true;
			const float targetProgress = updateDialog.percent < 0
				? updateDialog.progress
				: (std::min)(1.0f, static_cast<float>(updateDialog.percent) / 100.0f);
			if (approach(updateDialog.progress, targetProgress, 0.09)) needsPaint = true;
			updateDialog.sweep += static_cast<float>(dt) * 0.55f;
			if (updateDialog.sweep >= 1.0f) updateDialog.sweep -= 1.0f;
			// 总长度未知时没有别的动画源，得自己续上重绘，否则高光停在原地。
			if (updateDialog.mode == UpdateDialogMode::Download && updateDialog.percent < 0) needsPaint = true;
		}

		const bool toastAlive = !toastText.empty() && now <= toastUntil;
		if (!toastText.empty() && !toastAlive) {
			toastText.clear();
			needsPaint = true;
		}
		else if (toastAlive) {
			// 淡入和淡出都要逐帧重绘，最后 260 ms 才算结束。
			if (now < toastStart + 130 || now + 260 >= toastUntil) needsPaint = true;
		}

		if (!needsPaint) {
			// 没有任何动画在跑，停掉定时器；下次有交互动效时再开。
			KillTimer(_hWnd, TIMER_ANIMATION);
			animationTimerRunning = false;
			return;
		}
		if (IsWindowVisible(_hWnd) && !IsIconic(_hWnd))
			InvalidateRect(_hWnd, nullptr, FALSE);
	}
	else if (timer == TIMER_RERUN) {
		KillTimer(_hWnd, TIMER_RERUN);
		if (currentWorker) currentWorker->RunOperation(TrainerWorkerOp1);
	}
	else if (timer == TIMER_AUTO_SHUT && --autoShutSec < 0) {
		KillTimer(_hWnd, TIMER_AUTO_SHUT);
		ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, 0);
	}
	else if (timer == TIMER_TRAY_REPAIR) {
		EnsureTrayIcon();
	}
	else if (timer == TIMER_UPDATE_RESTART) {
		// 「更新完成」露过一帧了，可以退场：新版本实例正等着互斥体接手。
		KillTimer(_hWnd, TIMER_UPDATE_RESTART);
		DestroyWindow(_hWnd);
	}
}

void MainWindow::OnWmHotKey(WPARAM id)
{
	if (id == static_cast<WPARAM>(hotkeyShowHide)) SendMessageW(_hWnd, WM_COMMAND, IDM_SHOWMAIN, 0);
	else if (id == static_cast<WPARAM>(hotkeySwFull) && currentWorker) currentWorker->SwitchFakeFull();
}

void MainWindow::OnWmUser(WPARAM, LPARAM lParam)
{
	if (lParam == WM_LBUTTONDBLCLK) SendMessageW(_hWnd, WM_COMMAND, IDM_SHOWMAIN, 0);
	else if (lParam == WM_RBUTTONDOWN && hMenuTray) {
		POINT point{};
		GetCursorPos(&point);
		SetForegroundWindow(_hWnd);
		TrackPopupMenu(GetSubMenu(hMenuTray, 0), TPM_RIGHTBUTTON, point.x, point.y, 0, _hWnd, nullptr);
	}
}

void MainWindow::OnWmDestroy()
{
	if (currentLogger) currentLogger->LogInfo(L"Main window destroy started");
	StopAvProcessScan();
	StopEmbeddedTeacherGui();
	StopTeacherService();
	RemoveInputOriginHooks();
	SaveSettingsOnQuit();
	KillTimer(_hWnd, TIMER_ANIMATION);
	KillTimer(_hWnd, TIMER_SERVICE_POLL);
	animationTimerRunning = false;
	KillTimer(_hWnd, TIMER_TRAY_REPAIR);
	UnregisterHotKey(_hWnd, hotkeyShowHide);
	UnregisterHotKey(_hWnd, hotkeySwFull);
	if (trayIconRegistered) Shell_NotifyIconW(NIM_DELETE, &nid);
	trayIconRegistered = false;
	if (currentLogger) currentLogger->LogInfo(L"Main window destroy completed; leaving UI message loop");
	PostQuitMessage(0);
	_hWnd = nullptr;
}

void MainWindow::CreateTrayIcon()
{
	if (setDoNotShowTrayIcon || !_hWnd) return;
	nid.cbSize = sizeof(nid);
	nid.hWnd = _hWnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_USER;
	nid.hIcon = LoadIconW(JTAppGetInstanceDirect(), MAKEINTRESOURCEW(IDI_APP));
	wcscpy_s(nid.szTip, L"Dzjs Trainer");
	// Re-adding the same window/id is intentional: Explorer removes all
	// notification items when it restarts, and hostile software can remove an
	// item without affecting the owning process.
	trayIconRegistered = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
	if (trayIconRegistered) {
		Shell_NotifyIconW(NIM_MODIFY, &nid);
	}
}

void MainWindow::EnsureTrayIcon()
{
	if (setDoNotShowTrayIcon || !_hWnd) return;
	nid.cbSize = sizeof(nid);
	nid.hWnd = _hWnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_USER;
	nid.hIcon = LoadIconW(JTAppGetInstanceDirect(), MAKEINTRESOURCEW(IDI_APP));
	wcscpy_s(nid.szTip, L"Dzjs Trainer");
	if (trayIconRegistered && Shell_NotifyIconW(NIM_MODIFY, &nid)) return;
	trayIconRegistered = false;
	CreateTrayIcon();
}

void MainWindow::ShowTrayBalloon(LPCWSTR title, LPCWSTR text)
{
	if (setDoNotShowTrayIcon) return;
	nid.uFlags = NIF_INFO;
	nid.dwInfoFlags = NIIF_NONE;
	wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
	wcsncpy_s(nid.szInfo, text, _TRUNCATE);
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void MainWindow::LogCallBack(const wchar_t* str, LogLevel level, LPARAM data)
{
	auto self = reinterpret_cast<MainWindow*>(data);
	if (self) self->WriteLogItem(str, level);
}

void MainWindow::WriteLogItem(const wchar_t* str, LogLevel level)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		logs.push_back({ str ? str : L"", level });
		if (logs.size() > 200) logs.erase(logs.begin(), logs.begin() + 50);
	}
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

int MainWindow::RunLoop()
{
	if (!isValid()) return -1;
	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return static_cast<int>(message.wParam);
}

void MainWindow::Close()
{
	if (_hWnd) DestroyWindow(_hWnd);
}
