#include "stdafx.h"
#include "MsgCenter.h"
#include <list>

std::list<HWND> jiyuWnds;

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
