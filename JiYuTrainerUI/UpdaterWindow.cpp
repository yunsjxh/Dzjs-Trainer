#include "stdafx.h"
#include "UpdaterWindow.h"
#include "../WindowCaptureProtection.h"
#include "resource.h"
#include "../JiYuTrainerUpdater/JiYuTrainerUpdater.h"
#include <CommCtrl.h>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

namespace {
constexpr UINT WM_UPDATE_PROGRESS = WM_APP + 42;
constexpr int IDC_UPDATE_CANCEL = 43001;

struct UpdatePayload {
	std::wstring percent;
	int status = 0;
};
}

UpdaterWindow::UpdaterWindow(HWND parentHWnd)
{
	HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
	wc.hbrBackground = CreateSolidBrush(RGB(246, 248, 246));
	wc.lpszClassName = L"JiYuTrainerNativeUpdater";
	RegisterClassExW(&wc);

	hWnd = CreateWindowExW(0, wc.lpszClassName, L"Dzjs Trainer \u66f4\u65b0",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 470, 230,
		parentHWnd, nullptr, instance, this);
	if (!hWnd) return;
	font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	statusLabel = CreateWindowExW(0, L"STATIC", L"\u6b63\u5728\u51c6\u5907\u66f4\u65b0...", WS_CHILD | WS_VISIBLE,
		28, 30, 390, 28, hWnd, nullptr, instance, nullptr);
	progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
		28, 78, 390, 14, hWnd, nullptr, instance, nullptr);
	progressLabel = CreateWindowExW(0, L"STATIC", L"0%", WS_CHILD | WS_VISIBLE | SS_RIGHT,
		348, 100, 70, 24, hWnd, nullptr, instance, nullptr);
	HWND cancel = CreateWindowExW(0, L"BUTTON", L"\u53d6\u6d88", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		338, 140, 80, 34, hWnd, reinterpret_cast<HMENU>(IDC_UPDATE_CANCEL), instance, nullptr);
	for (HWND control : { statusLabel, progressBar, progressLabel, cancel })
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	SendMessageW(progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
	RECT window{};
	GetWindowRect(hWnd, &window);
	const int screenW = GetSystemMetrics(SM_CXSCREEN);
	const int screenH = GetSystemMetrics(SM_CYSCREEN);
	SetWindowPos(hWnd, nullptr, (screenW - (window.right - window.left)) / 2,
		(screenH - (window.bottom - window.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);
	JUpdater_DownLoadUpdateFile(UpdateDownloadCallback, reinterpret_cast<LPARAM>(this));
}

UpdaterWindow::~UpdaterWindow()
{
	if (font) DeleteObject(font);
}

int UpdaterWindow::RunLoop()
{
	if (!hWnd) return -1;
	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return static_cast<int>(message.wParam);
}

LRESULT CALLBACK UpdaterWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	UpdaterWindow* self = reinterpret_cast<UpdaterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
		self = static_cast<UpdaterWindow*>(create->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		JiYuWindowCapture::ExcludeWindowFromCapture(hwnd);
	}
	switch (message) {
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_UPDATE_CANCEL && self) self->Cancel();
		return 0;
	case WM_CLOSE:
		if (self) self->Cancel();
		return 0;
	case WM_UPDATE_PROGRESS: {
		auto payload = reinterpret_cast<UpdatePayload*>(lParam);
		if (self && payload) self->OnUpdate(payload->percent.c_str(), payload->status);
		delete payload;
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

void UpdaterWindow::UpdateDownloadCallback(LPCWSTR percent, LPARAM lParam, int status)
{
	auto self = reinterpret_cast<UpdaterWindow*>(lParam);
	if (!self || !self->hWnd) return;
	auto payload = new UpdatePayload{ percent ? percent : L"0%", status };
	PostMessageW(self->hWnd, WM_UPDATE_PROGRESS, 0, reinterpret_cast<LPARAM>(payload));
}

void UpdaterWindow::OnUpdate(LPCWSTR percent, int status)
{
	if (status == UPDATE_STATUS_DWONLAODING) {
		SetWindowTextW(statusLabel, L"\u6b63\u5728\u4e0b\u8f7d\u66f4\u65b0...");
		SetWindowTextW(progressLabel, percent);
		int value = _wtoi(percent);
		const int progressValue = value < 0 ? 0 : (value > 100 ? 100 : value);
		SendMessageW(progressBar, PBM_SETPOS, progressValue, 0);
	}
	else if (status == UPDATE_STATUS_COULD_NOT_CONNECT) {
		SetWindowTextW(statusLabel, L"\u8fde\u63a5\u66f4\u65b0\u670d\u52a1\u5931\u8d25");
		MessageBoxW(hWnd, L"\u65e0\u6cd5\u8fde\u63a5\u66f4\u65b0\u670d\u52a1\u5668\u3002", L"Dzjs Trainer", MB_OK | MB_ICONWARNING);
		DestroyWindow(hWnd);
	}
	else if (status == UPDATE_STATUS_COULD_NOT_CREATE_FILE) {
		SetWindowTextW(statusLabel, L"\u65e0\u6cd5\u5199\u5165\u66f4\u65b0\u6587\u4ef6");
		MessageBoxW(hWnd, L"\u8bf7\u4f7f\u7528\u7ba1\u7406\u5458\u6743\u9650\u91cd\u8bd5\u3002", L"Dzjs Trainer", MB_OK | MB_ICONWARNING);
		DestroyWindow(hWnd);
	}
	else if (status == UPDATE_STATUS_FINISHED) {
		SetWindowTextW(statusLabel, L"\u66f4\u65b0\u4e0b\u8f7d\u5b8c\u6210");
		SendMessageW(progressBar, PBM_SETPOS, 100, 0);
		SetWindowTextW(progressLabel, L"100%");
		if (JUpdater_RunInstallion()) DestroyWindow(hWnd);
	}
}

void UpdaterWindow::Cancel()
{
	if (JUpdater_Updatering() && MessageBoxW(hWnd, L"\u66f4\u65b0\u6b63\u5728\u4e0b\u8f7d\uff0c\u786e\u5b9a\u53d6\u6d88\uff1f", L"Dzjs Trainer",
		MB_YESNO | MB_ICONQUESTION) != IDYES) return;
	if (JUpdater_Updatering()) JUpdater_CancelDownLoadUpdateFile();
	DestroyWindow(hWnd);
}

void UpdaterWindow::ApplyProgress()
{
}
