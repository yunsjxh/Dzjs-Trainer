#include "HooksCommon.h"
#include "GuardWindow.h"
#include "ReplaceCodec.h"

// ===== GuardWindow ============================================================
// 从 DzjsTrainerHooks.cpp 机械迁出，函数体逐字节保持原样。

BOOL WINAPI hkSetForegroundWindow(HWND hWnd)
{
	VTraceHook(L"hkSetForegroundWindow");
	return raSetForegroundWindow(hWnd);
}

BOOL WINAPI hkSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
	// 只记录：没打开「拦截远程窗口操作」时原样放行，只在日志里留一条。
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"SetWindowPos", L"hwnd=%p x=%d y=%d cx=%d cy=%d flags=0x%X", hWnd, x, y, cx, cy, uFlags);
		return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
	}
	VTraceHook(L"hkSetWindowPos");
	if (loaded)
	{
		if ((uFlags & SWP_NOSIZE) == 0)
		{
			if ((x == 0 && y == 0 && cx == screenWidth && cy == screenHeight))
			{
				VSwitchLockState(hWnd, TRUE);
				if ((fakeFull) && VIsWindowGbOrHp(hWnd))
					return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
				if (VIsInIllegalCanSizeWindows(hWnd))
				{
					{ VWndListGuard guard; jiYuWndCanSize.remove(hWnd); }
					raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags | SWP_NOZORDER);
					// 同步 SendMessage：目标窗口线程若不处理消息，这里会一直等下去。
					// 日志打在它前后，卡死时最后一行就能指出是不是这里。
					if (g_hookTrace) VDebugLog(L"hkSetWindowPos SendMessage(WM_SIZE) hwnd=%p before", hWnd);
					SendMessage(hWnd, WM_SIZE, NULL, NULL);
					if (g_hookTrace) VDebugLog(L"hkSetWindowPos SendMessage(WM_SIZE) hwnd=%p after", hWnd);
					return TRUE;
				}
				return TRUE;
			}
			else {
				VSwitchLockState(hWnd, FALSE);
				if (fakeFull)
					return raSetWindowPos(hWnd, HWND_NOTOPMOST, x, y, cx, cy, uFlags);
			}
		}

		if (VIsInIllegalWindows(hWnd)) 
			return TRUE;
	}
	return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
}

HDWP WINAPI hkDeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"DeferWindowPos", L"hwnd=%p x=%d y=%d cx=%d cy=%d flags=0x%X", hWnd, x, y, cx, cy, uFlags);
		return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
	}
	VTraceHook(L"hkDeferWindowPos");
	if (loaded)
	{
		if ((uFlags & SWP_NOSIZE) == 0)
		{
			if (x == 0 && y == 0 && cx == screenWidth && cy == screenHeight) {
				VSwitchLockState(hWnd, TRUE);
				if (fakeFull && VIsWindowGbOrHp(hWnd))
					return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
				if (VIsInIllegalCanSizeWindows(hWnd)) {
					{ VWndListGuard guard; jiYuWndCanSize.remove(hWnd); }
					HDWP rs = faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags | SWP_NOZORDER);
					if (g_hookTrace) VDebugLog(L"hkDeferWindowPos SendMessage(WM_SIZE) hwnd=%p before", hWnd);
					SendMessage(hWnd, WM_SIZE, NULL, NULL);
					if (g_hookTrace) VDebugLog(L"hkDeferWindowPos SendMessage(WM_SIZE) hwnd=%p after", hWnd);
					return rs;
				}
				return NULL;
			}
			else {
				VSwitchLockState(hWnd, FALSE);
				if (fakeFull) {
					return faDeferWindowPos(hWinPosInfo, hWnd, HWND_NOTOPMOST, x, y, cx, cy, uFlags);
				}

			}
		}

		if (VIsInIllegalWindows(hWnd))
			return NULL;
	}
	return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
}

BOOL WINAPI hkMoveWindow(HWND hWnd, int x, int y, int cx, int cy, BOOL bRepaint)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"MoveWindow", L"hwnd=%p x=%d y=%d cx=%d cy=%d", hWnd, x, y, cx, cy);
		return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
	}
	VTraceHook(L"hkMoveWindow");
	if (loaded)
	{
		if (x == 0 && y == 0 && cx == screenWidth && cy == screenHeight) {
			VSwitchLockState(hWnd, TRUE);
			if (fakeFull && VIsWindowGbOrHp(hWnd))
				return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
			if (VIsInIllegalCanSizeWindows(hWnd)) {
				{ VWndListGuard guard; jiYuWndCanSize.remove(hWnd); }
				return raMoveWindow(hWnd,  x, y, cx, cy, bRepaint);
			}
			return TRUE;
		}
		else {
			VSwitchLockState(hWnd, FALSE);
			if (fakeFull) 
				return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
		}
		if (VIsInIllegalWindows(hWnd))
			return TRUE;
	} 
	return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
}

BOOL WINAPI hkBringWindowToTop(HWND hWnd)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"BringWindowToTop", L"hwnd=%p", hWnd);
		return faBringWindowToTop(hWnd);
	}
	VTraceHook(L"hkBringWindowToTop");
	if (loaded)
	{
		if (VIsInIllegalWindows(hWnd))
			return TRUE;
	}
	return faBringWindowToTop(hWnd);
}

LONG WINAPI hkSetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"SetWindowLongA", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetWindowLongA(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetWindowLongA");
	if (loaded) {
		if (fakeFull && VIsWindowGbOrHp(hWnd))
			return faSetWindowLongA(hWnd, nIndex, dwNewLong);
		if (VIsInIllegalWindows(hWnd))
			return GetWindowLongA(hWnd, nIndex);
	}
	return faSetWindowLongA(hWnd, nIndex, dwNewLong);
}

LONG WINAPI hkSetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"SetWindowLongW", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetWindowLongW(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetWindowLongW");
	if (loaded) {
		if(fakeFull && VIsWindowGbOrHp(hWnd))
			return faSetWindowLongW(hWnd, nIndex, dwNewLong);
		if (VIsInIllegalWindows(hWnd))
			return GetWindowLongW(hWnd, nIndex);
	}
	return faSetWindowLongW(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI hkShowWindow(HWND hWnd, int nCmdShow)
{
	VTraceHook(L"hkShowWindow");
	if (VIsWindowGbOrHp(hWnd)) 
	{
		if (nCmdShow == SW_SHOW || nCmdShow == SW_SHOWNORMAL  || nCmdShow == SW_MAXIMIZE || nCmdShow == SW_MAX || nCmdShow == SW_SHOWNA
			|| nCmdShow ==  SW_SHOWMAXIMIZED || nCmdShow == SW_SHOWNOACTIVATE || nCmdShow == SW_SHOWDEFAULT) {
			if(VIsWindowHp(hWnd))
				VSwitchLockState(hWnd, TRUE);
			// 只在"广播窗口从未显示变为已显示"时回传一次。
			// 原先每次 ShowWindow 都发，而 VSendMessageBack 内部是 CreateThread ——
			// 极域频繁显示窗口时会一次调用堆一个线程。
			if (VIsWindowGb(hWnd) && !isGbFounded) {
				isGbFounded = true;
				VSendMessageBack(L"hkb:immck", hWndMsgCenter);
			}
		}
		else if (nCmdShow == SW_HIDE) {
			if (VIsWindowGb(hWnd)) isGbFounded = false;
			VSwitchLockState(hWnd, FALSE);
		}
	}
	return faShowWindow(hWnd, nCmdShow);
}

BOOL WINAPI hkEnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable)
{
	VTraceHook(L"hkEnableMenuItem");
	// 屏蔽"关闭"菜单项属于「阻止远程关闭窗口」这一项。原先无条件生效，
	// 与 hkPostMessageW 里用 ProhibitCloseWindow 判断的做法不一致。
	if (ProhibitCloseWindow && uIDEnableItem == SC_CLOSE) {
		VRecordHook(L"EnableMenuItem", L"SC_CLOSE");
		return 0;
	}
	return faEnableMenuItem(hMenu, uIDEnableItem, uEnable);
}

DWORD WINAPI hkSetClassLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"SetClassLongA", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetClassLongA(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetClassLongA");
	if (loaded) {
		if (nIndex == GCL_STYLE && (dwNewLong & CS_NOCLOSE) == CS_NOCLOSE)
			return faSetClassLongA(hWnd, nIndex, dwNewLong ^ CS_NOCLOSE);
	}
	return faSetClassLongA(hWnd, nIndex, dwNewLong);
}

DWORD WINAPI hkSetClassLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (!(loaded && blockWindowOps)) {
		VRecordHook(L"SetClassLongW", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetClassLongW(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetClassLongW");
	if (loaded) {
		if(nIndex == GCL_STYLE && (dwNewLong & CS_NOCLOSE) == CS_NOCLOSE)
			return faSetClassLongW(hWnd, nIndex, dwNewLong ^ CS_NOCLOSE);
	}
	return faSetClassLongW(hWnd, nIndex, dwNewLong);
}

HWND WINAPI hkGetForegroundWindow(VOID)
{
	VTraceHook(L"hkGetForegroundWindow");
	if (allowAllRunOp) return faGetForegroundWindow();
	VRecordHook(L"GetForegroundWindow", L"");
	return NULL;
}

HWND WINAPI hkGetDesktopWindow(VOID)
{
	if (loaded && !allowMonitor) {
		VRecordHook(L"GetDesktopWindow", L"");
		return fakeDesktopWindow;
	}
	return desktopWindow;
}

HDC WINAPI hkGetWindowDC(__in_opt HWND hWnd) {
	if (loaded && hWnd == desktopWindow && !allowMonitor) {
		VRecordHook(L"GetWindowDC", L"hwnd=%p(desktop)", hWnd);
		return faGetWindowDC(fakeDesktopWindow);
	}
	return faGetWindowDC(hWnd);
}

HDC WINAPI hkCreateDCW(LPCWSTR pwszDriver, LPCWSTR pwszDevice, LPCWSTR pszPort, const DEVMODEW * pdm)
{
	if (!allowMonitor)
	{
		if (StrEqual(pwszDriver, L"DISPLAY")) {
			VRecordHook(L"CreateDCW", L"driver=DISPLAY");
			return NULL;
		}
	}
	return faCreateDCW(pwszDriver, pwszDevice, pszPort, pdm);
}

HRESULT WINAPI hkDwmEnableComposition(UINT uCompositionAction)
{
	if (!allowMonitor && uCompositionAction == DWM_EC_DISABLECOMPOSITION) {
		VRecordHook(L"DwmEnableComposition", L"action=disable");
		return S_OK;
	}
	return faDwmEnableComposition(uCompositionAction);
}

// ===== 截屏的另外三条路（移植自 R3ShieldCore/screen_guard.cpp）==================
//
// 只钩 BitBlt 挡不住截屏（而且 hk41 现在是禁用状态）：
//   1. GetDC(NULL) 拿到屏幕 DC 之后直接 GetDIBits 读像素，少一步拷贝；
//   2. StretchBlt 缩放拷贝，整屏一样能拿全；
//   3. PrintWindow 按窗口直接抓，连屏幕 DC 都不用自己取。
//
// 判据沿用参考项目实测出来的结论（他们的 tools/dctest.cpp）：
//   GetDC(NULL) / GetDC(GetDesktopWindow())  → WindowFromDC == 桌面窗口  ← 命中
//   CreateCompatibleDC(screen)               → WindowFromDC == NULL      ← 内存 DC，放行
//   GetDC(任意窗口)                          → WindowFromDC == 该窗口    ← 不是整屏，放行
//
// 为什么必须先过滤：GetDIBits / StretchBlt 是**高频**函数，任何位图操作都走它们。
// 不过滤就会把极域自己的界面绘制、字体渲染一起涂白，直接把目标进程画花。
//
// 屏幕 DC 有两种，别只认第一种：
//   Window  —— GetDC(NULL) / GetWindowDC(桌面)：WindowFromDC 返回桌面窗口；
//   Display —— CreateDCW(L"DISPLAY")：WindowFromDC 一律返回 NULL，它是**设备** DC。
// 只看 WindowFromDC 会把 Display 当成内存 DC 放行，整屏照样被抓走（探针第 09 项）。
enum class ScreenDcKind { None, Window, Display };

static ScreenDcKind ClassifyScreenDc(HDC dc)
{
	if (!dc) return ScreenDcKind::None;
	// ⚠️ 这里**不能**调 GetDesktopWindow() —— 它本身已被 hkGetDesktopWindow(hk27) 钩住，
	//    在 !allowMonitor 时返回的是 fakeDesktopWindow，判据就永远不成立 ——
	//    正好在最需要它生效的那个模式下失效。desktopWindow 是 VLoad 里在装钩之前
	//    缓存的真实句柄；万一它还没初始化，退回走 trampoline 取原始值。
	HWND desk = desktopWindow;
	if (!desk && faGetDesktopWindow) desk = faGetDesktopWindow();
	__try {
		const HWND owner = WindowFromDC(dc);
		if (owner) return owner == desk ? ScreenDcKind::Window : ScreenDcKind::None;
		// owner == NULL 的分两种：内存 DC（OBJ_MEMDC）和显示 DC（OBJ_DC）。
		// 打印机也是 OBJ_DC，靠 TECHNOLOGY 挡掉；不能拿"尺寸差不多"猜 ——
		// 那会把离屏渲染的 DC 一起换掉。
		if (GetObjectType(dc) == OBJ_DC && GetDeviceCaps(dc, TECHNOLOGY) == DT_RASDISPLAY)
			return ScreenDcKind::Display;
		return ScreenDcKind::None;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// 调用方传进来的野句柄：让原函数自己去报错，别在这里拦。
		return ScreenDcKind::None;
	}
}

// 对外的一层壳：通用宿主的 BitBlt 帧替换（DzjsTrainerHooks.cpp 的 Hook_BitBlt）
// 与三个截屏钩子共用同一套判据，否则几处各写一份「什么是屏幕 DC」，迟早会分叉。
//
// 刻意**不**提供"只判源 DC"的版本：显示 DC 同时也是正常的绘制目标（老程序直接往
// 屏幕 DC 上 BitBlt 画画面），少了目的端判据就会把宿主自己的绘制贴掉。
namespace GuardWindow {
	bool IsScreenCaptureInto(HDC src, HDC dest)
	{
		const ScreenDcKind kind = ClassifyScreenDc(src);
		if (kind == ScreenDcKind::None) return false;
		if (kind == ScreenDcKind::Window) return true;
		return GetObjectType(dest) == OBJ_MEMDC;
	}

	// GetDIBits 的 hdc 只用来取格式，像素来自 hbm —— 传了显示 DC 但读的是一张
	// 无关的小位图时不能动。只有整屏大小的位图才按抓屏处理。
	bool IsScreenDibSource(HDC hdc, HBITMAP hbm)
	{
		const ScreenDcKind kind = ClassifyScreenDc(hdc);
		if (kind != ScreenDcKind::Display) return kind == ScreenDcKind::Window;
		BITMAP bm = {};
		if (!hbm || GetObjectW(hbm, sizeof(bm), &bm) != sizeof(bm)) return false;
		return bm.bmWidth >= GetDeviceCaps(hdc, HORZRES)
			&& bm.bmHeight >= GetDeviceCaps(hdc, VERTRES);
	}
}

// ===== 通用替换：四条 GDI 捕获路径共用同一套决策与绘制 ======================
//
// 原来「替换成一张图」只在 LibJPEG20!EncodeToJPEGBuffer 这一条路上生效 ——
// 那是极域自己的编码器，换一个电子教室就不存在了。
// 这里把同一套逻辑挂到 GDI 层：谁抓到屏幕，谁就被换成同一张图。
// 决策复用 ReplaceCodec::PlanJpegFrame，图与 JPEG 路径共用 g_frames，
// 所以界面上换一张图，对所有捕获路径同时生效。

// 这一帧该变成什么。三态与 JPEG 路径完全一致（颜色口径见 ReplaceCodec.h）。
static ReplaceCodec::Plan PlanForCapture()
{
	ReplaceCodec::Request request;
	request.allowMonitor = allowMonitor;
	request.videoModify = VVideoReplacementEnabled();
	request.imageReady = g_frames.Valid();
	return ReplaceCodec::PlanJpegFrame(request);
}

// 没有替换图时的纯色：禁止监看 → 白（让教师看到一片空白）；
// 启用替换但允许监看 → 黑（把画面盖掉）。与 PlanJpegFrame 同一口径，
// 别"统一成白"—— 那是行为变更，不是重构。
static BYTE SolidFallback()
{
	return allowMonitor ? 0x00 : 0xFF;
}

// 把替换图铺到目标 DC 的矩形上。铺不上返回 false，由调用方回落纯色。
static bool OverlayOnDc(HDC dc, int x, int y, int w, int h)
{
	if (!dc || w <= 0 || h <= 0) return false;
	ReplaceState::Frame frame;
	if (!g_frames.Acquire(frame)) return false;
	bool ok = false;
	if (frame.dib && frame.width > 0 && frame.height > 0)
		ok = ReplaceCodec::PaintDib(dc, x, y, w, h, frame.dib, frame.width, frame.height, frame.dibStride);
	g_frames.Release();
	return ok;
}

// 纯色填充。PatBlt 对 DDB / DIB section 都有效，不需要知道位深和行距。
static void FillOnDc(HDC dc, int x, int y, int w, int h, BYTE value)
{
	if (!dc || w <= 0 || h <= 0) return;
	PatBlt(dc, x, y, w, h, value ? WHITENESS : BLACKNESS);
}

// 把替换图铺进调用方的 DIB 缓冲区。
//
// 与 DC 版本的差别：这里拿到的是一段裸内存，必须自己按位深写像素。
// 支持 24/32bpp，以及 16bpp 的 555（BI_RGB）和 565（BI_BITFIELDS + 0xF800/07E0/001F）。
// 8bpp 需要调色板量化，仍然返回 false 让调用方回落纯色（宁可整块变黑，也不要花屏）。
//
// startRow/fullHeight 用来处理「只请求了部分行」的 GetDIBits：目标第 i 行
// 对应整张位图的第 startRow+i 行，源图必须按整张高度映射，否则贴图会错位。
static bool OverlayOnDib(LPVOID bits, int width, int rows, int fullHeight,
	int startRow, int stride, WORD bitCount, DWORD compression, const DWORD* masks, bool bottomUp)
{
	if (!bits || width <= 0 || rows <= 0 || fullHeight <= 0) return false;

	// 16bpp 有两种布局，写错就是一片偏色（红蓝互换只是最轻的一种）。
	int pack16 = 0;   // 0=不是 16bpp / 1=555 / 2=565
	if (bitCount == 16) {
		if (compression == BI_RGB)
			pack16 = 1;
		else if (compression == BI_BITFIELDS && masks &&
		         masks[0] == 0xF800 && masks[1] == 0x07E0 && masks[2] == 0x001F)
			pack16 = 2;
		else
			return false;   // 自定义掩码：不去猜
	}
	else if (bitCount != 24 && bitCount != 32)
		return false;

	const int bytesPerPixel = bitCount / 8;
	if (stride < width * bytesPerPixel) return false;

	ReplaceState::Frame frame;
	if (!g_frames.Acquire(frame)) return false;

	bool ok = false;
	if (frame.rgb && frame.width > 0 && frame.height > 0) {
		for (int i = 0; i < rows; ++i) {
			const int dibRow = startRow + i;
			if (dibRow < 0 || dibRow >= fullHeight) break;
			// biHeight 为负是顶向下；为正则 DIB 第 0 行在底部。
			const int screenY = bottomUp ? (fullHeight - 1 - dibRow) : dibRow;
			int sy = (int)(((INT64)screenY * frame.height) / fullHeight);
			if (sy < 0) sy = 0;
			if (sy >= frame.height) sy = frame.height - 1;

			const BYTE* srow = frame.rgb + (size_t)sy * frame.width * 3;
			BYTE* drow = (BYTE*)bits + (size_t)i * stride;
			for (int x = 0; x < width; ++x) {
				int sx = (int)(((INT64)x * frame.width) / width);
				if (sx < 0) sx = 0;
				if (sx >= frame.width) sx = frame.width - 1;
				const BYTE* s = srow + (size_t)sx * 3;
				BYTE* d = drow + (size_t)x * bytesPerPixel;
				if (pack16) {
					// 通道截断比四舍五入快，误差半个色阶，肉眼看不出来。
					const WORD v = (pack16 == 2)
						? (WORD)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3))
						: (WORD)(((s[0] >> 3) << 10) | ((s[1] >> 3) << 5) | (s[2] >> 3));
					d[0] = (BYTE)(v & 0xFF);
					d[1] = (BYTE)(v >> 8);
					continue;
				}
				d[0] = s[2];   // B
				d[1] = s[1];   // G
				d[2] = s[0];   // R
				if (bytesPerPixel == 4) d[3] = 0xFF;   // 32bpp 的填充字节给不透明
			}
		}
		ok = true;
	}
	g_frames.Release();
	return ok;
}

BOOL WINAPI hkStretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
	HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
{
	// 热路径：源不是屏幕 DC（绝大多数调用）直接放行，连日志都不写。
	if (!GuardWindow::IsScreenCaptureInto(hdcSrc, hdcDest))
		return faStretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

	// 只处理整块拷贝。rop 带 AND/OR/XOR 的是合成操作，覆盖会把画面毁掉。
	const bool isCopy = (rop == SRCCOPY) || (rop == (SRCCOPY | CAPTUREBLT));
	const ReplaceCodec::Plan plan = PlanForCapture();

	if (!isCopy || plan.action == ReplaceCodec::Action::Passthrough) {
		VRecordHook(L"StretchBlt", L"dest=%d,%d %dx%d src=%d,%d %dx%d rop=0x%X",
			xDest, yDest, wDest, hDest, xSrc, ySrc, wSrc, hSrc, rop);
		return faStretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
	}

	VTraceHook(L"hkStretchBlt");
	const BOOL result = faStretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
	// 先让真调用把内容画上去，再整块覆盖 —— 调用方拿到的是"成功"，但内容已经换了。
	if (result) {
		const bool drawn = plan.action == ReplaceCodec::Action::OverlayImage
			&& OverlayOnDc(hdcDest, xDest, yDest, wDest, hDest);
		if (!drawn) FillOnDc(hdcDest, xDest, yDest, wDest, hDest, SolidFallback());
	}
	return result;
}

BOOL WINAPI hkPrintWindow(HWND hwnd, HDC hdcBlt, UINT nFlags)
{
	const ReplaceCodec::Plan plan = PlanForCapture();
	if (plan.action == ReplaceCodec::Action::Passthrough) {
		if (loaded) VRecordHook(L"PrintWindow", L"hwnd=%p flags=0x%X", hwnd, nFlags);
		return faPrintWindow(hwnd, hdcBlt, nFlags);
	}

	VTraceHook(L"hkPrintWindow");
	const BOOL result = faPrintWindow(hwnd, hdcBlt, nFlags);
	if (result) {
		// hdcBlt 通常是内存 DC，尺寸只能从裁剪框反推。
		RECT rc = {};
		if (GetClipBox(hdcBlt, &rc) != ERROR && rc.right > rc.left && rc.bottom > rc.top) {
			const int w = rc.right - rc.left;
			const int h = rc.bottom - rc.top;
			const bool drawn = plan.action == ReplaceCodec::Action::OverlayImage
				&& OverlayOnDc(hdcBlt, rc.left, rc.top, w, h);
			if (!drawn) FillOnDc(hdcBlt, rc.left, rc.top, w, h, SolidFallback());
		}
	}
	return result;
}

int WINAPI hkGetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT usage)
{
	// 热路径：内存 DC 读回自己的位图，在这里就挡回去。
	if (!GuardWindow::IsScreenDibSource(hdc, hbm))
		return faGetDIBits(hdc, hbm, start, cLines, lpvBits, lpbmi, usage);

	const ReplaceCodec::Plan plan = PlanForCapture();
	if (plan.action == ReplaceCodec::Action::Passthrough) {
		VRecordHook(L"GetDIBits", L"start=%u lines=%u bits=%p", start, cLines, lpvBits);
		return faGetDIBits(hdc, hbm, start, cLines, lpvBits, lpbmi, usage);
	}

	VTraceHook(L"hkGetDIBits");
	const int result = faGetDIBits(hdc, hbm, start, cLines, lpvBits, lpbmi, usage);
	// 真调用已经把真实像素写进调用者的缓冲区，这里按"本次真正写入的行数"覆盖。
	if (result > 0 && lpvBits && lpbmi) {
		const WORD bitCount = lpbmi->bmiHeader.biBitCount;
		if (bitCount >= 8 && lpbmi->bmiHeader.biWidth > 0) {
			// 行距按 DIB 规则算：((width * bitCount + 31) / 32) * 4，四字节对齐。
			const int width = (int)lpbmi->bmiHeader.biWidth;
			const int stride = (int)((((DWORD)width * bitCount) + 31) / 32 * 4);
			const LONG rawHeight = lpbmi->bmiHeader.biHeight;
			const int fullHeight = rawHeight < 0 ? (int)-rawHeight : (int)rawHeight;
			const bool bottomUp = rawHeight > 0;
			// 24/32bpp 与 16bpp（555/565）都能贴图；8bpp 落到下面的纯色兜底。
			// BI_BITFIELDS 的掩码紧跟在 BITMAPINFOHEADER 后面，就是 bmiColors 那块。
			const DWORD* masks = lpbmi->bmiHeader.biCompression == BI_BITFIELDS
				? reinterpret_cast<const DWORD*>(lpbmi->bmiColors) : nullptr;
			const bool drawn = plan.action == ReplaceCodec::Action::OverlayImage
				&& OverlayOnDib(lpvBits, width, result, fullHeight, (int)start, stride,
				                bitCount, lpbmi->bmiHeader.biCompression, masks, bottomUp);
			// 兜底对 8/16/24/32bpp 都成立：0xFF 在 565 里是白，0x00 是黑。
			if (!drawn)
				FillMemory(lpvBits, (SIZE_T)stride * (SIZE_T)result, SolidFallback());
		}
	}
	return result;
}

LONG WINAPI hkChangeDisplaySettingsW(DEVMODEW* lpDevMode, DWORD dwFlags)
{
	return DISP_CHANGE_SUCCESSFUL;
}

HDESK WINAPI hkOpenDesktopA(LPCSTR lpszDesktop, DWORD dwFlags, BOOL fInherit, ACCESS_MASK dwDesiredAccess)
{
	SetLastError(ERROR_ACCESS_DENIED);
	return NULL;
}

HDESK WINAPI hkOpenInputDesktop(DWORD dwFlags,BOOL fInherit, ACCESS_MASK dwDesiredAccess)
{
	SetLastError(ERROR_ACCESS_DENIED);
	return NULL;
}
