#include "HooksCommon.h"
#include "GuardInput.h"

// ===== GuardInput ============================================================
// 从 DzjsTrainerHooks.cpp 机械迁出，函数体逐字节保持原样。

HHOOK hMouseHook;
HHOOK hKeyboardHook;
HOOKPROC lpMouseHookfn = NULL;
HOOKPROC lpKeyboardHookfn = NULL;

LRESULT WINAPI LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = faCallNextHookEx(hMouseHook, nCode, wParam, lParam);
	if (lpMouseHookfn)
		lpMouseHookfn(nCode, wParam, lParam);
	return result;
}

LRESULT WINAPI LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = faCallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	if (lpKeyboardHookfn)
		lpKeyboardHookfn(nCode, wParam, lParam);
	return result;
}

VOID WINAPI hkmouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData,  ULONG_PTR dwExtraInfo)
{
	VTraceHook(L"hkmouse_event");
	// 只记录：allowControl 为真（默认）时不干预，只在日志里留一条。
	if (!allowControl) {
		VRecordHook(L"mouse_event", L"flags=0x%lX dx=%ld dy=%ld", dwFlags, dx, dy);
		return;
	}
	famouse_event(dwFlags, dx, dy, dwData, dwExtraInfo);
}

UINT WINAPI hkSendInput(UINT cInputs, LPINPUT pInputs, int cbSize)
{
	VTraceHook(L"hkSendInput");
	if (!allowControl && loaded) {
		VRecordHook(L"SendInput", L"count=%u", cInputs);
		return cInputs;
	}
	return faSendInput(cInputs, pInputs, cbSize);
}

// A/W 两个版本的参数完全一致（W/A 的差别只在没有 hmod 的那个重载），
// 所以共用一份实现，只把"调哪个原始函数"和"日志里写哪个名字"参数化。
// 原来只钩了 A 版，但实际调用方绝大多数走 W 版 —— 等于漏了一半。
static HHOOK SetWindowsHookExImpl(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId, bool wide)
{
	VTraceHook(wide ? L"hkSetWindowsHookExW" : L"hkSetWindowsHookExA");
	const LPCWSTR name = wide ? L"SetWindowsHookExW" : L"SetWindowsHookExA";
	LONG logIndex = InterlockedIncrement(&inputHookLogCount);
	if (logIndex <= 20)
		VDebugLog(L"%s call index=%ld idHook=%d callback=%p module=%p thread=%lu loaded=%d",
			name, logIndex, idHook, lpfn, hmod, dwThreadId, loaded);

	// 只记录：允许教师控制（默认）时不接管极域的输入钩子，原样放行。
	if (allowControl) {
		VRecordHook(name, L"idHook=%d module=%p thread=%lu", idHook, hmod, dwThreadId);
		return wide ? faSetWindowsHookExW(idHook, lpfn, hmod, dwThreadId)
		            : faSetWindowsHookExA(idHook, lpfn, hmod, dwThreadId);
	}
	if (loaded)
	{
		if (idHook == WH_CBT) {
			if (logIndex <= 20) VDebugLog(L"%s rejected reason=WH_CBT", name);
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		if (idHook == WH_MOUSE_LL || idHook == WH_MOUSE || idHook == WH_KEYBOARD_LL) {
			if (hmod == hLibTDMaster) {
				switch (idHook)
				{
				case WH_MOUSE_LL:
					lpMouseHookfn = lpfn;
					hMouseHook = faSetWindowsHookExA(idHook, LowLevelMouseProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_MOUSE_LL hooked 0x%08x", hMouseHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_MOUSE;
				case WH_MOUSE:
					lpMouseHookfn = lpfn;
					hMouseHook = faSetWindowsHookExA(idHook, LowLevelMouseProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_MOUSE hooked 0x%08x", hMouseHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_MOUSE;
				case WH_KEYBOARD_LL:
					lpKeyboardHookfn = lpfn;
					hKeyboardHook = faSetWindowsHookExA(idHook, LowLevelKeyboardProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_KEYBOARD_LL hooked 0x%08x", hKeyboardHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_KEYBOARD;
				}
			}
			if (logIndex <= 20) VDebugLog(L"%s rejected reason=input-hook-module-mismatch", name);
			return FALSE;
		}
	}
	return wide ? faSetWindowsHookExW(idHook, lpfn, hmod, dwThreadId)
	            : faSetWindowsHookExA(idHook, lpfn, hmod, dwThreadId);
}

HHOOK WINAPI hkSetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId)
{
	return SetWindowsHookExImpl(idHook, lpfn, hmod, dwThreadId, false);
}

HHOOK WINAPI hkSetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId)
{
	return SetWindowsHookExImpl(idHook, lpfn, hmod, dwThreadId, true);
}

LRESULT WINAPI hkCallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam) 
{
	VTraceHook(L"hkCallNextHookEx");
	if (hhk == (HHOOK)INVALID_HHOOK_MOUSE) return 0;
	if (hhk == (HHOOK)INVALID_HHOOK_KEYBOARD) return 0;
	return faCallNextHookEx(hhk, nCode, wParam, lParam);
}

BOOL WINAPI hkUnhookWindowsHookEx(HHOOK hhk)
{
	VTraceHook(L"hkUnhookWindowsHookEx");
	if (hhk == (HHOOK)INVALID_HHOOK_MOUSE) {
		BOOL rs = faUnhookWindowsHookEx(hMouseHook);
		hMouseHook = NULL;
		VOutPutStatus(L"[K] hMouseHook Unkooked !");
		return rs;
	}
	if (hhk == (HHOOK)INVALID_HHOOK_KEYBOARD) {
		BOOL rs = faUnhookWindowsHookEx(hKeyboardHook);
		hMouseHook = hKeyboardHook;
		VOutPutStatus(L"[K] hKeyboardHook Unkooked !");
		return rs;
	}
	return faUnhookWindowsHookEx(hhk);
}
