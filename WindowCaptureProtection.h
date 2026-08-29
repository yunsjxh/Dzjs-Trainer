#pragma once

#include <windows.h>
#include <cstring>

namespace JiYuWindowCapture {

using SetWindowDisplayAffinityFunction = BOOL(WINAPI*)(HWND, DWORD);

inline SetWindowDisplayAffinityFunction ResolveSetWindowDisplayAffinity()
{
	static SetWindowDisplayAffinityFunction function = [] {
		const HMODULE user32 = GetModuleHandleW(L"user32.dll");
		const FARPROC address = user32 ? GetProcAddress(user32, "SetWindowDisplayAffinity") : nullptr;
		SetWindowDisplayAffinityFunction resolved = nullptr;
		if (address) {
			static_assert(sizeof(resolved) == sizeof(address));
			std::memcpy(&resolved, &address, sizeof(resolved));
		}
		return resolved;
	}();
	return function;
}

inline void ExcludeWindowFromCapture(HWND window)
{
	if (!window || !IsWindow(window)) return;
	if (SetWindowDisplayAffinityFunction setAffinity = ResolveSetWindowDisplayAffinity()) {
		// WDA_EXCLUDEFROMCAPTURE is supported by Windows 10 version 2004+.
		// Keep this best-effort so older systems retain normal window behavior.
		setAffinity(window, 0x11);
	}
}

}
