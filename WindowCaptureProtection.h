#pragma once

#include <windows.h>
#include <cstring>
#include <string>

// 窗口在抓屏里的表现，由 ini 的 [JTSettings] CaptureExclusion 决定：
//   1 = 透过（默认）  抓屏里看不到这个窗口，看到的是它背后的画面
//   2 = 黑块          抓屏里是一块纯黑（WDA_MONITOR）
//   0 = 正常显示      不设亲和性，截图能拍到界面本身
//
// 0x11（WDA_EXCLUDEFROMCAPTURE）只有 Win10 2004（build 19041）以后才是「完全不
// 出现」。更早的系统会把它当 0x01 处理，结果就是一块黑。所以策略 1 在旧系统上
// 直接退成 0：宁可露出界面，也不留一块黑。
namespace JiYuWindowCapture {

constexpr DWORD kSeeThrough = 0x00000011; // WDA_EXCLUDEFROMCAPTURE
constexpr DWORD kBlackBlock = 0x00000001; // WDA_MONITOR
constexpr DWORD kVisible = 0x00000000;    // WDA_NONE

using SetWindowDisplayAffinityFunction = BOOL(WINAPI*)(HWND, DWORD);
using GetWindowDisplayAffinityFunction = BOOL(WINAPI*)(HWND, DWORD*);

inline FARPROC ResolveUser32Export(const char* name)
{
	const HMODULE user32 = GetModuleHandleW(L"user32.dll");
	return user32 ? GetProcAddress(user32, name) : nullptr;
}

inline SetWindowDisplayAffinityFunction ResolveSetWindowDisplayAffinity()
{
	static SetWindowDisplayAffinityFunction function = [] {
		const FARPROC address = ResolveUser32Export("SetWindowDisplayAffinity");
		SetWindowDisplayAffinityFunction resolved = nullptr;
		if (address) {
			static_assert(sizeof(resolved) == sizeof(address));
			std::memcpy(&resolved, &address, sizeof(resolved));
		}
		return resolved;
	}();
	return function;
}

inline GetWindowDisplayAffinityFunction ResolveGetWindowDisplayAffinity()
{
	static GetWindowDisplayAffinityFunction function = [] {
		const FARPROC address = ResolveUser32Export("GetWindowDisplayAffinity");
		GetWindowDisplayAffinityFunction resolved = nullptr;
		if (address) {
			static_assert(sizeof(resolved) == sizeof(address));
			std::memcpy(&resolved, &address, sizeof(resolved));
		}
		return resolved;
	}();
	return function;
}

struct OsVersionNumber
{
	DWORD major;
	DWORD minor;
	DWORD build;
};

// RtlGetVersion 不受兼容性清单影响，比 GetVersionEx 可靠。
inline OsVersionNumber QueryOsVersion()
{
	static const OsVersionNumber version = [] {
		OsVersionNumber result{ 0, 0, 0 };
		using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
		RtlGetVersionFunction function = nullptr;
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (ntdll) {
			const FARPROC address = GetProcAddress(ntdll, "RtlGetVersion");
			if (address) {
				static_assert(sizeof(function) == sizeof(address));
				std::memcpy(&function, &address, sizeof(function));
			}
		}
		OSVERSIONINFOW info{};
		info.dwOSVersionInfoSize = sizeof(info);
		if (function && function(&info) == 0) {
			result.major = info.dwMajorVersion;
			result.minor = info.dwMinorVersion;
			result.build = info.dwBuildNumber;
		}
		return result;
	}();
	return version;
}

inline bool SeeThroughSupported()
{
	static const bool supported = [] {
		const OsVersionNumber version = QueryOsVersion();
		return version.major > 10 || (version.major == 10 && version.build >= 19041);
	}();
	return supported;
}

inline const wchar_t* OwnIniPath()
{
	static const std::wstring path = [] {
		wchar_t buffer[MAX_PATH] = {};
		if (!GetModuleFileNameW(nullptr, buffer, MAX_PATH)) return std::wstring();
		wchar_t* slash = wcsrchr(buffer, L'\\');
		if (!slash) return std::wstring();
		*(slash + 1) = L'\0';
		std::wstring full(buffer);
		full += L"DzjsTrainer.ini";
		return full;
	}();
	return path.c_str();
}

// 每次进程只读一次：改 ini 后重启生效。
inline int CapturePolicy()
{
	static const int policy = [] {
		int value = GetPrivateProfileIntW(L"JTSettings", L"CaptureExclusion", 1, OwnIniPath());
		if (value != 0 && value != 1 && value != 2) value = 1;
		return value;
	}();
	return policy;
}

inline DWORD CurrentAffinity(HWND window)
{
	DWORD value = kVisible;
	if (GetWindowDisplayAffinityFunction get = ResolveGetWindowDisplayAffinity()) {
		if (!get(window, &value)) value = kVisible;
	}
	return value;
}

// 返回实际生效的亲和性，调用方拿它写日志。
inline DWORD ApplyCapturePolicy(HWND window)
{
	if (!window || !IsWindow(window)) return kVisible;
	const SetWindowDisplayAffinityFunction set = ResolveSetWindowDisplayAffinity();
	if (!set) return kVisible;

	DWORD wanted = kVisible;
	switch (CapturePolicy()) {
	case 2:
		wanted = kBlackBlock;
		break;
	case 1:
		wanted = SeeThroughSupported() ? kSeeThrough : kVisible;
		break;
	default:
		wanted = kVisible;
		break;
	}
	if (wanted == kVisible) return kVisible;

	set(window, wanted);
	// 窗口还没被合成时首次设置可能不生效，读回来对不上就再设一次。
	if (CurrentAffinity(window) != wanted) set(window, wanted);
	return CurrentAffinity(window);
}

}
