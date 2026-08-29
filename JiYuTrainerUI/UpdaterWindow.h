#pragma once

#include "stdafx.h"
#include <string>

class UpdaterWindow
{
public:
	explicit UpdaterWindow(HWND parentHWnd);
	~UpdaterWindow();
	int RunLoop();

private:
	HWND hWnd = nullptr;
	HWND statusLabel = nullptr;
	HWND progressBar = nullptr;
	HWND progressLabel = nullptr;
	HFONT font = nullptr;
	std::wstring pendingPercent = L"0%";
	int pendingStatus = 0;

	static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static void UpdateDownloadCallback(LPCWSTR percent, LPARAM lParam, int status);
	void OnUpdate(LPCWSTR percent, int status);
	void ApplyProgress();
	void Cancel();
};
