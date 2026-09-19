#include "stdafx.h"
#include "MainWindow.h"
#include "../WindowCaptureProtection.h"
#include "DriverServiceActions.h"
#include "resource.h"
#include "DzjsTrainerUI.h"
#include "../DzjsTrainerUpdater/DzjsTrainerUpdater.h"
#include "../DzjsTrainer/AppPublic.h"

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

DWORD WINAPI UpdateProbeThread(LPVOID parameter)
{
	UpdateProbeContext* context = static_cast<UpdateProbeContext*>(parameter);
	if (!context) return 0;
	// 新建线程会**继承创建线程的令牌**，包括模拟（impersonate）状态。主程序在提升
	// 置顶权限时曾模拟 SYSTEM，一旦这个线程继承了模拟令牌，它调用 CreateProcess
	// 就会失败并返回 ERROR_BAD_IMPERSONATION_LEVEL(1346)。
	// 本线程只做只读探测、不需要任何模拟身份，先撤掉。线程用完即退，无需恢复。
	RevertToSelf();
	// 用更新器代取清单：主程序（GUI 进程）里直接调 WinINet 会 ERROR_INTERNET_CANNOT_CONNECT，
	// 而更新器是控制台进程，同样的调用正常。详见 JUpdater_ProbeManifest 的说明。
	context->ok = JUpdater_ProbeManifest(&context->manifest) != FALSE;
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

bool ApplyHighestPermittedTopmost(HWND window, bool pinned, bool* uiAccessBandApplied = nullptr)
{
	if (uiAccessBandApplied) *uiAccessBandApplied = false;
	if (!window || !IsWindow(window)) return false;
	constexpr DWORD kWindowBandDefault = 0;
	constexpr DWORD kWindowBandUiAccess = 2;
	if (IsCurrentProcessUiAccessEnabled()) {
		if (SetWindowBandFunction setWindowBand = ResolveSetWindowBand()) {
			const bool applied = setWindowBand(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
				pinned ? kWindowBandUiAccess : kWindowBandDefault) != FALSE;
			if (uiAccessBandApplied) *uiAccessBandApplied = applied;
		}
	}
	if (!SetWindowPos(window, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return false;
	if (pinned) BringWindowToTop(window);
	return true;
}
constexpr UINT WM_COPYGLOBALDATA_COMPAT = 0x0049;
constexpr UINT TIMER_ANIMATION = 20;
constexpr UINT TIMER_TRAY_REPAIR = 21;
constexpr UINT TIMER_RERUN = 21;
constexpr UINT TIMER_AUTO_SHUT = 22;
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
std::array<bool, 10> materialIconGlyphs{};
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

void DrawNativeButton(const DRAWITEMSTRUCT* item, HFONT font, bool dark)
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
	default: g.DrawEllipse(&pen, X(3), Y(3), 14 * s, 14 * s); break;
	}
}

void Text(Graphics& g, const wchar_t* value, float x, float y, float width, float height,
	float size, FontStyle style, const Color& color, StringAlignment align = StringAlignmentNear) {
	Font font(L"Microsoft YaHei UI", size, style, UnitPixel);
	SolidBrush brush(color);
	StringFormat format;
	format.SetAlignment(align);
	format.SetLineAlignment(StringAlignmentNear);
	format.SetTrimming(StringTrimmingEllipsisCharacter);
	g.DrawString(value ? value : L"", -1, &font, RectF(x, y, width, height), &format, &brush);
}

bool Hit(const RECT& rect, POINT point) {
	return PtInRect(&rect, point) != FALSE;
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
	Text(graphics, L"仅接受真实键盘按键，粘贴或程序模拟输入不会通过验证。", panelX + 34, panelY + 178, 452, 22, 10, FontStyleRegular,
		C(context->darkMode, 91, 70, 20, 232, 211, 162), StringAlignmentCenter);
	if (context->invalidInput) {
		Text(graphics, L"输入不匹配或不是直接键盘输入，请重试。", panelX + 34, panelY + 204, 452, 22, 10, FontStyleBold,
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
	if (controlFont) DeleteObject(controlFont);
	delete backBuffer;
	backBuffer = nullptr;
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
	UnloadMaterialIconFont();
	if (gdiplusToken) GdiplusShutdown(gdiplusToken);
	if (currentMainWindow == this) currentMainWindow = nullptr;
}

bool MainWindow::RegisterWindowClass()
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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

	SetTimer(_hWnd, TIMER_ANIMATION, 16, nullptr);
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
		JiYuWindowCapture::ExcludeWindowFromCapture(hwnd);
	}
	if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
	if (self->WM_TASKBARCREATED != 0 && message == static_cast<UINT>(self->WM_TASKBARCREATED)) {
		self->CreateTrayIcon();
		return 0;
	}

	switch (message) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: self->Paint(); return 0;
	case WM_SIZE: self->LayoutEmbeddedWindows(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
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
	case WM_LBUTTONUP: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		self->HandleClick(point);
		return 0;
	}
	case WM_TIMER: self->OnWmTimer(wParam); return 0;
	case WM_COMMAND: self->OnWmCommand(wParam); return 0;
	case WM_DRAWITEM: {
		auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
		if (item && ((item->CtlID >= IDC_NET_DETECT_PORT && item->CtlID <= IDC_NET_SCAN) || item->CtlID == IDC_NET_TEACHER ||
			(item->CtlID >= IDC_TEACHER_REFRESH && item->CtlID <= IDC_TEACHER_REBOOT) || item->CtlID == IDC_TEACHER_VIEW || item->CtlID == IDC_TEACHER_CHAT_SEND)) {
			DrawNativeButton(item, self->controlFont, self->darkMode);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX: {
		HDC controlDc = reinterpret_cast<HDC>(wParam);
		SetTextColor(controlDc, self->darkMode ? RGB(222, 230, 226) : RGB(35, 42, 39));
		SetBkColor(controlDc, self->darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
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

void MainWindow::Paint()
{
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(_hWnd, &ps);
	RECT client{};
	GetClientRect(_hWnd, &client);
	const int width = client.right;
	const int height = client.bottom;
	if (width <= 0 || height <= 0) {
		EndPaint(_hWnd, &ps);
		return;
	}
	if (!backBuffer || backBufferWidth != width || backBufferHeight != height) {
		delete backBuffer;
		backBuffer = new Bitmap(width, height, PixelFormat32bppPARGB);
		backBufferWidth = width;
		backBufferHeight = height;
	}
	Graphics g(backBuffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	// ClearType assumes an opaque target; the ARGB back buffer needs alpha-safe antialiasing.
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
	g.Clear(C(darkMode, 246, 248, 246, 17, 20, 19));

	PaintNavigation(g, height);
	PaintHeader(g, width);
	const double easedMotion = 1.0 - pow(1.0 - pageMotion, 3.0);
	const int offset = static_cast<int>((1.0 - easedMotion) * 18.0);
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
	PaintToast(g, width);
	Graphics target(dc);
	target.DrawImage(backBuffer, 0, 0);
	EndPaint(_hWnd, &ps);
}

void MainWindow::PaintNavigation(Graphics& g, int height)
{
	const bool d = darkMode;
	SolidBrush side(C(d, 235, 241, 238, 27, 33, 31));
	g.FillRectangle(&side, 0, 0, 220, height);

	FillRound(g, RectF(20, 18, 44, 44), 8, C(d, 0, 107, 95, 130, 213, 199));
	Text(g, L"J", 20, 24, 44, 32, 21, FontStyleBold, C(d, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
	Text(g, L"Dzjs Trainer", 76, 21, 128, 24, 15, FontStyleBold, C(d, 26, 29, 28, 226, 232, 229));
	Text(g, L"\u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3", 76, 44, 128, 18, 10, FontStyleRegular, C(d, 67, 72, 70, 191, 200, 196));
	Text(g, L"\u5de5\u4f5c\u533a", 24, 94, 150, 18, 10, FontStyleBold, C(d, 91, 97, 94, 164, 174, 169));

	const int navIcons[] = { 0, 1, 5, 5, 6, 3, 0, 2, 3, 6 };
	for (int i = 0; i < 10; ++i) {
		const float y = 110.0f + i * 45.0f;
		navRects[i] = { 12, static_cast<LONG>(y), 208, static_cast<LONG>(y + 40) };
		const bool active = static_cast<int>(page) == i;
		if (active) {
			FillRound(g, RectF(12, y, 196, 40), 20, C(d, 183, 232, 222, 42, 79, 72));
		}
		else if (hoverProgress[i] > 0.002f) {
			FillRound(g, RectF(12, y, 196, 40), 20,
				Blend(C(d, 235, 241, 238, 27, 33, 31), C(d, 220, 229, 225, 39, 47, 44), hoverProgress[i]));
		}
		const Color navColor = active ? C(d, 0, 81, 72, 156, 240, 225) : C(d, 58, 64, 61, 197, 207, 202);
		DrawIcon(g, navIcons[i], 30, y + 10, navColor, 18);
		Text(g, kNavLabels[i], 66, y + 9, 126, 22, 10, active ? FontStyleBold : FontStyleRegular, navColor);
	}

	FillRound(g, RectF(16, static_cast<float>(height - 82), 188, 62), 8, C(d, 221, 231, 226, 34, 42, 39));
	SolidBrush online(C(d, 0, 107, 95, 130, 213, 199));
	g.FillEllipse(&online, 32, height - 59, 10, 10);
	Text(g, L"\u9632\u62a4\u670d\u52a1\u5df2\u5c31\u7eea", 54, static_cast<float>(height - 66), 136, 20, 10, FontStyleBold, C(d, 35, 42, 39, 222, 230, 226));
	Text(g, NavVersionLine().c_str(), 54, static_cast<float>(height - 45), 136, 16, 9, FontStyleRegular, C(d, 91, 99, 95, 166, 177, 171));
}

void MainWindow::PaintHeader(Graphics& g, int width)
{
	const int pageIndex = static_cast<int>(page);
	const bool topMostEnabled = setTopMost && IsWindowActuallyTopmost(_hWnd);
	const wchar_t* topMostText = !topMostEnabled
		? L"\u7f6e\u9876\uff1a\u672a\u5f00\u542f"
		: (topMostUiAccessBand ? L"\u8d85\u7ea7\u7f6e\u9876\uff1a\u5df2\u5f00\u542f" : L"\u666e\u901a\u7f6e\u9876\uff1a\u5df2\u5f00\u542f");
	Text(g, kPageTitles[pageIndex], 248, 22, 430, 34, 23, FontStyleBold, C(darkMode, 28, 32, 30, 227, 233, 230));
	Text(g, page == Page::Overview ? L"\u5f53\u524d\u8bbe\u5907\u7684\u8fd0\u884c\u4e0e\u9632\u62a4\u72b6\u6001" : L"Dzjs Trainer  \u00b7  \u672c\u673a", 248, 55, 440, 18, 10,
		FontStyleRegular, C(darkMode, 86, 92, 89, 174, 184, 179));

	superTopMostRect = { width - 214, 20, width - 88, 68 };
	const float superX = static_cast<float>(width - 208);
	const bool superHover = hoverTarget == 80;
	FillRound(g, RectF(superX, 26, 118, 36), 18,
		topMostEnabled ? C(darkMode, 185, 226, 217, 43, 76, 69) :
			(superHover ? C(darkMode, 220, 237, 231, 49, 63, 58) : C(darkMode, 237, 242, 239, 31, 37, 34)));
	Text(g, topMostText, superX, 36, 118, 18, 10, FontStyleBold,
		topMostEnabled ? C(darkMode, 0, 81, 72, 155, 239, 224) : C(darkMode, 42, 48, 45, 220, 229, 225), StringAlignmentCenter);
	themeRect = { width - 76, 20, width - 28, 68 };
	const bool themeHover = hoverTarget == 41;
	if (themeHover) FillRound(g, RectF(static_cast<float>(width - 76), 20, 48, 48), 24, C(darkMode, 226, 233, 229, 39, 47, 44));
	DrawIcon(g, darkMode ? 8 : 9, static_cast<float>(width - 62), 34, C(darkMode, 52, 59, 56, 205, 215, 210), 20);
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
	FillRound(g, RectF(contentX, top, contentW, 170), 8, C(darkMode, 219, 239, 233, 28, 55, 50));
	FillRound(g, RectF(contentX + 24, top + 22, 96, 28), 14, C(darkMode, 185, 226, 217, 43, 76, 69));
	Text(g, L"\u9632\u62a4\u4e2d", contentX + 24, top + 29, 96, 18, 10, FontStyleBold, C(darkMode, 0, 81, 72, 155, 239, 224), StringAlignmentCenter);
	Text(g, statusTitle.c_str(), contentX + 24, top + 64, contentW - 230, 38, 23, FontStyleBold, C(darkMode, 25, 35, 32, 225, 236, 232));
	Text(g, statusDetail.c_str(), contentX + 24, top + 108, contentW - 230, 38, 11, FontStyleRegular, C(darkMode, 65, 79, 74, 178, 197, 190));

	const float badgeX = contentX + contentW - 158;
	const float badgeY = top + 27;
	FillRound(g, RectF(badgeX, badgeY, 126, 116), 8, primary);
	DrawIcon(g, 1, badgeX + 48, badgeY + 20, onPrimary, 30);
	Text(g, studentRunning ? L"\u5df2\u8fde\u63a5" : L"\u5f85\u68c0\u6d4b", badgeX + 12, badgeY + 60, 102, 25, 14, FontStyleBold, onPrimary, StringAlignmentCenter);
	Text(g, studentRunning ? L"StudentMain" : L"\u8fdb\u7a0b\u672a\u8fd0\u884c", badgeX + 12, badgeY + 88, 102, 18, 9, FontStyleRegular, onPrimary, StringAlignmentCenter);

	const float gap = 12.0f;
	const float cardW = (contentW - gap * 2) / 3.0f;
	const float metricsY = top + 186;
	for (int i = 0; i < 3; ++i) {
		const float x = contentX + i * (cardW + gap);
		FillRound(g, RectF(x, metricsY, cardW, 94), 8, C(darkMode, 255, 255, 255, 28, 33, 31));
		StrokeRound(g, RectF(x, metricsY, cardW, 94), 8, C(darkMode, 198, 203, 200, 67, 75, 71));
	}
	Text(g, L"\u9632\u62a4\u72b6\u6001", contentX + 18, metricsY + 15, cardW - 32, 18, 10, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, currentControlled ? L"\u5b8c\u6574\u9632\u62a4" : L"\u6b63\u5728\u76d1\u63a7", contentX + 18, metricsY + 42, cardW - 32, 28, 16, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	wchar_t pidText[64];
	swprintf_s(pidText, L"PID  %lu", static_cast<unsigned long>(studentPid));
	Text(g, L"\u5b66\u751f\u7aef\u8fdb\u7a0b", contentX + cardW + gap + 18, metricsY + 15, cardW - 32, 18, 10, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, studentRunning ? pidText : L"\u672a\u8fd0\u884c", contentX + cardW + gap + 18, metricsY + 42, cardW - 32, 28, 16, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	Text(g, L"\u68c0\u6d4b\u8def\u5f84", contentX + (cardW + gap) * 2 + 18, metricsY + 15, cardW - 32, 18, 10, FontStyleRegular, C(darkMode, 91, 97, 94, 169, 179, 174));
	Text(g, studentPath.c_str(), contentX + (cardW + gap) * 2 + 18, metricsY + 40, cardW - 34, 38, 10, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));

	const float actionsY = metricsY + 110;
	FillRound(g, RectF(contentX, actionsY, contentW, static_cast<float>(height) - actionsY - 20), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	Text(g, L"\u5feb\u6377\u64cd\u4f5c", contentX + 20, actionsY + 16, 180, 24, 14, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	Text(g, L"\u5e38\u7528\u7684\u8fd0\u884c\u63a7\u5236\u548c\u8bbe\u5907\u5de5\u5177", contentX + 20, actionsY + 42, 300, 18, 9, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	const wchar_t* labels[] = { L"\u9632\u62a4\u72b6\u6001", setTopMost ? L"\u53d6\u6d88\u7f6e\u9876" : L"\u5f00\u542f\u7f6e\u9876", studentRunning ? L"\u7ed3\u675f\u6781\u57df" : L"\u91cd\u542f\u6781\u57df", L"\u63a7\u5236\u4e0e\u8bbe\u7f6e", L"\u7535\u6e90\u63a7\u5236" };
	const float actionGap = 10.0f;
	const float actionW = (contentW - 40 - actionGap * 4) / 5.0f;
	for (int i = 0; i < 5; ++i) {
		const float x = contentX + 20 + i * (actionW + actionGap);
		const float y = actionsY + 70;
		actionRects[i] = { static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + actionW), static_cast<LONG>(y + 68) };
		const float hover = hoverProgress[10 + i];
		const Color base = i == 4 ? C(darkMode, 255, 248, 231, 47, 42, 30) : C(darkMode, 255, 255, 255, 37, 44, 41);
		const Color over = i == 4 ? C(darkMode, 247, 232, 197, 64, 55, 34) : C(darkMode, 220, 237, 231, 49, 63, 58);
		FillRound(g, RectF(x, y, actionW, 68), 8, Blend(base, over, hover));
		const Color iconColor = i == 4 ? C(darkMode, 121, 86, 0, 241, 194, 98) : primary;
		DrawIcon(g, i == 0 ? 1 : i == 1 ? 4 : i == 2 ? 5 : i == 3 ? 6 : 7, x + 14, y + 23, iconColor, 20);
		Text(g, labels[i], x + 44, y + 21, actionW - 52, 28, 10, FontStyleBold, C(darkMode, 42, 48, 45, 220, 229, 225));
	}
}

void MainWindow::PaintProtection(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const bool replacementPage = page == Page::Replacement;
	FillRound(g, RectF(x, y, w, static_cast<float>(height) - y - 20), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	if (!replacementPage) {
	Text(g, L"\u884c\u4e3a\u63a7\u5236", x + 24, y + 20, 200, 28, 16, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	Text(g, L"\u9009\u62e9\u5141\u8bb8\u7684\u8fdc\u7a0b\u884c\u4e3a\uff0c\u66f4\u6539\u5c06\u7acb\u5373\u5e94\u7528", x + 24, y + 50, 440, 20, 10, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	bool values[] = { !setAllowAllRunOp, setBandAllRunOp, setAllowGbTop, setProhibitKillProcess, setAllowMonitor, setProhibitCloseWindow, setAllowControl };
	const float cardW = (w - 62) / 2.0f;
	for (int i = 0; i < 7; ++i) {
		const int col = i % 2;
		const int row = i / 2;
		const float cardX = x + 24 + col * (cardW + 14);
		const float cardY = y + 88 + row * 76;
		toggleRects[i] = { static_cast<LONG>(cardX), static_cast<LONG>(cardY), static_cast<LONG>(cardX + cardW), static_cast<LONG>(cardY + 62) };
		FillRound(g, RectF(cardX, cardY, cardW, 62), 8,
			Blend(C(darkMode, 255, 255, 255, 38, 45, 42), C(darkMode, 223, 238, 232, 48, 60, 56), hoverProgress[20 + i]));
		Text(g, kToggleLabels[i], cardX + 16, cardY + 20, cardW - 94, 25, 11, FontStyleRegular, C(darkMode, 43, 49, 46, 219, 228, 224));
		FillRound(g, RectF(cardX + cardW - 62, cardY + 18, 44, 26), 13, values[i] ? C(darkMode, 0, 107, 95, 130, 213, 199) : C(darkMode, 116, 124, 120, 97, 108, 103));
		SolidBrush knob(values[i] ? C(darkMode, 255, 255, 255, 0, 55, 49) : C(darkMode, 245, 247, 245, 219, 225, 222));
		g.FillEllipse(&knob, RectF(cardX + cardW - (values[i] ? 40.0f : 58.0f), cardY + 22, 18.0f, 18.0f));
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
		FillRound(g, RectF(buttonX, buttonY, 126, 36), 18, C(darkMode, 246, 221, 214, 52, 50, 48));
		Text(g, L"\u5378\u8f7d\u4e3b\u9a71\u52a8", buttonX, buttonY + 10, 126, 18, 9, FontStyleBold, C(darkMode, 145, 45, 35, 230, 155, 140), StringAlignmentCenter);
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
	FillRound(g, RectF(x + 24, panelY, w - 48, panelH), 8, C(darkMode, 255, 255, 255, 38, 45, 42));

	Text(g, page == Page::Replacement ? L"\u66ff\u6362\u753b\u9762" : L"\u4fe1\u606f\u6d41\u4fdd\u62a4", x + 42, panelY + 12, 200, 24, 13, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	Text(g, L"开启后教师端看到你选的图片/视频；未选媒体时为纯黑。", x + 42, panelY + 46, 420, 17, 9, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	// Keep the main-driver action visible on the same protection surface.
	driverUnloadRect = {};

	advancedSettingsRect = { static_cast<LONG>(x + w - 250), static_cast<LONG>(panelY + 12), static_cast<LONG>(x + w - 144), static_cast<LONG>(panelY + 44) };
	FillRound(g, RectF(x + w - 250, panelY + 12, 106, 32), 16, C(darkMode, 219, 238, 232, 43, 76, 69));
	Text(g, L"高级设置", x + w - 250, panelY + 20, 106, 18, 9, FontStyleBold, C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);

	const bool imageMode = temporaryVideoMode == L"image" || temporaryVideoMode == L"photo";
	const float controlX = x + 42;

	// 左列 · 信息流保护开关（原先在右上角，被预览图整块盖住，用户看不到）
	Text(g, L"信息流保护", controlX, panelY + 78, 80, 18, 10, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	temporaryVideoEnableRect = { static_cast<LONG>(controlX + 86), static_cast<LONG>(panelY + 72), static_cast<LONG>(controlX + 178), static_cast<LONG>(panelY + 100) };
	FillRound(g, RectF(controlX + 86, panelY + 72, 92, 28), 14,
		temporaryVideoProtection ? C(darkMode, 0, 107, 95, 130, 213, 199) : C(darkMode, 116, 124, 120, 97, 108, 103));
	Text(g, temporaryVideoProtection ? L"已启用" : L"已关闭", controlX + 86, panelY + 79, 92, 18, 10, FontStyleBold,
		temporaryVideoProtection ? C(darkMode, 255, 255, 255, 0, 55, 49) : C(darkMode, 245, 247, 245, 219, 225, 222), StringAlignmentCenter);

	// 左列 · 模式
	Text(g, L"模式", controlX, panelY + 112, 40, 18, 10, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
	temporaryVideoImageModeRect = { static_cast<LONG>(controlX + 46), static_cast<LONG>(panelY + 106), static_cast<LONG>(controlX + 110), static_cast<LONG>(panelY + 134) };
	temporaryVideoFileModeRect = { static_cast<LONG>(controlX + 116), static_cast<LONG>(panelY + 106), static_cast<LONG>(controlX + 180), static_cast<LONG>(panelY + 134) };
	auto drawMode = [&](RECT rect, LPCWSTR label, bool selected) {
		FillRound(g, RectF(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top), static_cast<REAL>(rect.right - rect.left), static_cast<REAL>(rect.bottom - rect.top)), 14,
			selected ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 225, 232, 228, 39, 47, 44));
		Text(g, label, static_cast<float>(rect.left), static_cast<float>(rect.top + 7), static_cast<float>(rect.right - rect.left), 18, 9,
			selected ? FontStyleBold : FontStyleRegular, selected ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);
	};
	drawMode(temporaryVideoImageModeRect, L"图片", imageMode);
	drawMode(temporaryVideoFileModeRect, L"视频", !imageMode);

	// 左列 · 选择媒体 + 循环
	temporaryVideoImagePickerRect = { static_cast<LONG>(controlX), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 100), static_cast<LONG>(panelY + 170) };
	temporaryVideoFilePickerRect = { static_cast<LONG>(controlX + 106), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 206), static_cast<LONG>(panelY + 170) };
	auto drawPicker = [&](RECT rect, LPCWSTR label, bool selected) {
		FillRound(g, RectF(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top), static_cast<REAL>(rect.right - rect.left), static_cast<REAL>(rect.bottom - rect.top)), 7,
			Blend(C(darkMode, 219, 238, 232, 43, 76, 69), C(darkMode, 198, 228, 219, 55, 91, 83), selected ? 1.0f : 0.0f));
		Text(g, label, static_cast<float>(rect.left), static_cast<float>(rect.top + 7), static_cast<float>(rect.right - rect.left), 18, 9, FontStyleBold,
			C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	};
	drawPicker(temporaryVideoImagePickerRect, L"选择图片", imageMode);
	drawPicker(temporaryVideoFilePickerRect, L"选择视频", !imageMode);

	temporaryVideoLoopRect = { static_cast<LONG>(controlX + 218), static_cast<LONG>(panelY + 140), static_cast<LONG>(controlX + 288), static_cast<LONG>(panelY + 170) };
	FillRound(g, RectF(controlX + 218, panelY + 140, 70, 30), 7,
		temporaryVideoLoop ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 225, 232, 228, 39, 47, 44));
	Text(g, temporaryVideoLoop ? L"循环：开" : L"循环：关", controlX + 218, panelY + 147, 70, 18, 9, FontStyleBold,
		temporaryVideoLoop ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);

	// 左列 · 状态 / 警告
	const std::wstring& selectedPath = imageMode ? temporaryVideoImagePath : temporaryVideoFilePath;
	// 没选媒体时开启保护会回退成纯黑。明确写出来，别让用户以为是坏了。
	const bool noMedia = selectedPath.empty();
	const wchar_t* pathText = noMedia
		? (imageMode ? L"尚未选择图片 —— 此时开启保护，教师端会显示纯黑" : L"尚未选择视频 —— 此时开启保护，教师端会显示纯黑")
		: selectedPath.c_str();
	Text(g, pathText, controlX, panelY + 176, 340, 17, 8, FontStyleRegular,
		(noMedia && temporaryVideoProtection) ? C(darkMode, 198, 96, 40, 236, 146, 90) : C(darkMode, 91, 99, 95, 166, 177, 171));

	// 左列 · 一键恢复（遮挡有两个来源，只关一个教师端还是黑的）
	restoreTeacherViewRect = { static_cast<LONG>(controlX), static_cast<LONG>(panelY + 196), static_cast<LONG>(controlX + 152), static_cast<LONG>(panelY + 226) };
	FillRound(g, RectF(controlX, panelY + 196, 152, 30), 7, C(darkMode, 214, 234, 227, 48, 84, 76));
	Text(g, L"恢复教师画面", controlX, panelY + 203, 152, 18, 9, FontStyleBold, C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);

	// 右列 · 预览图（从 panelY+72 起，标题行与说明行都留给上方）
	const float previewW = (std::min)(300.0f, (std::max)(228.0f, w * 0.30f));
	const float previewH = (std::min)(300.0f, (std::max)(110.0f, panelH - 100.0f));
	temporaryVideoPreviewRect = { static_cast<LONG>(x + w - previewW - 42), static_cast<LONG>(panelY + 72), static_cast<LONG>(x + w - 42), static_cast<LONG>(panelY + 72 + previewH) };
	FillRound(g, RectF(static_cast<REAL>(temporaryVideoPreviewRect.left), static_cast<REAL>(temporaryVideoPreviewRect.top), previewW, previewH), 7, C(darkMode, 29, 38, 34, 232, 240, 236));
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
		Text(g, imageMode ? L"图片预览" : L"视频文件", x + w - previewW - 42, panelY + 72 + previewH / 2.0f + 28, previewW, 18, 9, FontStyleRegular,
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
	const Color titleColor = C(darkMode, 32, 38, 35, 223, 231, 227);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	const Color surface = C(darkMode, 255, 255, 255, 38, 45, 42);

	FillRound(g, RectF(x, y, w, h), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	Text(g, L"\u8fdb\u7a0b\u5185\u5b58\u7279\u5f81\u626b\u63cf", x + 24, y + 18, 320, 28, 16, FontStyleBold, titleColor);
	Text(g, L"\u7531 JiYuAvKernel \u626b\u63cf\u6240\u6709\u8fdb\u7a0b\u7684\u53ef\u6267\u884c\u5185\u5b58\u533a\u57df\uff0c\u652f\u6301\u8de8\u7248\u672c\u901a\u914d\u7279\u5f81", x + 24, y + 48, w - 48, 20, 10, FontStyleRegular, secondary);

	FillRound(g, RectF(x + 24, y + 78, w - 48, 86), 8, surface);
	FillRound(g, RectF(x + 42, y + 98, 12, 12), 6,
		driverReady ? primary : C(darkMode, 180, 72, 48, 232, 154, 123));
	Text(g, driverReady ? L"JiYuAvKernel \u5df2\u8fde\u63a5" : L"JiYuAvKernel \u672a\u8fde\u63a5",
		x + 66, y + 91, 240, 24, 12, FontStyleBold, titleColor);
	Text(g, avScanStatus.c_str(), x + 42, y + 124, w - 84, 24, 10, FontStyleRegular, secondary);

	auto drawButton = [&](RECT& target, float buttonX, float buttonW, LPCWSTR label, int hoverId, bool primaryButton) {
		target = { static_cast<LONG>(buttonX), static_cast<LONG>(y + 178),
			static_cast<LONG>(buttonX + buttonW), static_cast<LONG>(y + 220) };
		const Color base = primaryButton ? primary : C(darkMode, 219, 238, 232, 43, 76, 69);
		const Color over = primaryButton ? C(darkMode, 0, 82, 73, 154, 232, 217) : C(darkMode, 198, 228, 219, 55, 91, 83);
		FillRound(g, RectF(buttonX, y + 178, buttonW, 42), 8, Blend(base, over, hoverProgress[hoverId]));
		Text(g, label, buttonX + 8, y + 189, buttonW - 16, 20, 10, FontStyleBold,
			primaryButton ? C(darkMode, 255, 255, 255, 0, 55, 49) : C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	};
	const float buttonGap = 10.0f;
	const float buttonAvailable = w - 48.0f - buttonGap * 5.0f;
	const float unit = buttonAvailable / 8.6f;
	drawButton(avScanRect, x + 24, unit * 1.65f, running ? L"\u626b\u63cf\u4e2d..." : L"\u626b\u63cf\u5168\u90e8\u8fdb\u7a0b", 45, true);
	drawButton(avImportRect, avScanRect.right + buttonGap, unit * 1.45f, L"\u8fd0\u884c\u8fdb\u7a0b\u5f55\u5165", 46, false);
	drawButton(avInputRect, avImportRect.right + buttonGap, unit * 1.35f, L"\u8f93\u5165\u7279\u5f81\u7801", 50, false);
	drawButton(avCopyRect, avInputRect.right + buttonGap, unit * 1.15f, L"\u590d\u5236", 47, false);
	drawButton(avUnloadRect, avCopyRect.right + buttonGap, unit * 1.45f, L"\u5378\u8f7d\u626b\u63cf\u9a71\u52a8", 49, false);
	drawButton(avCancelRect, avUnloadRect.right + buttonGap, unit * 1.0f, L"\u505c\u6b62", 48, false);

	const float metricsY = y + 238;
	const float metricGap = 10.0f;
	const float metricW = (w - 48.0f - metricGap * 3.0f) / 4.0f;
	const wchar_t* metricLabels[] = { L"\u8fdb\u7a0b", L"\u5df2\u626b\u63cf", L"\u547d\u4e2d", L"\u8df3\u8fc7/\u5931\u8d25" };
	const DWORD metricValues[] = { avScanSummary.processCount, avScanSummary.scannedCount, avScanSummary.detectionCount, avScanSummary.failedCount };
	for (int index = 0; index < 4; ++index) {
		const float cardX = x + 24 + index * (metricW + metricGap);
		FillRound(g, RectF(cardX, metricsY, metricW, 74), 8, surface);
		Text(g, metricLabels[index], cardX + 14, metricsY + 11, metricW - 28, 18, 9, FontStyleRegular, secondary);
		WCHAR value[32] = {};
		swprintf_s(value, L"%lu", static_cast<unsigned long>(metricValues[index]));
		Text(g, value, cardX + 14, metricsY + 34, metricW - 28, 27, 17, FontStyleBold,
			index == 2 && metricValues[index] != 0 ? C(darkMode, 176, 62, 43, 246, 164, 139) : titleColor);
	}

	const float signatureY = metricsY + 88;
	FillRound(g, RectF(x + 24, signatureY, w - 48, 84), 8, surface);
	WCHAR signatureCaption[128] = {};
	swprintf_s(signatureCaption, L"\u6700\u8fd1\u5f55\u5165\u7684\u5185\u5b58\u7279\u5f81  |  %lu \u4e2a\u7248\u672c  |  %lu \u4e2a\u7cbe\u786e\u5b57\u8282",
		static_cast<unsigned long>(avSignatureSampleCount), static_cast<unsigned long>(avSignatureExactBytes));
	Text(g, signatureCaption, x + 40, signatureY + 11, w - 80, 18, 9, FontStyleRegular, secondary);
	Text(g, avSignaturePath.empty() ? L"\u5c1a\u672a\u5f55\u5165\u8fd0\u884c\u65f6\u7279\u5f81" : avSignaturePath.c_str(),
		x + 40, signatureY + 32, w - 80, 18, 9, FontStyleBold, titleColor);
	Text(g, avSignaturePattern.empty() ? L"-" : avSignaturePattern.c_str(),
		x + 40, signatureY + 54, w - 80, 18, 9, FontStyleRegular, primary);

	const float resultY = signatureY + 98;
	const float resultH = std::max(58.0f, y + h - resultY - 16.0f);
	FillRound(g, RectF(x + 24, resultY, w - 48, resultH), 8, surface);
	Text(g, L"\u626b\u63cf\u547d\u4e2d", x + 40, resultY + 12, 160, 20, 11, FontStyleBold, titleColor);
	if (avScanSummary.findingCount == 0) {
		Text(g, running ? L"\u6b63\u5728\u68c0\u67e5\u8fdb\u7a0b\u6620\u50cf..." : L"\u6682\u65e0\u547d\u4e2d\u8bb0\u5f55",
			x + 40, resultY + 42, w - 80, 20, 10, FontStyleRegular, secondary);
	}
	else {
		const DWORD visibleCount = std::min<DWORD>(avScanSummary.findingCount, AV_INTEGRATED_MAX_FINDINGS);
		for (DWORD index = 0; index < visibleCount; ++index) {
			Text(g, avScanSummary.findings[index], x + 40, resultY + 40 + index * 22.0f,
				w - 80, 18, 9, FontStyleRegular, C(darkMode, 150, 52, 37, 244, 158, 132));
		}
	}
}

void MainWindow::PaintAdvanced(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	FillRound(g, RectF(x, y, w, h), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	const wchar_t* tabs[] = { L"\u5e38\u89c4\u4e0e\u5feb\u6377\u952e", L"\u8c03\u8bd5\u4e0e\u517c\u5bb9\u6027" };
	for (int i = 0; i < 2; ++i) {
		const float tabX = x + 24 + i * 166.0f;
		advancedTabRects[i] = { static_cast<LONG>(tabX), static_cast<LONG>(y + 18), static_cast<LONG>(tabX + 156), static_cast<LONG>(y + 58) };
		FillRound(g, RectF(tabX, y + 18, 156, 40), 20,
			advancedTab == i ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 225, 232, 228, 39, 47, 44));
		Text(g, tabs[i], tabX, y + 29, 156, 20, 10, advancedTab == i ? FontStyleBold : FontStyleRegular,
			advancedTab == i ? C(darkMode, 0, 81, 72, 156, 240, 225) : C(darkMode, 60, 68, 64, 197, 207, 202), StringAlignmentCenter);
	}
	auto drawToggle = [&](int index, float cardX, float cardY, float cardW, LPCWSTR title, LPCWSTR detail, bool enabled) {
		advancedToggleRects[index] = { static_cast<LONG>(cardX), static_cast<LONG>(cardY), static_cast<LONG>(cardX + cardW), static_cast<LONG>(cardY + 64) };
		FillRound(g, RectF(cardX, cardY, cardW, 64), 8,
			Blend(C(darkMode, 255, 255, 255, 38, 45, 42), C(darkMode, 220, 237, 231, 49, 63, 58), hoverProgress[50 + index]));
		Text(g, title, cardX + 16, cardY + 12, cardW - 92, 20, 10, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
		Text(g, detail, cardX + 16, cardY + 35, cardW - 92, 17, 8, FontStyleRegular, C(darkMode, 98, 106, 102, 159, 171, 165));
		FillRound(g, RectF(cardX + cardW - 60, cardY + 19, 44, 26), 13,
			enabled ? C(darkMode, 0, 107, 95, 130, 213, 199) : C(darkMode, 116, 124, 120, 97, 108, 103));
		SolidBrush knob(enabled ? C(darkMode, 255, 255, 255, 0, 55, 49) : C(darkMode, 245, 247, 245, 219, 225, 222));
		g.FillEllipse(&knob, RectF(cardX + cardW - (enabled ? 38.0f : 56.0f), cardY + 23, 18, 18));
	};
	if (advancedTab == 0) {
		const float gap = 12.0f;
		const float cardW = (w - 60) / 2.0f;
		const wchar_t* titles[] = { L"\u7981\u7528\u5185\u6838\u9a71\u52a8", L"\u9a71\u52a8\u5c42\u81ea\u6211\u4fdd\u62a4", L"\u81ea\u52a8\u5f3a\u5236\u6e05\u7406", L"\u4e25\u683c\u7a97\u53e3\u63a7\u5236", L"\u9690\u85cf\u63a7\u5236\u8f93\u51fa", L"\u9690\u85cf\u4efb\u52a1\u680f\u56fe\u6807" };
		const wchar_t* details[] = { L"32 \u4f4d\u7cfb\u7edf\u9009\u9879", L"\u4ec5 32 \u4f4d\u7cfb\u7edf\u6709\u6548", L"\u64cd\u4f5c\u5931\u8d25\u65f6\u7ed3\u675f\u76f8\u5173\u8fdb\u7a0b", L"\u63a7\u5236\u6240\u6709\u6781\u57df\u7a97\u53e3", L"\u9690\u85cf\u5de6\u4e0a\u89d2\u8f93\u51fa\u7a97\u53e3", L"\u4ecd\u53ef\u4f7f\u7528\u5feb\u6377\u952e\u5524\u8d77" };
		bool values[] = { advDisableDriver, advSelfProtect, advAutoForceKill, advStrictWindow, advHideOutput, advHideTaskbar };
		for (int i = 0; i < 6; ++i) {
			const int col = i % 2, row = i / 2;
			drawToggle(i, x + 24 + col * (cardW + gap), y + 78 + row * 76.0f, cardW, titles[i], details[i], values[i]);
		}
		Text(g, L"\u7d27\u6025\u5168\u5c4f\u5feb\u6377\u952e", x + 24, y + 316, 170, 20, 10, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
		Text(g, L"\u663e\u793a / \u9690\u85cf\u4e3b\u7a97\u53e3", x + w / 2 + 6, y + 316, 180, 20, 10, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
	}
	else {
		Text(g, L"\u7ed3\u675f\u8fdb\u7a0b\u65b9\u5f0f", x + 24, y + 78, 180, 22, 11, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
		const wchar_t* killLabels[] = { L"TerminateProcess", L"NtTerminateProcess", L"Kernel Mode" };
		for (int i = 0; i < 3; ++i) {
			const float bx = x + 24 + i * 154.0f;
			advancedKillRects[i] = { static_cast<LONG>(bx), static_cast<LONG>(y + 108), static_cast<LONG>(bx + 144), static_cast<LONG>(y + 146) };
			FillRound(g, RectF(bx, y + 108, 144, 38), 19, advancedKillMode == i ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 255, 255, 255, 38, 45, 42));
			Text(g, killLabels[i], bx, y + 118, 144, 18, 9, advancedKillMode == i ? FontStyleBold : FontStyleRegular,
				C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
		}
		Text(g, L"\u6ce8\u5165\u65b9\u5f0f", x + 24, y + 166, 180, 22, 11, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
		const wchar_t* injectLabels[] = { L"RemoteThread", L"HookDllStub" };
		for (int i = 0; i < 2; ++i) {
			const float bx = x + 24 + i * 154.0f;
			advancedInjectRects[i] = { static_cast<LONG>(bx), static_cast<LONG>(y + 196), static_cast<LONG>(bx + 144), static_cast<LONG>(y + 234) };
			FillRound(g, RectF(bx, y + 196, 144, 38), 19, advancedInjectMode == i ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 255, 255, 255, 38, 45, 42));
			Text(g, injectLabels[i], bx, y + 206, 144, 18, 9, advancedInjectMode == i ? FontStyleBold : FontStyleRegular,
				C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
		}
		const wchar_t* titles[] = { L"\u542f\u7528\u63a7\u5236\u5668", L"\u59cb\u7ec8\u68c0\u67e5\u66f4\u65b0", L"\u5f3a\u5236\u5b89\u88c5\u5230\u5f53\u524d\u76ee\u5f55", L"\u7981\u7528 WatchDog", L"\u6ce8\u5165 Master Helper", L"\u6ce8\u5165 64 \u4f4d Helper" };
		bool values[] = { advController, advAlwaysUpdate, advForceCurrentDir, advDisableWatchdog, advInjectMaster, advInject64 };
		const float cardW = (w - 60) / 2.0f;
		for (int i = 0; i < 6; ++i) {
			const int col = i % 2, row = i / 2;
			drawToggle(i, x + 24 + col * (cardW + 12), y + 260 + row * 68.0f, cardW, titles[i], L"\u9ad8\u7ea7\u9009\u9879\uff0c\u4fee\u6539\u540e\u8bf7\u4fdd\u5b58", values[i]);
		}
		Text(g, L"\u68c0\u6d4b\u95f4\u9694 (ms)", x + w - 184, y + 198, 150, 20, 10, FontStyleBold, C(darkMode, 42, 49, 46, 220, 229, 225));
	}
	advancedSaveRect = { static_cast<LONG>(x + w - 148), static_cast<LONG>(y + h - 54), static_cast<LONG>(x + w - 24), static_cast<LONG>(y + h - 14) };
	FillRound(g, RectF(x + w - 148, y + h - 54, 124, 40), 20, C(darkMode, 0, 107, 95, 130, 213, 199));
	Text(g, L"\u4fdd\u5b58\u9ad8\u7ea7\u8bbe\u7f6e", x + w - 148, y + h - 43, 124, 20, 10, FontStyleBold, C(darkMode, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
}

void MainWindow::PaintHelp(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	FillRound(g, RectF(x, y, w, h), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	Text(g, L"\u5feb\u901f\u4e0a\u624b", x + 28, y + 24, 220, 28, 17, FontStyleBold, C(darkMode, 32, 38, 35, 223, 231, 227));
	Text(g, L"Dzjs Trainer \u4f1a\u81ea\u52a8\u68c0\u6d4b StudentMain \u5e76\u5e94\u7528\u5f53\u524d\u9632\u62a4\u7b56\u7565\u3002", x + 28, y + 58, w - 56, 28, 11,
		FontStyleRegular, C(darkMode, 81, 91, 86, 171, 183, 177));
	const wchar_t* steps[] = {
		L"1", L"\u786e\u8ba4\u72b6\u6001", L"\u5728\u201c\u72b6\u6001\u603b\u89c8\u201d\u4e2d\u68c0\u67e5\u5b66\u751f\u7aef\u8fdb\u7a0b\u548c\u9632\u62a4\u670d\u52a1\u662f\u5426\u6b63\u5e38\u3002",
		L"2", L"\u8c03\u6574\u7b56\u7565", L"\u5728\u201c\u9632\u62a4\u7b56\u7565\u201d\u4e2d\u9009\u62e9\u5141\u8bb8\u7684\u8fdc\u7a0b\u884c\u4e3a\uff0c\u4fee\u6539\u540e\u7acb\u5373\u5e94\u7528\u3002",
		L"3", L"\u67e5\u770b\u65e5\u5fd7", L"\u5982\u679c\u9632\u62a4\u672a\u6309\u9884\u671f\u5de5\u4f5c\uff0c\u8bf7\u5728\u201c\u8fd0\u884c\u65e5\u5fd7\u201d\u4e2d\u67e5\u770b\u539f\u56e0\u3002"
	};
	for (int i = 0; i < 3; ++i) {
		const float rowY = y + 112 + i * 116.0f;
		FillRound(g, RectF(x + 28, rowY, w - 56, 94), 8, C(darkMode, 255, 255, 255, 38, 45, 42));
		FillRound(g, RectF(x + 46, rowY + 25, 44, 44), 22, C(darkMode, 183, 232, 222, 42, 79, 72));
		Text(g, steps[i * 3], x + 46, rowY + 35, 44, 24, 12, FontStyleBold, C(darkMode, 0, 81, 72, 156, 240, 225), StringAlignmentCenter);
		Text(g, steps[i * 3 + 1], x + 112, rowY + 19, w - 170, 24, 13, FontStyleBold, C(darkMode, 37, 44, 41, 222, 230, 226));
		Text(g, steps[i * 3 + 2], x + 112, rowY + 50, w - 170, 34, 10, FontStyleRegular, C(darkMode, 91, 99, 95, 166, 177, 171));
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
		const Color surface = C(darkMode, 237, 242, 239, 31, 37, 34);
		const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
		const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
		FillRound(g, RectF(x, y, w, h), 8, surface);
		const int hostX = 264;
		const int hostY = 170;
		const int hostW = std::max(560, width - hostX - 24);
		const int hostH = std::max(320, height - hostY - 24);
		teacherLaunchRect = { hostX, 120, hostX + 144, 154 };
		FillRound(g, RectF(static_cast<float>(hostX), static_cast<float>(hostY), static_cast<float>(hostW), static_cast<float>(hostH)), 8,
			C(darkMode, 255, 255, 255, 38, 45, 42));
		const bool teacherRunning = teacherProcess != nullptr && teacherChildWindow != nullptr;
		Text(g, teacherRunning ? L"\u5df2\u5d4c\u5165\u539f\u6559\u5e08\u7aef\u7a97\u53e3" : L"\u539f\u6559\u5e08\u7aef\u5c1a\u672a\u542f\u52a8", hostX + 168, 126, w - 192, 24, 10, FontStyleBold, title);
		Text(g, teacherRunning ? L"\u4e0b\u65b9\u533a\u57df\u4e3a\u539f\u7a97\u53e3\u5185\u5bb9\uff0c\u53ef\u76f4\u63a5\u64cd\u4f5c\u3002" : L"\u70b9\u51fb\u4e0a\u65b9\u6309\u94ae\u5f00\u59cb\u5d4c\u5165\u3002", hostX + 168, 148, w - 192, 20, 9, FontStyleRegular, secondary);
		return;
	}
	// The teacher endpoint is rendered by this GDI+ page; its service is only a
	// native backend and never creates a second window.
	{
		const Color surface = C(darkMode, 237, 242, 239, 31, 37, 34);
		const Color field = C(darkMode, 255, 255, 255, 38, 45, 42);
		const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
		const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
		FillRound(g, RectF(x, y, w, h), 8, surface);
		const int topY = static_cast<int>(y + 78);
		FillRound(g, RectF(x + 16, static_cast<float>(topY), w - 32, 54), 8, field);
		const bool teacherRunning = teacherBackend && teacherBackend->running;
		Text(g, teacherRunning ? L"\u6559\u5e08\u670d\u52a1\u8fd0\u884c\u4e2d" : L"\u6559\u5e08\u670d\u52a1\u672a\u542f\u52a8", x + 32, static_cast<float>(topY + 12), 180, 22, 11, FontStyleBold, title);
		Text(g, teacherRunning ? L"\u5df2\u76d1\u542c 4705 / \u4f1a\u8bdd\u7ec4\u64ad\uff0c\u6b63\u5728\u63a5\u6536\u5b66\u751f\u5217\u8868" : L"\u70b9\u51fb\u542f\u52a8\u6559\u5e08\u670d\u52a1\u5f00\u59cb\u81ea\u52a8\u53d1\u73b0", x + 220, static_cast<float>(topY + 14), w - 360, 20, 9, FontStyleRegular, secondary);
		const float cardY = y + 148;
		const float listW = std::min(520.0f, w * 0.46f);
		FillRound(g, RectF(x + 16, cardY, listW, h - 170), 8, field);
		FillRound(g, RectF(x + 28 + listW, cardY, w - listW - 44, h - 170), 8, field);
		Text(g, L"\u5b66\u751f\u5217\u8868", x + 32, cardY + 14, 180, 22, 11, FontStyleBold, title);
		Text(g, L"\u81ea\u52a8\u53d1\u73b0\u3001\u9884\u89c8\u3001\u4fe1\u606f\u3001\u804a\u5929\u548c\u8fdc\u7a0b\u63a7\u5236", x + 32, cardY + 38, listW - 32, 18, 9, FontStyleRegular, secondary);
		Text(g, L"\u5b66\u751f\u64cd\u4f5c", x + 44 + listW, cardY + 14, 180, 22, 11, FontStyleBold, title);
		Text(g, L"\u9009\u62e9\u5de6\u4fa7\u5b66\u751f\u540e\u53d1\u9001\u64cd\u4f5c", x + 44 + listW, cardY + 38, w - listW - 76, 18, 9, FontStyleRegular, secondary);
		const float previewX = x + 44 + listW;
		const float previewY = cardY + 252;
		const float previewW = std::max(260.0f, w - listW - 76.0f);
		const float previewH = std::max(132.0f, h - 170.0f - (previewY - cardY) - 16.0f);
		FillRound(g, RectF(previewX, previewY, previewW, previewH), 8, C(darkMode, 20, 25, 23, 255, 28, 25));
		Text(g, L"\u5c4f\u5e55\u9884\u89c8", previewX + 14, previewY + 12, 120, 20, 10, FontStyleBold, C(darkMode, 210, 218, 214, 190, 199, 194));
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
		else Text(g, L"\u9009\u62e9\u5b66\u751f\u540e\u70b9\u51fb\u5237\u65b0\u9884\u89c8", previewX + 16, previewY + previewH * 0.5f - 8, previewW - 32, 24, 10, FontStyleRegular,
			C(darkMode, 142, 151, 146, 150, 161, 155), StringAlignmentCenter);
		return;
	}
#if 0
	const float panelX = x + 16;
	const float panelW = w - 32;
	const float innerX = x + 32;
	const float innerW = w - 64;
	const float fieldW = std::max(174.0f, innerW - 422.0f);
	const Color surface = C(darkMode, 237, 242, 239, 31, 37, 34);
	const Color field = C(darkMode, 255, 255, 255, 38, 45, 42);
	const Color outline = C(darkMode, 116, 124, 120, 101, 113, 107);
	const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	FillRound(g, RectF(x, y, w, h), 8, surface);
	Text(g, L"\u7f51\u7edc\u8bca\u65ad\u4e0e\u6d88\u606f", innerX, y + 18, 300, 28, 17, FontStyleBold, title);
	Text(g, L"\u4f7f\u7528\u5c40\u57df\u7f51\u626b\u63cf\u3001\u7aef\u53e3\u68c0\u6d4b\u548c UDP \u5de5\u5177", innerX, y + 48, 420, 18, 9, FontStyleRegular, secondary);

	FillRound(g, RectF(panelX, y + 76, panelW, 72), 8, field);
	Text(g, L"\u76ee\u6807 IP", innerX, y + 100, 64, 20, 10, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 91, fieldW + 8, 38), 8, field);
	StrokeRound(g, RectF(innerX + 70, y + 91, fieldW + 8, 38), 8, outline);
	Text(g, L"\u7aef\u53e3", innerX + 88 + fieldW, y + 100, 42, 20, 10, FontStyleBold, title);
	FillRound(g, RectF(innerX + 132 + fieldW, y + 91, 80, 38), 8, field);
	StrokeRound(g, RectF(innerX + 132 + fieldW, y + 91, 80, 38), 8, outline);

	FillRound(g, RectF(panelX, y + 156, panelW, 58), 8, field);
	Text(g, L"\u6d88\u606f", innerX, y + 177, 64, 20, 10, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 166, innerW - 180, 38), 8, field);
	StrokeRound(g, RectF(innerX + 70, y + 166, innerW - 180, 38), 8, outline);

	FillRound(g, RectF(panelX, y + 222, panelW, 58), 8, field);
	Text(g, L"\u547d\u4ee4", innerX, y + 243, 64, 20, 10, FontStyleBold, title);
	FillRound(g, RectF(innerX + 70, y + 232, innerW - 180, 38), 8, field);
	StrokeRound(g, RectF(innerX + 70, y + 232, innerW - 180, 38), 8, outline);

	Text(g, L"\u5feb\u6377\u64cd\u4f5c", innerX, y + 300, 120, 20, 10, FontStyleBold, title);
	Text(g, L"\u8fd0\u884c\u7ed3\u679c", innerX, y + 356, 120, 20, 11, FontStyleBold, title);
	FillRound(g, RectF(panelX, y + 382, panelW, std::max(72.0f, h - 398)), 8, field);
	StrokeRound(g, RectF(panelX, y + 382, panelW, std::max(72.0f, h - 398)), 8, C(darkMode, 199, 205, 201, 67, 75, 71));

	FillRound(g, RectF(x + 8, y + 8, w - 16, h - 16), 8,
		darkMode ? Color(232, 17, 20, 19) : Color(232, 246, 248, 246));
	const float centerY = y + h * 0.5f - 82.0f;
	FillRound(g, RectF(x + w * 0.5f - 28, centerY, 56, 56), 8, C(darkMode, 219, 238, 232, 43, 76, 69));
	DrawIcon(g, 6, x + w * 0.5f - 10, centerY + 18, C(darkMode, 0, 81, 72, 177, 239, 228), 20);
	Text(g, L"\u65b0\u7248\u53cd\u63a7\u5de5\u5177\u6b63\u5728\u5f00\u53d1\u4e2d", x + 40, centerY + 76, w - 80, 34, 20, FontStyleBold,
		C(darkMode, 35, 42, 39, 222, 230, 226), StringAlignmentCenter);
	Text(g, L"\u8bf7\u524d\u5f80 GitHub \u9879\u76ee\u67e5\u770b\u5f00\u53d1\u8fdb\u5ea6\u4e0e\u72ec\u7acb\u7248\u672c", x + 40, centerY + 116, w - 80, 24, 10, FontStyleRegular,
		C(darkMode, 91, 99, 95, 166, 177, 171), StringAlignmentCenter);
	const float linkW = std::min(360.0f, w - 80.0f);
	const float linkX = x + (w - linkW) * 0.5f;
	networkProjectRect = { static_cast<LONG>(linkX), static_cast<LONG>(centerY + 158), static_cast<LONG>(linkX + linkW), static_cast<LONG>(centerY + 202) };
	FillRound(g, RectF(linkX, centerY + 158, linkW, 44), 22,
		Blend(C(darkMode, 183, 232, 222, 42, 79, 72), C(darkMode, 157, 220, 207, 55, 96, 87), hoverProgress[72]));
	Text(g, L"GitHub  /  yunsjxh/Third-party-JiYu-Teacher-Endpoint", linkX + 16, centerY + 170, linkW - 32, 20, 10, FontStyleBold,
		C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	const int hostX = 264;
		const int hostY = 170;
	const int hostW = std::max(560, width - hostX - 24);
	const int hostH = std::max(320, height - hostY - 24);
	FillRound(g, RectF(static_cast<float>(hostX), static_cast<float>(hostY), static_cast<float>(hostW), static_cast<float>(hostH)), 8,
		C(darkMode, 255, 255, 255, 38, 45, 42));
	teacherLaunchRect = { hostX, 120, hostX + 144, 154 };
	const bool teacherRunning = teacherProcess != nullptr && teacherChildWindow != nullptr;
	Text(g, teacherRunning ? L"\u6559\u5e08\u7aef\u5df2\u5d4c\u5165\u8fd0\u884c" : L"\u6559\u5e08\u7aef\u5c1a\u672a\u542f\u52a8", hostX + 168, 126, w - 192, 24, 10, FontStyleBold,
		C(darkMode, 35, 42, 39, 222, 230, 226));
	Text(g, teacherRunning ? L"\u7a97\u53e3\u548c\u7f51\u7edc\u670d\u52a1\u5728\u6b64\u533a\u57df\u5185\u8fd0\u884c\u3002" : L"\u70b9\u51fb\u4e0a\u65b9\u6309\u94ae\u542f\u52a8\u5e76\u5d4c\u5165 cpp-gui\u3002", hostX + 168, 148, w - 192, 20, 9, FontStyleRegular,
	C(darkMode, 91, 99, 95, 166, 177, 171));
#endif
}

void MainWindow::PaintDiagnostics(Graphics& g, int width, int height, int offsetY)
{
	const float x = 248.0f;
	const float y = 94.0f + offsetY;
	const float w = static_cast<float>(width - 276);
	const float h = static_cast<float>(height) - y - 20;
	const Color surface = C(darkMode, 237, 242, 239, 31, 37, 34);
	const Color field = C(darkMode, 255, 255, 255, 38, 45, 42);
	const Color title = C(darkMode, 35, 42, 39, 222, 230, 226);
	const Color secondary = C(darkMode, 91, 99, 95, 166, 177, 171);
	FillRound(g, RectF(x, y, w, h), 8, surface);
	Text(g, L"网络与 USB 阻断诊断", x + 20, y + 18, 330, 26, 17, FontStyleBold, title);
	Text(g, L"选择驱动查看位置、签名、服务和筛选依据；手动卸载前会要求确认。", x + 20, y + 50, w - 190, 20, 10, FontStyleRegular, secondary);
	diagnosticsHideOwnRect = { static_cast<LONG>(x + w - 326), static_cast<LONG>(y + 16),
		static_cast<LONG>(x + w - 172), static_cast<LONG>(y + 50) };
	FillRound(g, RectF(x + w - 326, y + 16, 154, 34), 17,
		diagnosticsHideOwnDrivers ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 235, 240, 237, 48, 56, 52));
	Text(g, diagnosticsHideOwnDrivers ? L"显示本程序条目" : L"隐藏本程序条目",
		x + w - 326, y + 25, 154, 18, 9, FontStyleBold,
		diagnosticsHideOwnDrivers ? C(darkMode, 0, 81, 72, 177, 239, 228) : secondary, StringAlignmentCenter);
	diagnosticsScanRect = { static_cast<LONG>(x + w - 152), static_cast<LONG>(y + 16),
		static_cast<LONG>(x + w - 20), static_cast<LONG>(y + 50) };
	FillRound(g, RectF(x + w - 152, y + 16, 132, 34), 17,
		C(darkMode, 0, 107, 95, 130, 213, 199));
	Text(g, diagnosticsRunning ? L"正在检查..." : L"开始诊断", x + w - 152, y + 25, 132, 18, 10,
		FontStyleBold, C(darkMode, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);

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
		FillRound(g, RectF(cardX, metricY, metricW, 60), 8, field);
		Text(g, metrics[i].first.c_str(), cardX + 14, metricY + 12, metricW - 28, 16, 9, FontStyleRegular, secondary);
		Text(g, metrics[i].second.c_str(), cardX + 14, metricY + 32, metricW - 28, 18, 11, FontStyleBold, title);
	}

	const float panelY = metricY + 76;
	const float listX = x + 16;
	const float listW = std::max(330.0f, (w - 48) * 0.56f);
	const float detailX = listX + listW + 16;
	const float detailW = x + w - 16 - detailX;
	const float panelH = std::max(150.0f, y + h - panelY - 16);
	FillRound(g, RectF(listX, panelY, listW, panelH), 8, field);
	FillRound(g, RectF(detailX, panelY, detailW, panelH), 8, field);
	diagnosticsListRect = { static_cast<LONG>(listX), static_cast<LONG>(panelY),
		static_cast<LONG>(listX + listW), static_cast<LONG>(panelY + panelH) };
	Text(g, L"已加载驱动", listX + 16, panelY + 14, 130, 20, 12, FontStyleBold, title);
	Text(g, std::to_wstring(diagnosticsDrivers.size()).c_str(), listX + listW - 48, panelY + 15, 32, 18, 10, FontStyleBold, secondary, StringAlignmentFar);

	const std::array<const wchar_t*, 4> filterNames = { L"全部", L"高风险", L"已签名", L"待核查" };
	const float filterY = panelY + 42;
	const float filterW = (listW - 32 - 18) / 4.0f;
	for (size_t i = 0; i < filterNames.size(); ++i) {
		const float buttonX = listX + 16 + static_cast<float>(i) * (filterW + 6);
		diagnosticsFilterRects[i] = { static_cast<LONG>(buttonX), static_cast<LONG>(filterY), static_cast<LONG>(buttonX + filterW), static_cast<LONG>(filterY + 28) };
		const bool active = static_cast<int>(diagnosticsFilter) == static_cast<int>(i);
		FillRound(g, RectF(buttonX, filterY, filterW, 28), 14,
			active ? C(darkMode, 183, 232, 222, 42, 79, 72) : C(darkMode, 235, 240, 237, 48, 56, 52));
		Text(g, filterNames[i], buttonX, filterY + 7, filterW, 16, 9, active ? FontStyleBold : FontStyleRegular,
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
		Text(g, L"点击“开始诊断”收集已加载驱动。", listX + 20, rowY + 16, listW - 40, 22, 10, FontStyleRegular, secondary);
	}
	else if (diagnosticsVisibleDrivers.empty()) {
		Text(g, L"当前筛选没有匹配的驱动。", listX + 20, rowY + 16, listW - 40, 22, 10, FontStyleRegular, secondary);
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
				selected ? C(darkMode, 218, 239, 233, 46, 65, 60) : C(darkMode, 245, 248, 246, 43, 49, 46));
			FillRound(g, RectF(listX + 22, itemY + 16, 12, 12), 6, statusColor);
			Text(g, driver.name.c_str(), listX + 46, itemY + 8, listW - 170, 16, 10, FontStyleBold, title);
			Text(g, driver.path.c_str(), listX + 46, itemY + 26, listW - 64, 14, 8, FontStyleRegular, secondary);
			Text(g, driver.highRisk ? L"高风险" : (driver.needsReview ? L"核查" : L"已签名"), listX + listW - 90, itemY + 14, 58, 16, 9, FontStyleBold, statusColor, StringAlignmentFar);
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
			Text(g, range.c_str(), listX + listW - 110, panelY + 15, 56, 18, 8, FontStyleRegular, secondary, StringAlignmentFar);
		}
	}

	Text(g, L"驱动详情", detailX + 18, panelY + 16, detailW - 36, 20, 12, FontStyleBold, title);
	diagnosticsUnloadRect = {};
	const DiagnosticDriver* selectedDriver = diagnosticsSelectedDriver >= 0 && diagnosticsSelectedDriver < static_cast<int>(diagnosticsDrivers.size())
		? &diagnosticsDrivers[static_cast<size_t>(diagnosticsSelectedDriver)] : nullptr;
	if (!selectedDriver) {
		Text(g, L"从左侧选择一项驱动查看证据。", detailX + 18, panelY + 58, detailW - 36, 22, 10, FontStyleRegular, secondary);
		Text(g, L"诊断只给出风险线索，不对驱动功能或意图下结论。", detailX + 18, panelY + 86, detailW - 36, 38, 10, FontStyleRegular, secondary);
	}
	else {
		const Color detailStatus = selectedDriver->highRisk ? C(darkMode, 213, 83, 83, 244, 149, 149) :
			(selectedDriver->needsReview ? C(darkMode, 184, 130, 23, 239, 198, 101) : C(darkMode, 0, 127, 112, 130, 213, 199));
		FillRound(g, RectF(detailX + 18, panelY + 50, detailW - 36, 30), 6, C(darkMode, 245, 248, 246, 43, 49, 46));
		Text(g, selectedDriver->rating.c_str(), detailX + 30, panelY + 58, detailW - 60, 16, 9, FontStyleBold, detailStatus);
		diagnosticsUnloadRect = { static_cast<LONG>(detailX + detailW - 132), static_cast<LONG>(panelY + 88),
			static_cast<LONG>(detailX + detailW - 18), static_cast<LONG>(panelY + 120) };
		FillRound(g, RectF(detailX + detailW - 132, panelY + 88, 114, 32), 16,
			selectedDriver->canUnload ? C(darkMode, 213, 83, 83, 219, 139, 130) : C(darkMode, 220, 225, 222, 70, 78, 74));
		Text(g, selectedDriver->canUnload ? L"停止并卸载" : L"不可操作",
			detailX + detailW - 132, panelY + 97, 114, 18, 9, FontStyleBold,
			selectedDriver->canUnload ? C(darkMode, 255, 255, 255, 80, 88, 84) : secondary, StringAlignmentCenter);
		const std::array<std::pair<const wchar_t*, const std::wstring*>, 5> fields = {{
			{ L"文件名", &selectedDriver->name }, { L"发布者", &selectedDriver->signer },
			{ L"加载路径", &selectedDriver->path }, { L"服务", &selectedDriver->service },
			{ L"筛选依据", &selectedDriver->evidence }
		}};
		for (size_t i = 0; i < fields.size(); ++i) {
			const float fieldY = panelY + 132 + static_cast<float>(i) * 52;
			Text(g, fields[i].first, detailX + 18, fieldY, detailW - 36, 15, 9, FontStyleBold, secondary);
			Text(g, fields[i].second->c_str(), detailX + 18, fieldY + 19, detailW - 36, 28, 9, FontStyleRegular, title);
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
	FillRound(g, RectF(x, y, w, h), 8, C(darkMode, 31, 36, 34, 25, 30, 28));
	Text(g, L"\u5b9e\u65f6\u8f93\u51fa", x + 20, y + 18, 160, 20, 11, FontStyleBold, C(darkMode, 177, 231, 220, 130, 213, 199));
	exportLogRect = { static_cast<LONG>(x + w - 134), static_cast<LONG>(y + 12), static_cast<LONG>(x + w - 18), static_cast<LONG>(y + 44) };
	FillRound(g, RectF(x + w - 134, y + 12, 116, 32), 16,
		Blend(C(darkMode, 185, 232, 223, 43, 76, 69), C(darkMode, 161, 221, 210, 55, 91, 83), hoverProgress[40]));
	Text(g, L"\u5bfc\u51fa\u65e5\u5fd7", x + w - 134, y + 20, 116, 18, 10, FontStyleBold,
		C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	const int visible = std::max(1, static_cast<int>((h - 58) / 24));
	const int start = std::max(0, static_cast<int>(logs.size()) - visible);
	for (int i = start; i < static_cast<int>(logs.size()); ++i) {
		const float rowY = y + 50 + (i - start) * 24.0f;
		Color color = C(darkMode, 209, 216, 212, 197, 207, 202);
		if (logs[i].level == LogLevelInfo) color = C(darkMode, 65, 135, 120, 130, 213, 199);
		else if (logs[i].level == LogLevelWarn) color = C(darkMode, 151, 108, 10, 241, 194, 98);
		else if (logs[i].level == LogLevelError) color = C(darkMode, 186, 26, 26, 255, 180, 171);
		Text(g, logs[i].text.c_str(), x + 20, rowY, w - 40, 22, 10, FontStyleRegular, color);
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
	const Color surface = C(darkMode, 255, 255, 255, 38, 45, 42);
	FillRound(g, RectF(x, y, w, h), 8, C(darkMode, 237, 242, 239, 31, 37, 34));
	FillRound(g, RectF(x + 42, y + 42, 84, 84), 8, C(darkMode, 0, 107, 95, 130, 213, 199));
	Text(g, L"J", x + 42, y + 56, 84, 62, 40, FontStyleBold, C(darkMode, 255, 255, 255, 0, 55, 49), StringAlignmentCenter);
	Text(g, L"Dzjs Trainer", x + 154, y + 46, 420, 40, 27, FontStyleBold, titleColor);
	Text(g, L"Windows \u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3", x + 154, y + 89, 420, 24, 12, FontStyleRegular, C(darkMode, 84, 93, 88, 171, 183, 177));
	Text(g, L"\u7531\u8001 JiYu Trainer \u5347\u7ea7\u800c\u6765\u3002\u4f5c\u8005\uff1a\u4e91\u6563\u7686\u661f\u6cb3 & \u5feb\u4e50\u7684\u68a6\u9c7c\uff08\u539f\u4f5c\u8005\uff09", x + 154, y + 116, 520, 22, 10, FontStyleRegular, bodyColor);
	FillRound(g, RectF(x + 42, y + 158, 142, 30), 15, C(darkMode, 219, 238, 232, 43, 76, 69));
	Text(g, AboutVersionLine().c_str(), x + 42, y + 166, 142, 18, 10, FontStyleBold, C(darkMode, 0, 81, 72, 177, 239, 228), StringAlignmentCenter);
	aboutUpdateRect = { static_cast<LONG>(x + 204), static_cast<LONG>(y + 153), static_cast<LONG>(x + 354), static_cast<LONG>(y + 193) };
	FillRound(g, RectF(x + 204, y + 153, 150, 40), 20,
		Blend(C(darkMode, 219, 238, 232, 43, 76, 69), C(darkMode, 183, 232, 222, 55, 106, 96), hoverProgress[74]));
	DrawIcon(g, 2, x + 220, y + 164, C(darkMode, 0, 81, 72, 156, 240, 225), 17);
	Text(g, L"\u68c0\u67e5\u66f4\u65b0", x + 244, y + 162, 96, 22, 11, FontStyleBold,
		C(darkMode, 0, 81, 72, 156, 240, 225), StringAlignmentCenter);
	Text(g, L"\u9762\u5411 Windows \u8bbe\u5907\u7684\u672c\u5730\u9632\u62a4\u4e0e\u8fd0\u7ef4\u5de5\u5177\uff0c\u96c6\u4e2d\u63d0\u4f9b\u72b6\u6001\u76d1\u63a7\u3001\u8fdc\u7a0b\u884c\u4e3a\u63a7\u5236\u548c\u8bbe\u5907\u8bca\u65ad\u3002", x + 42, y + 215, w - 84, 38, 11, FontStyleRegular, bodyColor);

	const float columnGap = 14.0f;
	const float columnW = (w - 84.0f - columnGap) * 0.5f;
	const float cardsY = y + 274.0f;
	FillRound(g, RectF(x + 42, cardsY, columnW, 154), 8, surface);
	FillRound(g, RectF(x + 42 + columnW + columnGap, cardsY, columnW, 154), 8, surface);
	Text(g, L"\u6838\u5fc3\u529f\u80fd", x + 62, cardsY + 18, columnW - 40, 22, 12, FontStyleBold, titleColor);
	Text(g, L"\u2022 \u5b9e\u65f6\u663e\u793a\u5b66\u751f\u7aef\u8fd0\u884c\u72b6\u6001", x + 62, cardsY + 53, columnW - 40, 20, 10, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u8fdc\u7a0b\u64cd\u4f5c\u3001\u7a97\u53e3\u548c\u8f93\u5165\u9632\u62a4", x + 62, cardsY + 82, columnW - 40, 20, 10, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u89c6\u9891\u6d41\u7167\u7247 / \u89c6\u9891\u66ff\u6362\u6a21\u5f0f", x + 62, cardsY + 111, columnW - 40, 20, 10, FontStyleRegular, bodyColor);
	const float rightX = x + 42 + columnW + columnGap;
	Text(g, L"\u8fd0\u884c\u65b9\u5f0f", rightX + 20, cardsY + 18, columnW - 40, 22, 12, FontStyleBold, titleColor);
	Text(g, L"\u2022 \u539f\u751f Win32 \u754c\u9762\uff0c\u4f4e\u5360\u7528\u3001\u5feb\u901f\u54cd\u5e94", rightX + 20, cardsY + 53, columnW - 40, 20, 10, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 JiYuAvKernel \u63d0\u4f9b\u8fdb\u7a0b\u7279\u5f81\u626b\u63cf\u4e0e\u8bbe\u5907\u8bca\u65ad", rightX + 20, cardsY + 82, columnW - 40, 20, 10, FontStyleRegular, bodyColor);
	Text(g, L"\u2022 \u9632\u62a4\u7b56\u7565\u53ef\u5728\u8fd0\u884c\u4e2d\u4fdd\u5b58\u5e76\u5e94\u7528", rightX + 20, cardsY + 111, columnW - 40, 20, 10, FontStyleRegular, bodyColor);

	FillRound(g, RectF(x + 42, y + 452, w - 84, 74), 8, C(darkMode, 255, 243, 205, 64, 53, 28));
	Text(g, L"\u4f7f\u7528\u63d0\u793a", x + 64, y + 468, 120, 22, 12, FontStyleBold, C(darkMode, 91, 66, 0, 255, 220, 141));
	Text(g, L"\u8bf7\u4ec5\u5728\u5408\u6cd5\u3001\u6388\u6743\u7684\u73af\u5883\u4e2d\u4f7f\u7528\uff0c\u64cd\u4f5c\u524d\u8bf7\u9605\u8bfb\u5e2e\u52a9\u548c\u5f53\u524d\u65e5\u5fd7\u3002", x + 64, y + 500, w - 128, 22, 10, FontStyleRegular, C(darkMode, 91, 70, 20, 232, 211, 162));

	aboutExitRect = { static_cast<LONG>(x + w - 184), static_cast<LONG>(y + h - 56), static_cast<LONG>(x + w - 42), static_cast<LONG>(y + h - 14) };
	FillRound(g, RectF(x + w - 184, y + h - 56, 142, 42), 21,
		Blend(C(darkMode, 255, 231, 226, 83, 45, 41), C(darkMode, 244, 198, 190, 109, 61, 55), hoverProgress[73]));
	DrawIcon(g, 5, x + w - 168, y + h - 45, C(darkMode, 155, 45, 36, 244, 178, 165), 18);
	Text(g, L"\u9000\u51fa\u8f6f\u4ef6", x + w - 140, y + h - 45, 92, 20, 10, FontStyleBold,
		C(darkMode, 155, 45, 36, 244, 178, 165), StringAlignmentCenter);
}

void MainWindow::PaintToast(Graphics& g, int width)
{
	if (toastText.empty() || GetTickCount64() > toastUntil) return;
	const float w = std::min(520.0f, static_cast<float>(width - 320));
	const float x = 220.0f + (static_cast<float>(width - 220) - w) / 2.0f;
	FillRound(g, RectF(x, 84, w, 46), 8, C(darkMode, 47, 54, 51, 218, 226, 222, 248));
	Text(g, toastText.c_str(), x + 18, 97, w - 36, 22, 10, FontStyleRegular, C(darkMode, 255, 255, 255, 36, 43, 40), StringAlignmentCenter);
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

void MainWindow::UpdateHover(POINT point)
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
	if (page == Page::Protection && Hit(advancedSettingsRect, point)) target = 42;
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
		if (Hit(advancedSaveRect, point)) target = 69;
	}
	if (page == Page::Network && Hit(networkProjectRect, point)) target = 72;
	if (page == Page::About) {
		if (Hit(aboutUpdateRect, point)) target = 74;
		if (Hit(aboutExitRect, point)) target = 73;
	}
	if (target != hoverTarget) {
		hoverTarget = target;
		SetCursor(LoadCursorW(nullptr, target >= 0 ? IDC_HAND : IDC_ARROW));
		InvalidateRect(_hWnd, nullptr, FALSE);
	}
}

void MainWindow::HandleClick(POINT point)
{
	if (Hit(superTopMostRect, point)) {
		RequestSuperTopmost();
		return;
	}
	if (Hit(themeRect, point)) {
		darkMode = !darkMode;
		if (controlSurfaceBrush) DeleteObject(controlSurfaceBrush);
		controlSurfaceBrush = CreateSolidBrush(darkMode ? RGB(38, 45, 42) : RGB(255, 255, 255));
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
	if (!JUpdater_CheckInternet()) {
		if (byUser) ShowFastTip(L"\u5f53\u524d\u6ca1\u6709\u53ef\u7528\u7684\u7f51\u7edc\u8fde\u63a5");
		return;
	}

	// 探测放到**工作线程**里跑，原因有两条：
	//
	// 1) 正确性。WinINet 在 GUI 线程上依赖消息泵（内部会创建隐藏窗口、用消息做
	//    异步通知）。如果直接在 UI 线程里同步调用，消息循环被阻塞，连接会直接
	//    失败 —— 实测报 ERROR_INTERNET_CANNOT_CONNECT，而且 PRECONFIG / DIRECT
	//    两档都一样。控制台程序（没有消息循环）同样代码却能正常连通，可佐证。
	// 2) 体验。探测最坏要等几秒，放在 UI 线程会把界面冻住。
	UpdateProbeContext* context = new (std::nothrow) UpdateProbeContext();
	if (!context) {
		if (byUser) ShowFastTip(L"\u5185\u5b58\u4e0d\u8db3\uff0c\u65e0\u6cd5\u68c0\u67e5\u66f4\u65b0");
		return;
	}
	context->window = _hWnd;
	context->byUser = byUser;

	HANDLE thread = CreateThread(nullptr, 0, UpdateProbeThread, context, 0, nullptr);
	if (!thread) {
		delete context;
		if (byUser) ShowFastTip(L"\u65e0\u6cd5\u542f\u52a8\u66f4\u65b0\u68c0\u67e5");
		return;
	}
	CloseHandle(thread);
	if (byUser) ShowFastTip(L"\u6b63\u5728\u68c0\u67e5\u66f4\u65b0\u2026");
}

// 探测结果回到 UI 线程后在这里处理：比对版本、弹确认框、必要时启动更新器。
void MainWindow::FinishUpdateProbe(const UpdateProbeContext& probe)
{
	if (!probe.ok) {
		if (probe.byUser) ShowFastTip(L"\u65e0\u6cd5\u83b7\u53d6\u66f4\u65b0\u4fe1\u606f");
		return;
	}
	const JUpdaterManifestInfo& manifest = probe.manifest;

	const std::wstring current = ReadOwnFileVersion();
	if (!current.empty() && JUpdater_CompareVersion(manifest.version, current.c_str()) <= 0) {
		if (probe.byUser) ShowFastTip(L"\u5f53\u524d\u5df2\u662f\u6700\u65b0\u7248\u672c");
		return;
	}

	std::wstring message;
	message += L"\u53d1\u73b0\u65b0\u7248\u672c\n\n";
	message += L"\u5f53\u524d\u7248\u672c\uff1a";
	message += current.empty() ? L"\u672a\u77e5" : current;
	message += L"\n\u6700\u65b0\u7248\u672c\uff1a";
	message += manifest.version;
	if (manifest.notes[0] != L'\0') {
		message += L"\n\n\u66f4\u65b0\u8bf4\u660e\uff1a\n";
		message += manifest.notes;
	}
	if (manifest.sha256[0] != L'\0') {
		message += L"\n\n\u5b89\u88c5\u5305 SHA-256\uff1a\n";
		message += manifest.sha256;
	}
	message += L"\n\n\u662f\u5426\u73b0\u5728\u66f4\u65b0\uff1f\n"
		L"\uff08\u66f4\u65b0\u7a0b\u5e8f\u4f1a\u63d0\u793a\u4f60\u5148\u5173\u95ed\u672c\u7a0b\u5e8f\uff0c\u518d\u66ff\u6362\u6587\u4ef6\uff09";

	// 主窗口是置顶的，弹窗必须带 MB_TOPMOST，否则会被压在下面。
	if (MessageBoxW(_hWnd, message.c_str(), L"Dzjs Trainer \u66f4\u65b0",
		MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) != IDYES) {
		if (probe.byUser) ShowFastTip(L"\u5df2\u53d6\u6d88\u66f4\u65b0");
		return;
	}

	// The standalone updater owns the rest: download, verification and replacement.
	// It asks the user to close this program before it swaps the executable, so the
	// main program never waits on or exits for it.
	if (!JUpdater_LaunchUpdater()) {
		ShowFastTip(L"\u65e0\u6cd5\u542f\u52a8\u66f4\u65b0\u7a0b\u5e8f");
		return;
	}
	ShowFastTip(L"\u5df2\u542f\u52a8\u66f4\u65b0\u7a0b\u5e8f\uff0c\u8bf7\u5728\u63d0\u793a\u540e\u5173\u95ed\u672c\u7a0b\u5e8f");
}

void MainWindow::SelectPage(Page target)
{
	if (page == target) return;
	page = target;
	// The network page contains a real child HWND; keep its shell and child in
	// one coordinate frame instead of animating the painted shell underneath it.
	pageMotion = target == Page::Network ? 1.0 : 0.0;
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
	advancedControls[0] = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, nullptr,
		WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(IDC_ADV_HOTKEY_FAKE), instance, nullptr);
	advancedControls[1] = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, nullptr,
		WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(IDC_ADV_HOTKEY_SHOW), instance, nullptr);
	advancedControls[2] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"3100",
		WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, _hWnd,
		reinterpret_cast<HMENU>(IDC_ADV_INTERVAL), instance, nullptr);
	for (HWND control : advancedControls)
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
	LoadAdvancedSettings();
	LayoutAdvancedControls();
}

void MainWindow::LayoutAdvancedControls()
{
	if (!advancedControls[0]) return;
	RECT client{};
	GetClientRect(_hWnd, &client);
	const bool showBasic = page == Page::Advanced && advancedTab == 0;
	const bool showCompat = page == Page::Advanced && advancedTab == 1;
	const int contentW = client.right - 276;
	MoveWindow(advancedControls[0], 272, 430, std::max(160, contentW / 2 - 42), 32, TRUE);
	MoveWindow(advancedControls[1], 254 + contentW / 2, 430, std::max(160, contentW / 2 - 42), 32, TRUE);
	MoveWindow(advancedControls[2], client.right - 184, 296, 132, 32, TRUE);
	ShowWindow(advancedControls[0], showBasic ? SW_SHOW : SW_HIDE);
	ShowWindow(advancedControls[1], showBasic ? SW_SHOW : SW_HIDE);
	ShowWindow(advancedControls[2], showCompat ? SW_SHOW : SW_HIDE);
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
	SendMessageW(advancedControls[0], HKM_SETHOTKEY, settings->GetSettingInt(L"HotKeyFakeFull", 1606), 0);
	SendMessageW(advancedControls[1], HKM_SETHOTKEY, settings->GetSettingInt(L"HotKeyShowHide", 1604), 0);
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
	settings->SetSettingInt(L"HotKeyFakeFull", static_cast<int>(SendMessageW(advancedControls[0], HKM_GETHOTKEY, 0, 0)));
	settings->SetSettingInt(L"HotKeyShowHide", static_cast<int>(SendMessageW(advancedControls[1], HKM_GETHOTKEY, 0, 0)));
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
	controlFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	auto make = [this](int index, LPCWSTR cls, LPCWSTR text, DWORD style, int id, DWORD exStyle = 0) {
		networkControls[index] = CreateWindowExW(exStyle, cls, text, WS_CHILD | style,
			0, 0, 10, 10, _hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), JTAppGetInstanceDirect(), nullptr);
		SendMessageW(networkControls[index], WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
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
		ShowWindow(teacherHostWindow, visible ? SW_SHOW : SW_HIDE);
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
	std::lock_guard<std::mutex> guard(stateMutex);
	toastText = StripMarkup(text);
	toastUntil = GetTickCount64() + 4000;
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

void MainWindow::OnUpdateStudentMainInfo(bool running, LPCWSTR fullPath, DWORD pid, bool)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		studentRunning = running;
		studentPid = running ? pid : 0;
		studentPath = running && fullPath ? fullPath : L"Not detected";
	}
	PostMessageW(_hWnd, WM_NATIVE_REFRESH, 0, 0);
}

void MainWindow::OnUpdateState(TrainerStatus status, LPCWSTR textMain, LPCWSTR textMore)
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		currentStatus = status;
		currentControlled = status == TrainerStatusControlled || status == TrainerStatusControlledAndUnLocked;
		statusTitle = textMain ? textMain : L"Unknown status";
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

void MainWindow::OnWmTimer(WPARAM timer)
{
	if (timer == TIMER_ANIMATION) {
		PollEmbeddedTeacherGui();
		PollTeacherService();
		bool needsPaint = false;
		if (pageMotion < 1.0) {
			pageMotion = std::min(1.0, pageMotion + 0.045);
			needsPaint = true;
		}
		for (int i = 0; i < static_cast<int>(hoverProgress.size()); ++i) {
			const float target = hoverTarget == i ? 1.0f : 0.0f;
			if (fabs(hoverProgress[i] - target) >= 0.002f) {
				hoverProgress[i] += (target - hoverProgress[i]) * 0.24f;
				needsPaint = true;
			}
			else hoverProgress[i] = target;
		}
		if (!toastText.empty() && GetTickCount64() > toastUntil) {
			toastText.clear();
			needsPaint = true;
		}
		if (needsPaint && IsWindowVisible(_hWnd) && !IsIconic(_hWnd))
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
