#include "HooksCommon.h"
#include "GuardWindow.h"
#include "ReplaceCodec.h"

// ===== GuardWindow ============================================================
// 从 DzjsTrainerHooks.cpp 机械迁出，函数体逐字节保持原样。

// ===== 强制窗口化 ============================================================
// 电子教室的「全屏」无非是 SetWindowPos(0, 0, 屏宽, 屏高) 加一个 TOPMOST。以前的
// 做法是把整个请求吞掉 —— 宿主那边还以为自己已经铺满了，底下的绘制层照旧按全屏
// 画，屏幕上就只剩一片黑。这里改成「把请求改小再放行」：宿主知道自己是窗口，画面
// 跟着缩，用户还能自己拖边框调整。用户调过的尺寸按窗口类记下来，下次同类窗口直接
// 用那个比例，不用每次重调。

struct VEduClassSize {
	WCHAR cls[48];
	int   percent;
	ULONGLONG tick;
};

struct VEduTrackedWindow {
	HWND  hwnd;
	WCHAR cls[48];
	int   cx, cy;          // 上次由我们摆成的尺寸，用来判断用户有没有动过
	ULONGLONG tick;
};

static SRWLOCK g_eduWinLock = SRWLOCK_INIT;
static VEduClassSize g_eduClassSizes[8] = {};
static VEduTrackedWindow g_eduTracked[12] = {};

static void VEduClassNameOf(HWND hWnd, WCHAR* out, int cch)
{
	if (!out || cch <= 0) return;
	out[0] = 0;
	if (hWnd) GetClassNameW(hWnd, out, cch);
}

static int VEduRememberedPercent(const WCHAR* cls)
{
	if (!cls || !cls[0]) return 0;
	int result = 0;
	AcquireSRWLockShared(&g_eduWinLock);
	for (int i = 0; i < _countof(g_eduClassSizes); ++i) {
		if (g_eduClassSizes[i].percent > 0 && wcscmp(g_eduClassSizes[i].cls, cls) == 0) {
			result = g_eduClassSizes[i].percent;
			break;
		}
	}
	ReleaseSRWLockShared(&g_eduWinLock);
	return result;
}

static void VEduRememberPercent(const WCHAR* cls, int percent)
{
	if (!cls || !cls[0] || percent <= 0) return;
	if (percent < 25) percent = 25;
	if (percent > 95) percent = 95;
	AcquireSRWLockExclusive(&g_eduWinLock);
	int slot = -1;
	int oldest = 0;
	for (int i = 0; i < _countof(g_eduClassSizes); ++i) {
		if (g_eduClassSizes[i].percent > 0 && wcscmp(g_eduClassSizes[i].cls, cls) == 0) { slot = i; break; }
		if (g_eduClassSizes[i].percent <= 0) { slot = i; break; }
		if (g_eduClassSizes[i].tick < g_eduClassSizes[oldest].tick) oldest = i;
	}
	if (slot < 0) slot = oldest;
	wcscpy_s(g_eduClassSizes[slot].cls, cls);
	g_eduClassSizes[slot].percent = percent;
	g_eduClassSizes[slot].tick = GetTickCount64();
	ReleaseSRWLockExclusive(&g_eduWinLock);
}

// 这个窗口是不是被我们改过。巡查要靠它认人：改过之后窗口有了标题栏，
// 「有标题栏就不碰」这条规则会把宿主后来又撑开的窗口一直放过。
static bool VEduIsTracked(HWND hWnd)
{
	if (!hWnd) return false;
	bool found = false;
	AcquireSRWLockShared(&g_eduWinLock);
	for (int i = 0; i < _countof(g_eduTracked); ++i) {
		if (g_eduTracked[i].hwnd == hWnd) { found = true; break; }
	}
	ReleaseSRWLockShared(&g_eduWinLock);
	return found;
}

static void VEduTrackWindow(HWND hWnd, int cx, int cy)
{
	if (!hWnd) return;
	WCHAR cls[48];
	VEduClassNameOf(hWnd, cls, _countof(cls));
	AcquireSRWLockExclusive(&g_eduWinLock);
	int slot = -1;
	int oldest = 0;
	for (int i = 0; i < _countof(g_eduTracked); ++i) {
		if (g_eduTracked[i].hwnd == hWnd) { slot = i; break; }
		if (!g_eduTracked[i].hwnd) { slot = i; break; }
		if (g_eduTracked[i].tick < g_eduTracked[oldest].tick) oldest = i;
	}
	if (slot < 0) slot = oldest;
	g_eduTracked[slot].hwnd = hWnd;
	wcscpy_s(g_eduTracked[slot].cls, cls);
	g_eduTracked[slot].cx = cx;
	g_eduTracked[slot].cy = cy;
	g_eduTracked[slot].tick = GetTickCount64();
	ReleaseSRWLockExclusive(&g_eduWinLock);
}

static void VEduResetAnswers();

void VEduResetWindowedState()
{
	AcquireSRWLockExclusive(&g_eduWinLock);
	SecureZeroMemory(g_eduTracked, sizeof(g_eduTracked));
	SecureZeroMemory(g_eduClassSizes, sizeof(g_eduClassSizes));
	ReleaseSRWLockExclusive(&g_eduWinLock);
	VEduResetAnswers();
}

static bool VEduCoversMonitor(HWND hWnd, int cx, int cy);

// ===== 全屏请求的询问 ========================================================
// 「先问一句」这条路径按窗口记答案：广播时宿主会连着发一串全屏请求（改尺寸、
// 再置顶、再改一次），每帧问一次没法用。同一个窗口在 10 分钟内沿用第一次的答案；
// 窗口标题/用途变了、10 分钟过了会重新问。
//
// 框是从**调用线程**弹的，也就是宿主自己的 UI 线程 —— 它本来就是要抢屏幕，
// 停下来等一句话不影响它。但在框里 pumping 消息会让同一路径重入，所以用
// g_eduAsking 挡住：正在问的时候别的请求一律放行，免得叠出第二层框。
struct VEduAnswer {
	HWND      hwnd;
	bool      allow;
	ULONGLONG tick;
};

static VEduAnswer g_eduAnswers[8] = {};
static SRWLOCK g_eduAnswerLock = SRWLOCK_INIT;
static volatile LONG g_eduAsking = 0;

static const ULONGLONG kEduAnswerTtlMs = 10ULL * 60 * 1000;
static const WCHAR kEduAskTitle[] = L"极宇训练器 - 全屏请求";
// 框要是没人答，宿主就一直停在那儿。30 秒后自己关掉，按「否」处理。
static const int kEduAskTimeoutSec = 30;

// 框开着的时候窗口钩子一律放行，这个框也是窗口，摘掉它的置顶就会藏到广播层
// 后面去 —— 用户看不见框、宿主又在等那句回答，两边一起卡住。
static DWORD WINAPI VEduAskTimeoutThread(LPVOID param)
{
	// 等框真出来再开始计时：CreateWindow 之前的那点时间不算。
	for (int i = 0; i < 50; ++i) {
		Sleep(100);
		if (FindWindowW(L"#32770", kEduAskTitle)) break;
	}
	Sleep(static_cast<DWORD>(kEduAskTimeoutSec) * 1000);
	HWND box = FindWindowW(L"#32770", kEduAskTitle);
	if (box) PostMessageW(box, WM_CLOSE, 0, 0);
	return 0;
}

static void VEduResetAnswers()
{
	AcquireSRWLockExclusive(&g_eduAnswerLock);
	SecureZeroMemory(g_eduAnswers, sizeof(g_eduAnswers));
	ReleaseSRWLockExclusive(&g_eduAnswerLock);
}

// 查这个窗口上一次答的是什么。查不到返回 false（该问了）。
static bool VEduLookupAnswer(HWND hWnd, bool& allow)
{
	allow = false;
	if (!hWnd) return false;
	const ULONGLONG now = GetTickCount64();
	bool found = false;
	AcquireSRWLockShared(&g_eduAnswerLock);
	for (int i = 0; i < _countof(g_eduAnswers); ++i) {
		if (g_eduAnswers[i].hwnd != hWnd) continue;
		if (now - g_eduAnswers[i].tick > kEduAnswerTtlMs) break;
		allow = g_eduAnswers[i].allow;
		found = true;
		break;
	}
	ReleaseSRWLockShared(&g_eduAnswerLock);
	return found;
}

static void VEduRememberAnswer(HWND hWnd, bool allow)
{
	if (!hWnd) return;
	const ULONGLONG now = GetTickCount64();
	AcquireSRWLockExclusive(&g_eduAnswerLock);
	int slot = -1;
	int oldest = 0;
	for (int i = 0; i < _countof(g_eduAnswers); ++i) {
		if (g_eduAnswers[i].hwnd == hWnd) { slot = i; break; }
		if (!g_eduAnswers[i].hwnd) { slot = i; break; }
		if (g_eduAnswers[i].tick < g_eduAnswers[oldest].tick) oldest = i;
	}
	if (slot < 0) slot = oldest;
	g_eduAnswers[slot].hwnd = hWnd;
	g_eduAnswers[slot].allow = allow;
	g_eduAnswers[slot].tick = now;
	ReleaseSRWLockExclusive(&g_eduAnswerLock);
}

// 这个窗口是不是刚被「允许全屏」过。置顶拦截要认它：用户点了允许，再把人家的
// 置顶摘掉，广播窗口就会缩到别的窗口后面去。
static bool VEduFullScreenAllowed(HWND hWnd)
{
	bool allow = false;
	if (!askFullScreen) return false;
	return VEduLookupAnswer(hWnd, allow) && allow;
}

// 返回值：true = 放行全屏，false = 改成窗口。
static bool VEduAskFullScreen(HWND hWnd, int cx, int cy)
{
	bool allow = false;
	if (VEduLookupAnswer(hWnd, allow)) return allow;
	if (InterlockedCompareExchange(&g_eduAsking, 1, 0) != 0) return true;

	WCHAR cls[48] = {};
	VEduClassNameOf(hWnd, cls, _countof(cls));
	WCHAR title[128] = {};
	GetWindowTextW(hWnd, title, _countof(title));

	WCHAR text[768];
	swprintf_s(text,
		L"「%s」请求铺满整个屏幕（%d × %d）。\n\n"
		L"选「是」：照它说的全屏。\n"
		L"选「否」：改成工作区 %d%% 的可拖动窗口，可以自己拉大小。\n\n"
		L"同一个窗口 10 分钟内不再询问。",
		title[0] ? title : (cls[0] ? cls : L"电子教室"), cx, cy, windowedPercent);

	HANDLE timeout = CreateThread(NULL, 0, VEduAskTimeoutThread, NULL, 0, NULL);
	if (timeout) CloseHandle(timeout);

	// 框要压在置顶窗口上面：MB_TOPMOST 之外再给个前台，宿主的广播层抢不走。
	const int answer = MessageBoxW(hWnd, text, kEduAskTitle,
		MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON1);
	InterlockedExchange(&g_eduAsking, 0);

	allow = (answer == IDYES);
	VEduRememberAnswer(hWnd, allow);
	VDebugLog(L"windowed ask hwnd=%p class=%s title=%s %dx%d -> %s",
		hWnd, cls[0] ? cls : L"-", title[0] ? title : L"-", cx, cy,
		allow ? L"allow" : L"windowed");
	// 也送一份到主日志：「到底问没问、答的是什么」是用户反馈里最先要确认的事，
	// 钩子自己的 debug 日志在宿主目录里，用户不一定会翻。
	VNotifyFullScreenAsk(cls, allow, cx, cy);
	return allow;
}

// 全屏请求的最终裁决：要不要改写成窗口。askFullScreen 打开时先问一句。
static bool VEduShouldWindow(HWND hWnd, int cx, int cy)
{
	if (!loaded || !forceWindowed || !hWnd) return false;
	if (!VEduCoversMonitor(hWnd, cx, cy)) return false;
	// 还没显示出来的窗口没法问，也没人看得见回答：直接按窗口摆好，等它 show
	// 出来已经是小窗了。以前这里直接放行，广播画面刚好是在隐藏状态下摆尺寸的，
	// 于是屏幕上照样铺满一整块 —— 钩子「拦了」，只是拦成了放行。
	if (!IsWindowVisible(hWnd)) return true;
	if (askFullScreen && VEduAskFullScreen(hWnd, cx, cy)) return false;
	return true;
}

// 请求里的尺寸是否铺满整块显示器。工作区（扣除任务栏）不算：那是最大化的尺寸，
// 用户自己要的，拦下来就成了「无法自己放大」。电子教室的广播画面用的是整块屏，
// 高度连任务栏那块也一起占了。
static bool VEduCoversMonitor(HWND hWnd, int cx, int cy)
{
	if (cx <= 0 || cy <= 0) return false;
	MONITORINFO mi = { sizeof(mi) };
	HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	if (!mon || !GetMonitorInfoW(mon, &mi)) {
		return cx >= screenWidth - 2 && cy >= screenHeight - 2;
	}
	const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
	const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
	return cx >= mw - 2 && cy >= mh - 2;
}

// 算一块窗口化后的矩形：工作区的百分比，居中。类名有记忆就用记忆里的比例。
static void VEduWindowedRect(HWND hWnd, int& x, int& y, int& cx, int& cy)
{
	MONITORINFO mi = { sizeof(mi) };
	HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	if (!mon || !GetMonitorInfoW(mon, &mi)) {
		mi.rcWork.left = 0;
		mi.rcWork.top = 0;
		mi.rcWork.right = screenWidth;
		mi.rcWork.bottom = screenHeight;
	}
	int ww = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
	int wh = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
	if (ww < 320) ww = 320;
	if (wh < 240) wh = 240;

	WCHAR cls[48];
	VEduClassNameOf(hWnd, cls, _countof(cls));
	int percent = VEduRememberedPercent(cls);
	if (percent <= 0) percent = windowedPercent;
	if (percent < 25) percent = 25;
	if (percent > 95) percent = 95;

	int nw = ww * percent / 100;
	int nh = wh * percent / 100;
	// 太小的窗口没法看广播，给个下限；工作区本身比它还小时按工作区。
	if (nw < 400) nw = (ww < 400) ? ww : 400;
	if (nh < 300) nh = (wh < 300) ? wh : 300;

	x = mi.rcWork.left + (ww - nw) / 2;
	y = mi.rcWork.top + (wh - nh) / 2;
	cx = nw;
	cy = nh;
}

// 补上标题栏和可缩放的边框。全屏窗口多半是 WS_POPUP，没有边框的话即使改小了
// 也没法拖 —— 等于把用户困在一个小窗口里。
//
// 扩展样式也得动：广播窗口基本都是 WS_EX_TOOLWINDOW，工具窗口的标题栏是小号的，
// 只有一颗关闭按钮，没有最小化/最大化、标题栏上也没有图标，任务栏和 Alt+Tab 都
// 看不见它。换成 WS_EX_APPWINDOW 之后才是一颗能最小化、能右键出系统菜单的普通窗口。
static void VEduEnsureResizable(HWND hWnd)
{
	const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
	if (style & WS_CHILD) return;
	// WS_MAXIMIZE 一并摘掉：窗口原来要是最大化的，改小之后标题栏会一直显示
	// 「还原」按钮，状态对不上 —— 点一下还会跳回一个奇怪的尺寸。
	const LONG want = (style | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
		& ~WS_MAXIMIZE;
	const LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
	const LONG exWant = (exStyle & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW;
	const bool styleChanged = (want != style);
	const bool exChanged = (exWant != exStyle);
	if (!styleChanged && !exChanged) return;
	if (styleChanged) {
		if (faSetWindowLongW)
			faSetWindowLongW(hWnd, GWL_STYLE, want);
		else
			SetWindowLongW(hWnd, GWL_STYLE, want);
	}
	if (exChanged) SetWindowLongW(hWnd, GWL_EXSTYLE, exWant);
	if (raSetWindowPos) {
		// WS_THICKFRAME 要配合 SWP_FRAMECHANGED，边框才会立刻长出来。
		raSetWindowPos(hWnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}
}

static void VEduDropTopmost(HWND hWnd)
{
	if ((GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) return;
	if (raSetWindowPos)
		raSetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 窗口化日志节流：同一窗口 2 秒内只报一次，广播时宿主会连着请求好几次。
static bool VEduShouldLogWindowed(HWND hWnd)
{
	struct Slot { HWND hwnd; ULONGLONG tick; };
	static Slot slots[8] = {};
	static SRWLOCK lock = SRWLOCK_INIT;
	const ULONGLONG now = GetTickCount64();
	bool allow = false;
	AcquireSRWLockExclusive(&lock);
	int slot = -1;
	int oldest = 0;
	for (int i = 0; i < _countof(slots); ++i) {
		if (slots[i].hwnd == hWnd) { slot = i; break; }
		if (!slots[i].hwnd) { slot = i; break; }
		if (slots[i].tick < slots[oldest].tick) oldest = i;
	}
	if (slot < 0) slot = oldest;
	if (slots[slot].hwnd != hWnd || now - slots[slot].tick > 2000) {
		slots[slot].hwnd = hWnd;
		slots[slot].tick = now;
		allow = true;
	}
	ReleaseSRWLockExclusive(&lock);
	return allow;
}

// 把一次全屏请求改写成窗口化请求。返回 true 表示已改写，调用方直接把新参数
// 交给原函数即可。fakeFull（用户主动开的「假全屏」）由调用方先行放行。
static bool VEduForceWindowed(HWND hWnd, int& x, int& y, int& cx, int& cy)
{
	if (!forceWindowed || !hWnd) return false;
	const int fromW = cx;
	const int fromH = cy;
	VEduEnsureResizable(hWnd);
	if (blockTopmost) VEduDropTopmost(hWnd);
	VEduWindowedRect(hWnd, x, y, cx, cy);
	VEduTrackWindow(hWnd, cx, cy);

	if (VEduShouldLogWindowed(hWnd)) {
		WCHAR cls[48];
		VEduClassNameOf(hWnd, cls, _countof(cls));
		VDebugLog(L"windowed hwnd=%p class=%s %dx%d -> %dx%d (+caption/thickframe, topmost=%d)",
			hWnd, cls[0] ? cls : L"-", fromW, fromH, cx, cy, blockTopmost ? 0 : 1);
		VNotifyWindowed(hWnd, cls, fromW, fromH, cx, cy);
	}
	return true;
}

// 把「加边框 + 置顶」这条规则也用在没有尺寸变化的请求上：宿主可能先把窗口
// 置顶、隔一会儿再改尺寸，只拦后一步的话中间那段时间窗口已经盖上去了。
static bool VEduNormalizeTopmost(HWND hWnd)
{
	if (!blockTopmost || !hWnd) return false;
	const LONG ex = GetWindowLongW(hWnd, GWL_EXSTYLE);
	if ((ex & WS_EX_TOPMOST) == 0) return false;
	VEduDropTopmost(hWnd);
	return true;
}

// 把一个「已经铺满整屏」的窗口改小。show 和巡查两条路共用 —— 钩子那条路是在
// SetWindowPos 里直接改写参数，不走这里。
//
// allowAsk：要不要弹框问。ShowWindow 走的是宿主自己的线程，问是安全的；
// 巡查跑在消息中心线程上，绝对不能问（上一轮卡死就是这么来的）。
static bool VEduShrinkFullScreenWindow(HWND hWnd, bool allowAsk)
{
	if (!loaded || !forceWindowed || !hWnd) return false;
	const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
	if (style & WS_CHILD) return false;

	RECT rc = {};
	if (!GetWindowRect(hWnd, &rc)) return false;
	const int fromW = static_cast<int>(rc.right - rc.left);
	const int fromH = static_cast<int>(rc.bottom - rc.top);
	if (!VEduCoversMonitor(hWnd, fromW, fromH)) return false;
	if (allowAsk && askFullScreen && IsWindowVisible(hWnd)
		&& VEduAskFullScreen(hWnd, fromW, fromH)) return false;

	int x = 0, y = 0, cx = 0, cy = 0;
	VEduWindowedRect(hWnd, x, y, cx, cy);
	VEduEnsureResizable(hWnd);
	if (blockTopmost) VEduDropTopmost(hWnd);
	if (raSetWindowPos)
		raSetWindowPos(hWnd, blockTopmost ? HWND_NOTOPMOST : NULL, x, y, cx, cy,
			SWP_NOZORDER | SWP_NOACTIVATE);
	VEduTrackWindow(hWnd, cx, cy);

	if (VEduShouldLogWindowed(hWnd)) {
		WCHAR cls[48];
		VEduClassNameOf(hWnd, cls, _countof(cls));
		VDebugLog(L"windowed (%s) hwnd=%p class=%s %dx%d -> %dx%d",
			allowAsk ? L"show" : L"sweep", hWnd, cls[0] ? cls : L"-", fromW, fromH, cx, cy);
		VNotifyWindowed(hWnd, cls, fromW, fromH, cx, cy);
	}
	return true;
}

void VEduWindowIfFullScreen(HWND hWnd)
{
	VEduShrinkFullScreenWindow(hWnd, true);
}

// 巡查「创建时就铺满」的窗口。有些产品不走 SetWindowPos，而是直接
// CreateWindowEx(WS_POPUP, 0, 0, 屏宽, 屏高)，或者用 SetWindowPlacement —— 钩子
// 看不见这种请求，只能定时枚举一遍本进程的顶层窗口补刀。
//
// 认人规则：我们自己改过的窗口必须继续管（改过之后它有标题栏了，「有标题栏就不碰」
// 会把宿主后来又撑开的窗口一直放过）；用户自己的普通窗口、自己最大化的窗口不碰。
struct VEduSweepCtx {
	int fixedCount;
};

static BOOL CALLBACK VEduSweepProc(HWND hWnd, LPARAM lp)
{
	VEduSweepCtx* ctx = reinterpret_cast<VEduSweepCtx*>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(hWnd, &pid);
	if (pid != GetCurrentProcessId()) return TRUE;
	if (hWnd == hWndMsgCenter || hWnd == fakeDesktopWindow) return TRUE;
	if (GetAncestor(hWnd, GA_ROOT) != hWnd) return TRUE;
	const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
	if (style & WS_CHILD) return TRUE;
	if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) return TRUE;

	const bool topmost = (GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
	const bool managed = VEduIsTracked(hWnd);
	if (!managed && !topmost && captioned) return TRUE;
	// 用户刚刚在框里答应了「照它说的全屏」：巡查别马上又给改回去。
	if (VEduFullScreenAllowed(hWnd)) return TRUE;

	if (VEduShrinkFullScreenWindow(hWnd, false)) ++ctx->fixedCount;
	return TRUE;
}

void VEduSweepFullScreenWindows()
{
	// 询问模式下也照查，只是不弹框：这条路径跑在消息中心线程上，从那里弹框会把
	// 消息中心卡住。也就是说「创建即全屏」的窗口在询问模式下不问，直接按窗口摆好。
	if (!loaded || !forceWindowed) return;
	VEduSweepCtx ctx = {};
	EnumWindows(VEduSweepProc, reinterpret_cast<LPARAM>(&ctx));
}

void VEduSampleTrackedWindows()
{
	if (!loaded) return;
	struct Snap { HWND hwnd; WCHAR cls[48]; int cx, cy; };
	Snap snaps[_countof(g_eduTracked)] = {};
	int count = 0;
	AcquireSRWLockShared(&g_eduWinLock);
	for (int i = 0; i < _countof(g_eduTracked); ++i) {
		if (!g_eduTracked[i].hwnd) continue;
		snaps[count].hwnd = g_eduTracked[i].hwnd;
		wcscpy_s(snaps[count].cls, g_eduTracked[i].cls);
		snaps[count].cx = g_eduTracked[i].cx;
		snaps[count].cy = g_eduTracked[i].cy;
		++count;
	}
	ReleaseSRWLockShared(&g_eduWinLock);
	if (count == 0) return;

	for (int i = 0; i < count; ++i) {
		HWND hWnd = snaps[i].hwnd;
		// 窗口没了就把它从跟踪表里去掉，别让槽位被死 hwnd 占着。
		if (!IsWindow(hWnd) || IsIconic(hWnd)) {
			AcquireSRWLockExclusive(&g_eduWinLock);
			for (int k = 0; k < _countof(g_eduTracked); ++k)
				if (g_eduTracked[k].hwnd == hWnd) SecureZeroMemory(&g_eduTracked[k], sizeof(g_eduTracked[k]));
			ReleaseSRWLockExclusive(&g_eduWinLock);
			continue;
		}
		if (!IsWindowVisible(hWnd)) continue;
		// 用户自己最大化的别记：工作区尺寸会算成 95% 以上，下次广播就铺得更大。
		if (IsZoomed(hWnd)) continue;

		RECT rc = {};
		if (!GetWindowRect(hWnd, &rc)) continue;
		const int w = static_cast<int>(rc.right - rc.left);
		const int h = static_cast<int>(rc.bottom - rc.top);
		if (w <= 0 || h <= 0) continue;
		// 又被宿主撑回全屏了：钩子会处理，这里不记。
		if (VEduCoversMonitor(hWnd, w, h)) continue;
		// 跟上次摆的尺寸差不多，说明用户没动过。
		if (abs(w - snaps[i].cx) <= 16 && abs(h - snaps[i].cy) <= 16) continue;

		MONITORINFO mi = { sizeof(mi) };
		HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		int ww = 0, wh = 0;
		if (mon && GetMonitorInfoW(mon, &mi)) {
			ww = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
			wh = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
		}
		if (ww <= 0 || wh <= 0) { ww = screenWidth; wh = screenHeight; }
		// 铺满工作区 = 用户按了最大化（或者拖到顶上吸附），不代表他喜欢这个尺寸：
		// 记成 95% 的话，下一次广播就会铺到 95% 那么大。
		if (w >= ww - 4 && h >= wh - 4) continue;
		int pctW = static_cast<int>(static_cast<long long>(w) * 100 / ww);
		int pctH = static_cast<int>(static_cast<long long>(h) * 100 / wh);
		int pct = (pctW < pctH) ? pctW : pctH;
		if (pct < 25) pct = 25;
		if (pct > 95) pct = 95;
		VEduRememberPercent(snaps[i].cls, pct);
		// 更新跟踪值，避免每隔一秒重复记同一件事。
		AcquireSRWLockExclusive(&g_eduWinLock);
		for (int k = 0; k < _countof(g_eduTracked); ++k) {
			if (g_eduTracked[k].hwnd == hWnd) {
				g_eduTracked[k].cx = w;
				g_eduTracked[k].cy = h;
				g_eduTracked[k].tick = GetTickCount64();
				break;
			}
		}
		ReleaseSRWLockExclusive(&g_eduWinLock);
		VDebugLog(L"windowed size remembered class=%s %dx%d pct=%d (work=%dx%d)",
			snaps[i].cls[0] ? snaps[i].cls : L"-", w, h, pct, ww, wh);
	}
}

BOOL WINAPI hkSetForegroundWindow(HWND hWnd)
{
	VTraceHook(L"hkSetForegroundWindow");
	return raSetForegroundWindow(hWnd);
}

BOOL WINAPI hkSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
	// 只记录：既没开「拦截远程窗口操作」，也没开强制窗口化时原样放行。
	const bool guardWindowOps = loaded && blockWindowOps;
	const bool windowedMode = loaded && forceWindowed;
	if (!guardWindowOps && !windowedMode) {
		VRecordHook(L"SetWindowPos", L"hwnd=%p x=%d y=%d cx=%d cy=%d flags=0x%X", hWnd, x, y, cx, cy, uFlags);
		return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
	}
	VTraceHook(L"hkSetWindowPos");
	if (loaded)
	{
		// 正在弹「全屏请求」的框：这段时间原样放行。框自己也要走 SetWindowPos，
		// 被摘掉置顶就藏到广播层后面去了。
		if (g_eduAsking) return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
		const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
		// 全屏请求：吞掉它只能让宿主以为自己是全屏，底下的绘制层照旧铺满，
		// 屏幕上就剩一片黑。改成工作区里的窗口再放行，宿主会跟着重排画面。
		// askFullScreen 打开时先问一句，答「是」就走原样放行的老路。
		if (!fakeAllowed && windowedMode && (uFlags & SWP_NOSIZE) == 0
			&& VEduShouldWindow(hWnd, cx, cy)) {
			VEduForceWindowed(hWnd, x, y, cx, cy);
			// 位置一起重设：请求里可能带 SWP_NOMOVE，那样窗口会卡在旧角落。
			return raSetWindowPos(hWnd, blockTopmost ? HWND_NOTOPMOST : hWndInsertAfter,
				x, y, cx, cy, uFlags & ~(SWP_NOSIZE | SWP_NOMOVE));
		}
		if (!fakeAllowed && blockTopmost && !VEduFullScreenAllowed(hWnd)) {
			if (hWndInsertAfter == HWND_TOPMOST) hWndInsertAfter = HWND_NOTOPMOST;
			// 样式可能是别处加的（SetWindowLongPtr / 系统还原），顺手摘掉。
			VEduNormalizeTopmost(hWnd);
		}

		if (guardWindowOps && (uFlags & SWP_NOSIZE) == 0)
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

		if (guardWindowOps && VIsInIllegalWindows(hWnd)) 
			return TRUE;
	}
	return raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
}

HDWP WINAPI hkDeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
	const bool guardWindowOps = loaded && blockWindowOps;
	const bool windowedMode = loaded && forceWindowed;
	if (!guardWindowOps && !windowedMode) {
		VRecordHook(L"DeferWindowPos", L"hwnd=%p x=%d y=%d cx=%d cy=%d flags=0x%X", hWnd, x, y, cx, cy, uFlags);
		return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
	}
	VTraceHook(L"hkDeferWindowPos");
	if (loaded)
	{
		if (g_eduAsking) return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
		const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
		if (!fakeAllowed && windowedMode && (uFlags & SWP_NOSIZE) == 0
			&& VEduShouldWindow(hWnd, cx, cy)) {
			VEduForceWindowed(hWnd, x, y, cx, cy);
			return faDeferWindowPos(hWinPosInfo, hWnd, blockTopmost ? HWND_NOTOPMOST : hWndInsertAfter,
				x, y, cx, cy, uFlags & ~(SWP_NOSIZE | SWP_NOMOVE));
		}
		if (!fakeAllowed && blockTopmost && !VEduFullScreenAllowed(hWnd)) {
			if (hWndInsertAfter == HWND_TOPMOST) hWndInsertAfter = HWND_NOTOPMOST;
			VEduNormalizeTopmost(hWnd);
		}

		if (guardWindowOps && (uFlags & SWP_NOSIZE) == 0)
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

		if (guardWindowOps && VIsInIllegalWindows(hWnd))
			return NULL;
	}
	return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
}

BOOL WINAPI hkMoveWindow(HWND hWnd, int x, int y, int cx, int cy, BOOL bRepaint)
{
	const bool guardWindowOps = loaded && blockWindowOps;
	const bool windowedMode = loaded && forceWindowed;
	if (!guardWindowOps && !windowedMode) {
		VRecordHook(L"MoveWindow", L"hwnd=%p x=%d y=%d cx=%d cy=%d", hWnd, x, y, cx, cy);
		return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
	}
	VTraceHook(L"hkMoveWindow");
	if (loaded)
	{
		if (g_eduAsking) return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
		const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
		if (!fakeAllowed && windowedMode && VEduShouldWindow(hWnd, cx, cy)) {
			VEduForceWindowed(hWnd, x, y, cx, cy);
			return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
		}
		if (!fakeAllowed && blockTopmost && !VEduFullScreenAllowed(hWnd)) VEduNormalizeTopmost(hWnd);

		if (guardWindowOps && x == 0 && y == 0 && cx == screenWidth && cy == screenHeight) {
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
		if (guardWindowOps && VIsInIllegalWindows(hWnd))
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
	const bool guardWindowOps = loaded && blockWindowOps;
	const bool topGuard = loaded && blockTopmost;
	if (!guardWindowOps && !topGuard) {
		VRecordHook(L"SetWindowLongA", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetWindowLongA(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetWindowLongA");
	if (loaded) {
		// 弹框期间放行：框要给自己加 WS_EX_TOPMOST。
		if (g_eduAsking) return faSetWindowLongA(hWnd, nIndex, dwNewLong);
		const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
		// 置顶样式在这里就被掩掉：宿主用 SetWindowLong(GWL_EXSTYLE) 加 TOPMOST
		// 是最常见的写法之一，等下一次 SetWindowPos 再摘就已经晚了一帧。
		if (topGuard && !fakeAllowed && nIndex == GWL_EXSTYLE && (dwNewLong & WS_EX_TOPMOST))
			dwNewLong &= ~WS_EX_TOPMOST;
		if (guardWindowOps) {
			if (fakeAllowed)
				return faSetWindowLongA(hWnd, nIndex, dwNewLong);
			if (VIsInIllegalWindows(hWnd))
				return GetWindowLongA(hWnd, nIndex);
		}
	}
	return faSetWindowLongA(hWnd, nIndex, dwNewLong);
}

LONG WINAPI hkSetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
	const bool guardWindowOps = loaded && blockWindowOps;
	const bool topGuard = loaded && blockTopmost;
	if (!guardWindowOps && !topGuard) {
		VRecordHook(L"SetWindowLongW", L"hwnd=%p index=%d value=0x%X", hWnd, nIndex, dwNewLong);
		return faSetWindowLongW(hWnd, nIndex, dwNewLong);
	}
	VTraceHook(L"hkSetWindowLongW");
	if (loaded) {
		if (g_eduAsking) return faSetWindowLongW(hWnd, nIndex, dwNewLong);
		const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
		if (topGuard && !fakeAllowed && nIndex == GWL_EXSTYLE && (dwNewLong & WS_EX_TOPMOST))
			dwNewLong &= ~WS_EX_TOPMOST;
		if (guardWindowOps) {
			if (fakeAllowed)
				return faSetWindowLongW(hWnd, nIndex, dwNewLong);
			if (VIsInIllegalWindows(hWnd))
				return GetWindowLongW(hWnd, nIndex);
		}
	}
	return faSetWindowLongW(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI hkShowWindow(HWND hWnd, int nCmdShow)
{
	VTraceHook(L"hkShowWindow");
	if (loaded && g_eduAsking) return faShowWindow(hWnd, nCmdShow);
	// 宿主用 ShowWindow(SW_MAXIMIZE) 进全屏是另一条路：它不走 SetWindowPos 的
	// 参数，系统内部直接摆窗口。这里先按窗口化尺寸摆好，再以普通方式显示。
	const bool fakeAllowed = fakeFull && VIsWindowGbOrHp(hWnd);
	if (loaded && forceWindowed && !fakeAllowed
		&& (nCmdShow == SW_MAXIMIZE || nCmdShow == SW_MAX || nCmdShow == SW_SHOWMAXIMIZED)
		&& VEduShouldWindow(hWnd, screenWidth, screenHeight)) {
		int x = 0, y = 0, cx = 0, cy = 0;
		VEduWindowedRect(hWnd, x, y, cx, cy);
		VEduEnsureResizable(hWnd);
		if (blockTopmost) VEduDropTopmost(hWnd);
		VEduTrackWindow(hWnd, cx, cy);
		if (raSetWindowPos)
			raSetWindowPos(hWnd, blockTopmost ? HWND_NOTOPMOST : NULL, x, y, cx, cy,
				SWP_NOZORDER | SWP_NOACTIVATE);
		if (faShowWindow) return faShowWindow(hWnd, SW_SHOWNORMAL);
		return ShowWindow(hWnd, SW_SHOWNORMAL);
	}
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
	// 窗口本来就是整屏尺寸，宿主这会儿才让它出现：先摆好再 show，不然屏幕上会先
	// 铺满一整块，等巡查（1.2 秒）才改小 —— 用户看到的就是「全屏照样铺满」。
	if (loaded && forceWindowed && !fakeAllowed
		&& nCmdShow != SW_HIDE && nCmdShow != SW_MINIMIZE && nCmdShow != SW_SHOWMINIMIZED)
		VEduWindowIfFullScreen(hWnd);
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
