#include "stdafx.h"
#include "MsgCenter.h"
#include "EduProducts.h"
#include <list>

std::list<HWND> jiyuWnds;

namespace {

struct CustomHostBroadcast {
	LPCWSTR command;
	HWND form;
	int matched;
	int delivered;
};

BOOL CALLBACK EnumCustomHostWindow(HWND hWnd, LPARAM lParam)
{
	CustomHostBroadcast* ctx = reinterpret_cast<CustomHostBroadcast*>(lParam);
	// 先比标题：桌面上绝大多数窗口在这里就被筛掉，省掉后面的跨进程调用。
	WCHAR title[64] = {};
	if (GetWindowTextW(hWnd, title, _countof(title)) == 0) return TRUE;
	if (wcscmp(title, EDZU_CUSTOM_HOST_WINDOW_TITLE) != 0) return TRUE;

	COPYDATASTRUCT copyData = { 0 };
	copyData.lpData = (PVOID)ctx->command;
	copyData.cbData = sizeof(WCHAR) * (wcslen(ctx->command) + 1);
	DWORD_PTR response = 0;
	++ctx->matched;
	// 超时和极域那条通道取同一个值：对端在装钩子/读 ini，偶尔会慢一点，
	// 但不能因为一个卡死的宿主把整个设置界面拖住。
	if (SendMessageTimeoutW(hWnd, WM_COPYDATA, (WPARAM)ctx->form, (LPARAM)&copyData,
		SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &response) != 0)
		++ctx->delivered;
	return TRUE;
}

} // namespace

int MsgCenterSendToCustomHosts(LPCWSTR buff, HWND form)
{
	if (!buff) return 0;
	CustomHostBroadcast ctx = { buff, form, 0, 0 };
	EnumWindows(EnumCustomHostWindow, reinterpret_cast<LPARAM>(&ctx));
	return ctx.delivered;
}

MsgCenterSendResult MsgCenterSendToVirus(LPCWSTR buff, HWND form)
{
	MsgCenterSendResult result = { false, false, ERROR_SUCCESS, 0 };
	HWND receiveWindow = FindWindow(NULL, L"JiYu Trainer Virus Window");
	if (receiveWindow) {
		result.windowFound = true;
		COPYDATASTRUCT copyData = { 0 };
		copyData.lpData = (PVOID)buff;
		copyData.cbData = sizeof(WCHAR) * (wcslen(buff) + 1);
		DWORD_PTR response = 0;
		SetLastError(ERROR_SUCCESS);
		ULONGLONG started = GetTickCount64();
		result.delivered = SendMessageTimeoutW(receiveWindow, WM_COPYDATA, (WPARAM)form,
			(LPARAM)&copyData, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &response) != 0;
		result.elapsedMs = GetTickCount64() - started;
		result.error = GetLastError();
	}
	else
		result.error = ERROR_FILE_NOT_FOUND;
	return result;
}

bool IsInIllegalWindows(HWND hWnd) {
	std::list<HWND>::iterator testiterator;
	for (testiterator = jiyuWnds.begin(); testiterator != jiyuWnds.end(); ++testiterator)
	{
		if ((*testiterator) == hWnd)
			return true;
	}
	return false;
}
void MsgCenteAppendHWND(HWND hWnd)
{
	if (!IsInIllegalWindows(hWnd)) jiyuWnds.push_back(hWnd);
}
void MsgCenterSendHWNDS(HWND fromHWnd)
{
	int iwndCount = jiyuWnds.size();
	std::list<HWND>::iterator testiterator;
	for (testiterator = jiyuWnds.begin(); testiterator != jiyuWnds.end(); ++testiterator)
	{
		HWND hWnd = (*testiterator);
		WCHAR str[65];
		swprintf_s(str, L"hw:%d", (LONG)hWnd);
		MsgCenterSendToVirus(str, fromHWnd);
	}
	jiyuWnds.clear();
	//ResetGBStatus(iwndCount, lastHasGb, lastHasHp);
}
