#pragma once
// ============================================================================
// ProbeMethods —— 14 条真实的抓屏路径。
//
// 每一项都写成"截屏软件会怎么写"，而不是"为了被 hook 而写"：
//   * 抓全屏 → CreateCompatibleBitmap + BitBlt + GetDIBits 读回；
//   * 抓窗口 → PrintWindow；
//   * 走 DXGI → Desktop Duplication。
// 这样测出来的结论才对得上红蜘蛛、蓝鸽、NetOp 这些真实客户端的实现。
//
// 最后一组是**误伤自检**：源 DC 是内存 DC，任何钩子都不该改写它。
// ============================================================================
#include "ImageIo.h"
#include <windows.h>

enum class MethodId : int
{
	BitBltScreen = 0,      // GetDC(NULL) + BitBlt
	BitBltCaptureBlt,      // 同上，带 CAPTUREBLT（抓分层窗口）
	StretchBltScreen,      // GetDC(NULL) + StretchBlt 缩放
	GetDibScreenFull,      // GetDIBits 的 hdc 传屏幕 DC，全高
	GetDibScreenPartial,   // 同上，只请求上半屏（测部分行映射）
	GetDibMemDc,           // GetDIBits 的 hdc 传内存 DC（对照，不该被拦）
	GetDib16bpp,           // 16bpp 565：贴不了图，只能走纯色兜底
	DesktopWindowDc,       // GetDesktopWindow + GetWindowDC + BitBlt
	CreateDcDisplay,       // CreateDCW(L"DISPLAY") + BitBlt
	PrintWindowPlain,      // PrintWindow(hwnd, 0)
	PrintWindowFull,       // PrintWindow(hwnd, PW_RENDERFULLCONTENT)
	DxgiDuplication,       // DXGI Desktop Duplication
	SelfTestMemBitBlt,     // 自检：内存 DC 之间 BitBlt
	SelfTestMemStretch,    // 自检：内存 DC 之间 StretchBlt
	Count
};

struct MethodDesc
{
	const wchar_t* name;
	const wchar_t* api;
	bool wholeScreen;   // 抓整屏：跑之前要检查靶窗有没有被挡
	bool selfTest;      // 自检项：不产出画面，只回 PASS/FAIL
};

int MethodCount();
const MethodDesc& MethodAt(int index);

struct ProbeContext
{
	HWND hwnd = nullptr;
	RECT virtualScreen = {};
};

struct ProbeResult
{
	Image32 image;
	RECT screenRect = {};      // image 覆盖的屏幕矩形（虚拟屏坐标）
	bool captured = false;
	bool selfTest = false;
	bool selfTestOk = false;
	DWORD elapsedMs = 0;
	wchar_t note[512] = {};
};

void RunMethod(int index, const ProbeContext& ctx, ProbeResult& out);
