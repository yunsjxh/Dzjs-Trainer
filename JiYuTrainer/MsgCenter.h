#pragma once
#include "stdafx.h"

struct MsgCenterSendResult {
	bool windowFound;
	bool delivered;
	DWORD error;
	ULONGLONG elapsedMs;
};

MsgCenterSendResult MsgCenterSendToVirus(LPCWSTR buff, HWND form);
void MsgCenteAppendHWND(HWND hWnd);
void MsgCenterSendHWNDS(HWND fromHWnd);
