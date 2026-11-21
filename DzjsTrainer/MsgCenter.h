#pragma once
#include "stdafx.h"

struct MsgCenterSendResult {
	bool windowFound;
	bool delivered;
	DWORD error;
	ULONGLONG elapsedMs;
};

MsgCenterSendResult MsgCenterSendToVirus(LPCWSTR buff, HWND form);
// 广播给注入到第三方进程里的 hooks 实例（窗口标题见 EduProducts.h 的
// EDZU_CUSTOM_HOST_WINDOW_TITLE）。返回实际投递成功的窗口数。
int MsgCenterSendToCustomHosts(LPCWSTR buff, HWND form);
void MsgCenteAppendHWND(HWND hWnd);
void MsgCenterSendHWNDS(HWND fromHWnd);
