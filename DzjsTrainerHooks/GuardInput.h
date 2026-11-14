#pragma once
// GuardInput: 该域的 detour 实现。共享状态见 HooksCommon.h，安装顺序见 HookRegistry.cpp。
#include "HooksCommon.h"

LRESULT WINAPI LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
VOID WINAPI hkmouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData,  ULONG_PTR dwExtraInfo);
UINT WINAPI hkSendInput(UINT cInputs, LPINPUT pInputs, int cbSize);
HHOOK WINAPI hkSetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
HHOOK WINAPI hkSetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
LRESULT WINAPI hkCallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam);
BOOL WINAPI hkUnhookWindowsHookEx(HHOOK hhk);

namespace GuardInput {
    constexpr HookDomain Domain = HookDomain::Input;
    int HookCount();   // 已安装的钩子数，由 HookRegistry.cpp 按域统计
}
