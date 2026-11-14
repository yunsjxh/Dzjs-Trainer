#pragma once
// GuardWindow: 该域的 detour 实现。共享状态见 HooksCommon.h，安装顺序见 HookRegistry.cpp。
#include "HooksCommon.h"

BOOL WINAPI hkSetForegroundWindow(HWND hWnd);
BOOL WINAPI hkSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags);
HDWP WINAPI hkDeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags);
BOOL WINAPI hkMoveWindow(HWND hWnd, int x, int y, int cx, int cy, BOOL bRepaint);
BOOL WINAPI hkBringWindowToTop(HWND hWnd);
LONG WINAPI hkSetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong);
LONG WINAPI hkSetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong);
BOOL WINAPI hkShowWindow(HWND hWnd, int nCmdShow);
BOOL WINAPI hkEnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable);
DWORD WINAPI hkSetClassLongA(HWND hWnd, int nIndex, LONG dwNewLong);
DWORD WINAPI hkSetClassLongW(HWND hWnd, int nIndex, LONG dwNewLong);
HWND WINAPI hkGetForegroundWindow(VOID);
HWND WINAPI hkGetDesktopWindow(VOID);
HDC WINAPI hkGetWindowDC(__in_opt HWND hWnd);
HDC WINAPI hkCreateDCW(LPCWSTR pwszDriver, LPCWSTR pwszDevice, LPCWSTR pszPort, const DEVMODEW * pdm);
HRESULT WINAPI hkDwmEnableComposition(UINT uCompositionAction);
// 截屏另外三条路（Display 域，实现放在 GuardWindow.cpp 与既有 DC 钩同处）。
BOOL WINAPI hkPrintWindow(HWND hwnd, HDC hdcBlt, UINT nFlags);
int  WINAPI hkGetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT usage);
BOOL WINAPI hkStretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop);
LONG WINAPI hkChangeDisplaySettingsW(DEVMODEW* lpDevMode, DWORD dwFlags);
HDESK WINAPI hkOpenDesktopA(LPCSTR lpszDesktop, DWORD dwFlags, BOOL fInherit, ACCESS_MASK dwDesiredAccess);
HDESK WINAPI hkOpenInputDesktop(DWORD dwFlags,BOOL fInherit, ACCESS_MASK dwDesiredAccess);

namespace GuardWindow {
    constexpr HookDomain Domain = HookDomain::Window;
    int HookCount();   // 已安装的钩子数，由 HookRegistry.cpp 按域统计
    // 这次调用是不是在读整个桌面。认两种源：WindowFromDC == 桌面窗口的窗口 DC，
    // 以及 CreateDCW(L"DISPLAY") 出来的显示 DC（WindowFromDC 对它返回 NULL）。
    // 必须连目的端一起判 —— 显示 DC 也是合法的绘制目标，只看源会把宿主自己的
    // 画面贴掉。三个截屏钩子与 BitBlt 帧替换都走它，定义在 GuardWindow.cpp。
    bool IsScreenCaptureInto(HDC src, HDC dest);
    // GetDIBits 专用：hdc 只是格式上下文，像素来自 hbm，所以要按位图尺寸把关。
    bool IsScreenDibSource(HDC hdc, HBITMAP hbm);
}
