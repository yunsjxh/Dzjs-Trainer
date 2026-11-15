#include "stdafx.h"
#include "RunEnvironment.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct OsInfo
{
	DWORD major;
	DWORD minor;
	DWORD build;
	DWORD revision; // 注册表里的 UBR，累计补丁号；旧系统没有这项，为 0
	bool server;
};

struct Lines
{
	std::wstring summary;
	std::wstring detail;
};

template <typename Function>
Function ResolveExport(const wchar_t* moduleName, const char* name)
{
	Function function = nullptr;
	const HMODULE module = GetModuleHandleW(moduleName);
	if (!module) return function;
	const FARPROC address = GetProcAddress(module, name);
	if (!address) return function;
	static_assert(sizeof(function) == sizeof(address));
	std::memcpy(&function, &address, sizeof(function));
	return function;
}

// RtlGetVersion 不受兼容性清单影响，比 GetVersionEx 可靠。
OsInfo QueryOs()
{
	OsInfo info{ 0, 0, 0, 0, false };

	using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOEXW*);
	const RtlGetVersionFunction rtlGetVersion = ResolveExport<RtlGetVersionFunction>(L"ntdll.dll", "RtlGetVersion");
	OSVERSIONINFOEXW version{};
	version.dwOSVersionInfoSize = sizeof(version);
	if (rtlGetVersion && rtlGetVersion(&version) == 0) {
		info.major = version.dwMajorVersion;
		info.minor = version.dwMinorVersion;
		info.build = version.dwBuildNumber;
		// VER_NT_WORKSTATION 之外都按服务器版显示，包括域控制器。
		info.server = version.wProductType != VER_NT_WORKSTATION;
	}
	return info;
}

// UBR 只在 64 位视图里是全的，32 位进程直接读会拿到另一个值甚至读不到。
DWORD QueryRevision()
{
	HKEY key = nullptr;
	const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
		0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
	if (opened != ERROR_SUCCESS) return 0;

	DWORD value = 0;
	DWORD size = sizeof(value);
	DWORD type = 0;
	const LONG queried = RegQueryValueExW(key, L"UBR", nullptr, &type,
		reinterpret_cast<LPBYTE>(&value), &size);
	RegCloseKey(key);
	if (queried != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return 0;
	return value;
}

const wchar_t* ArchitectureName(WORD machine)
{
	switch (machine) {
	case IMAGE_FILE_MACHINE_AMD64: return L"x64";
	case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
	case IMAGE_FILE_MACHINE_I386: return L"x86";
	case IMAGE_FILE_MACHINE_ARMNT: return L"ARM";
	default: return L"未知";
	}
}

// native 是系统本身的架构，process 是这个进程的架构。训练器是 32 位程序，
// 在 64 位系统上两者不同 —— 这个差异会直接影响注入和抓屏的行为。
void QueryArchitecture(WORD* native, WORD* process)
{
	*native = IMAGE_FILE_MACHINE_UNKNOWN;
	*process = IMAGE_FILE_MACHINE_UNKNOWN;

	using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
	const IsWow64Process2Function isWow64Process2 =
		ResolveExport<IsWow64Process2Function>(L"kernel32.dll", "IsWow64Process2");
	USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
	USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
	if (isWow64Process2 && isWow64Process2(GetCurrentProcess(), &nativeMachine, &processMachine)) {
		*native = nativeMachine;
		// processMachine 为 UNKNOWN 表示进程就是原生位数，不是被 WOW64 转译的。
		*process = processMachine != IMAGE_FILE_MACHINE_UNKNOWN ? processMachine : nativeMachine;
		return;
	}

	// Win10 1511 以前没有 IsWow64Process2，退回「是否 WOW64 + 系统处理器架构」。
	SYSTEM_INFO info{};
	GetNativeSystemInfo(&info);
	switch (info.wProcessorArchitecture) {
	case PROCESSOR_ARCHITECTURE_AMD64: *native = IMAGE_FILE_MACHINE_AMD64; break;
	case PROCESSOR_ARCHITECTURE_ARM64: *native = IMAGE_FILE_MACHINE_ARM64; break;
	case PROCESSOR_ARCHITECTURE_INTEL: *native = IMAGE_FILE_MACHINE_I386; break;
	default: break;
	}
	if (*native == IMAGE_FILE_MACHINE_UNKNOWN) {
		*native = sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
	}
	*process = sizeof(void*) == 8 ? *native : IMAGE_FILE_MACHINE_I386;
}

// 提权与否看的是当前令牌。属于管理员组但 UAC 未提权时，令牌里的
// Administrators 是 deny-only，这里会返回 false，正好是我们要的语义。
bool IsElevated()
{
	BOOL member = FALSE;
	SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
	PSID administrators = nullptr;
	if (!AllocateAndInitializeSid(&authority, 2,
			SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
			0, 0, 0, 0, 0, 0, &administrators)) {
		return false;
	}
	CheckTokenMembership(nullptr, administrators, &member);
	FreeSid(administrators);
	return member != FALSE;
}

// UIAccess 决定本进程能不能盖住更高完整性级别的窗口，也决定它自己的
// SetWindowDisplayAffinity 有没有额外限制。
bool HasUiAccess()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	DWORD enabled = 0;
	DWORD size = 0;
	const BOOL ok = GetTokenInformation(token, TokenUIAccess, &enabled, sizeof(enabled), &size);
	CloseHandle(token);
	return ok && enabled != 0;
}

DWORD QuerySessionId()
{
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	return session;
}

const wchar_t* OsName(const OsInfo& os)
{
	if (os.major == 10) {
		if (os.server) {
			if (os.build >= 26100) return L"Windows Server 2025";
			if (os.build >= 20348) return L"Windows Server 2022";
			if (os.build >= 17763) return L"Windows Server 2019";
			return L"Windows Server 2016";
		}
		return os.build >= 22000 ? L"Windows 11" : L"Windows 10";
	}
	if (os.major == 6 && os.minor == 3) return os.server ? L"Windows Server 2012 R2" : L"Windows 8.1";
	if (os.major == 6 && os.minor == 2) return os.server ? L"Windows Server 2012" : L"Windows 8";
	if (os.major == 6 && os.minor == 1) return os.server ? L"Windows Server 2008 R2" : L"Windows 7";
	if (os.major == 6 && os.minor == 0) return os.server ? L"Windows Server 2008" : L"Windows Vista";
	return L"Windows";
}

Lines Build()
{
	Lines lines;

	OsInfo os = QueryOs();
	os.revision = QueryRevision();

	WORD native = IMAGE_FILE_MACHINE_UNKNOWN;
	WORD process = IMAGE_FILE_MACHINE_UNKNOWN;
	QueryArchitecture(&native, &process);

	wchar_t buffer[128] = {};

	lines.summary = OsName(os);
	// 带上 UBR 才能区分同一个 build 上不同补丁级别的机器，排查时用得到。
	if (os.revision > 0) swprintf_s(buffer, L" %lu.%lu", os.build, os.revision);
	else swprintf_s(buffer, L" %lu", os.build);
	lines.summary += buffer;
	lines.summary += L" · ";
	lines.summary += ArchitectureName(native);
	lines.summary += IsElevated() ? L" · 管理员" : L" · 标准用户";
	if (HasUiAccess()) lines.summary += L"+UIAccess";

	lines.detail = lines.summary;
	if (process != native) {
		lines.detail += L" · 进程 ";
		lines.detail += ArchitectureName(process);
	}
	lines.detail += GetSystemMetrics(SM_REMOTESESSION) ? L" · 远程会话" : L" · 本地会话";
	swprintf_s(buffer, L" · 会话 %lu", QuerySessionId());
	lines.detail += buffer;
	return lines;
}

const Lines& Current()
{
	// 进程内只算一次：PaintAbout 跑在动画定时器上，注册表和令牌查询没必要重复做。
	static const Lines lines = Build();
	return lines;
}

}  // namespace

const wchar_t* JTRunEnvSummary() { return Current().summary.c_str(); }
const wchar_t* JTRunEnvDetail() { return Current().detail.c_str(); }
