#pragma once
// ============================================================================
// HooksCommon.h —— hook 分域后的共享状态声明
//
// 参考 R3ShieldCore 的 guard 模块范式：每个 Guard*.cpp 只负责自己那个域的
// detour 逻辑，进程级状态一律集中在这里声明、由 DzjsTrainerHooks.cpp 定义。
// 安装顺序单独由 HookRegistry.cpp 的**有序表**维护，域的划分不改变
// HooksLimit=N 的「前 N 个」语义。
// ============================================================================

#include "stdafx.h"
#include "DzjsTrainerHooks.h"
#include "HookEngine.h"
#include "ReplaceState.h"
#include "StringHlp.h"
#include "StringSplit.h"

#include <list>
#include <vector>
#include <string>
#include <set>
#include <new>
#include <Shlwapi.h>
#include <ShellAPI.h>
#include <CommCtrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <TlHelp32.h>

// 低级钩子失效标记（原在主文件，guard 模块也要用）
#define INVALID_HHOOK_MOUSE 0x0001
#define INVALID_HHOOK_KEYBOARD 0x0002

// ---------------------------------------------------------------------------
// 分域与安装描述
// ---------------------------------------------------------------------------
enum class HookDomain { Window, Process, Input, Display };

enum class HookSpecKind {
	Install,            // 常规安装
	InstallIfOriginal,  // 仅当原始函数指针已解析（DwmEnableComposition 等）
	SkipWithLog,        // 明确不安装，只留一行诊断日志（hk37/38/41）
	Action              // 在序列的这一位置执行一个动作（如 VEnsureJPEGHook）
};

struct HookSpec {
	int            seq;        // 原 hkN 编号；注册表按此排序，保证调试开关语义不变
	HookDomain     domain;
	const wchar_t* name;
	PVOID*         original;
	PVOID          detour;
	BOOL*          flag;
	bool           useLimit;   // 是否经过 VHookAllowed()（影响 HooksLimit 计数）
	HookSpecKind   kind;
	void           (*action)();
};

namespace HookRegistry {
	// 按 kFullSet 的原始顺序安装整套钩子。调用方需已持有 ScopedBatch。
	void InstallFullSet();
	int  CountByDomain(HookDomain domain);
	int  TotalInstalled();
}

// ---------------------------------------------------------------------------
// 由 DzjsTrainerHooks.cpp 持有、原为 static 的调试状态
// ---------------------------------------------------------------------------
extern bool g_hookTrace;
void VTraceHook(const wchar_t* name);
bool VHookAllowed();

// 窗口链表锁。VWndListGuard 需要跨 TU 使用，故把临界区操作下沉成外部函数。
void VLockWndList();
void VUnlockWndList();
struct VWndListGuard {
	VWndListGuard() { VLockWndList(); }
	~VWndListGuard() { VUnlockWndList(); }
	VWndListGuard(const VWndListGuard&) = delete;
	VWndListGuard& operator=(const VWndListGuard&) = delete;
};

// ---------------------------------------------------------------------------
// 全局配置与状态（定义在 DzjsTrainerHooks.cpp）
// ---------------------------------------------------------------------------
extern HINSTANCE hInst;
extern WNDPROC jiYuWndProc, jiYuTDDeskWndProc;
extern std::list<HWND> jiYuWnds, jiYuWndCanSize;
extern HWND jiYuGBWnd, jiYuGBDeskRdWnd, jiYuGBToolWnd;
extern int jiYuGBToolHeight;
extern HWND hWndMsgCenter, hListBoxStatus;
extern std::vector<std::wstring> statusLines;
extern HWND desktopWindow, fakeDesktopWindow, mainWindow;
extern HMENU hMenuGb, hMenuGbP;
extern HANDLE hThreadMain, hThreadInitialize;
extern DWORD uiThreadId, initializeThreadId;
extern LRESULT hWndOpConformRs, hWndOutOfControlConformRs;
extern INT screenWidth, screenHeight;
extern bool outlineEndJiy;
extern HWND hWndOpConformNoBtn;
extern bool bandAllRunOp, allowNextRunOp, allowAllRunOp;
extern bool ProhibitKillProcess, ProhibitCloseWindow;
extern bool allowMonitor, allowControl, allowGbTop, fakeFull, gbFullManual;
// 「是否动手」开关（2026-11-14）。全部默认 0 = **只记录不拦截**：
// detour 先看开关，关着就记一条 RECORD 日志后原样调用原函数，行为与没装钩子一致。
// 用户在「防护策略」页打开对应选项后才真正拦截或替换。
//   blockWindowOps     窗口尺寸/位置/前台/样式（SetWindowPos/MoveWindow/…）
//   blockDriverAccess  极域驱动设备与通信端口（TDKeybd/TDProcHook/TDNetFilter）
// 其余行为复用已有开关：allowAllRunOp（远程运行）、ProhibitKillProcess（结束进程/关机）、
// ProhibitCloseWindow（关闭窗口）、allowControl（远程控制/输入注入）、allowMonitor（查看屏幕）。
extern bool blockWindowOps, blockDriverAccess;
// 全屏 / 置顶的现场询问（2026-11-21）。**默认开**，与 BlockWindowOps 分开：
// 那个开关管的是「静默拦下所有窗口操作」，而这里只是把最扎眼的两种先问一句，
// 用户点了是就原样放行，不算拦截。ini 里写 AskWindowOps=FALSE 可关。
extern bool askWindowOps;
// 关机拦截（2026-11-21）。与 ProhibitKillProcess 分开是刻意的：那个开关默认关，
// 而且只管「结束进程」。关机/重启/注销单独一根开关，默认**开** —— 学生机上
// 被教师端关掉一次就得重来，代价比误拦一次大得多。ini 里写 BlockShutdown=FALSE 可关。
extern bool blockShutdown;
// 告诉主程序「刚刚挡下了一次关机」。内部会起线程走 WM_COPYDATA，不阻塞调用方。
void VNotifyShutdownBlocked(const wchar_t* api);

// ===== 全屏 / 置顶的现场询问（2026-11-21）===================================
// 被注入窗口要全屏或置顶时，不再静默吞掉，而是问用户一句：允许就按原参数
// 重放这次调用，拒绝就记住，之后直接挡。决定按窗口记，窗口销毁时清；
// 还没有消息中心可问时直接放行（不静默干预）。
//
// 覆盖范围不只有极域：极域走 jiYuWnds 名单，其余适配的电子教室（红蜘蛛、
// 伽卡他卡、联想传奇，以及「进程注入」指定的任意宿主）走 VWindowOpAskable
// 的通用判据 —— 本进程的顶层窗口，且这次操作要把它变成铺满屏幕的大窗口。
// 名单没有这些产品的窗口，硬编码标题也认不全，只能按「谁在把自己铺满」认。
//
// 为什么不在钩子里同步弹框：这些调用发生在极域自己的 UI 线程上，模态框会把
// 「全屏」那一步卡住直到用户回答，中间的绘制和计时器全部停摆。所以第一次只
// 拦不问，PostMessage 到消息中心线程弹框，用户答「是」以后再重放。
enum : int {
	VOP_SRC_SETWINDOWPOS  = 0,
	VOP_SRC_MOVEWINDOW    = 1,
	VOP_SRC_SETWINDOWLONG = 2,
	// ShowWindow(SW_MAXIMIZE)：没有尺寸参数，cx 字段借来存 nCmdShow。
	VOP_SRC_SHOWWINDOW    = 3,
	// 后台巡查发现的「已经在整屏、但一次窗口 API 都没走过」的窗口。
	// 没有调用可重放（窗口本来就摆在那儿），答「是」什么也不用做。
	VOP_SRC_SWEEP         = 4,
};

struct WindowOpRequest {
	int   source;
	HWND  hwnd;
	HWND  insertAfter;
	int   x, y, cx, cy;
	UINT  flags;
	int   index;
	LONG  value;
	bool  wide;        // SetWindowLong 的 A/W 版本
};

// 返回 true = 调用方继续执行原操作；false = 已拦下，等用户答复后由 UI 线程重放。
bool VAllowWindowOp(const WindowOpRequest& req, bool full, bool top);
// 这个窗口的这次操作要不要问一句。极域看名单，其他宿主看「是不是它自己的
// 顶层窗口、是不是在铺满屏幕」。GuardWindow 在 blockWindowOps 分支里也要用它
// 决定该不该走询问路径，所以单独导出。
bool VWindowOpAskable(HWND hWnd, const WindowOpRequest* req, bool full, bool top);
// 第三方宿主的全屏请求未必严格写成 (0,0,主屏宽,主屏高)：有的用工作区尺寸，
// 有的按窗口所在显示器算，高 DPI 下还会差几个像素。这里放宽到
// 「贴住屏幕左上角 + 铺满所在显示器 90%」。极域那条精确判据不受影响。
bool VEduFullScreenRequest(HWND hWnd, int x, int y, int cx, int cy);
// 窗口销毁时清掉决定与待重放请求。
void VForgetWindowOp(HWND hWnd);
// 右键菜单「允许全屏和置顶」：改成允许，并把上一次被挡的请求补上。
void VGrantWindowOps(HWND hWnd);

// 只记录模式下的动作记录：按总量采样，避免热路径刷爆日志文件。
void VRecordHook(const wchar_t* name, const wchar_t* fmt, ...);
// 视频流替换是否开启（g_VideoProps 定义在 DzjsTrainerHooks.cpp，guard 模块看不到它）。
bool VVideoReplacementEnabled();
extern bool doNotShowVirusWindow, forceDisableWatchDog;
extern bool whitePreviewOnly, captureHelperForwarding;
// 「进程注入」通用宿主标记（定义在 DzjsTrainerHooks.cpp）。
// g_eduHost 表示宿主同时命中 EduProducts.h 的产品表，显示域按极域口径处理。
extern bool g_customHost, g_customHookMinimal, g_eduHost;
extern bool isLocked, gbCurrentIsTop, isGbFounded;
extern std::list<std::wstring> runOPWhiteList;
extern bool forceKill;
extern int wdCount;
extern WCHAR mainIniPath[MAX_PATH];
extern WCHAR mainFullPath[MAX_PATH];
extern WCHAR mainDir[MAX_PATH];
extern WCHAR currOpCfPath[MAX_PATH];
extern WCHAR currOpCfPararm[MAX_PATH];
extern HWND gbWindow;
extern DWORD currentPid;

// 原始函数指针（VInstallHooks 用 GetProcAddress 填充）
extern fnTDAjustCreateInstance faTDAjustCreateInstance;
extern fnUnLockLocalInput UnLockLocalInput;
extern fnSetWindowPos raSetWindowPos;
extern fnMoveWindow raMoveWindow;
extern fnSetForegroundWindow raSetForegroundWindow;
extern fnBringWindowToTop faBringWindowToTop;
extern fnDeviceIoControl raDeviceIoControl;
extern fnCreateFileA faCreateFileA;
extern fnCreateFileW faCreateFileW;
extern fnSetWindowsHookExA faSetWindowsHookExA;
extern fnDeferWindowPos faDeferWindowPos;
extern fnSendInput faSendInput;
extern fnmouse_event famouse_event;
extern fnChangeDisplaySettingsW faChangeDisplaySettingsW;
extern fnTDDeskCreateInstance faTDDeskCreateInstance;
extern fnSetWindowLongA faSetWindowLongA;
extern fnSetWindowLongW faSetWindowLongW;
extern fnShowWindow faShowWindow;
extern fnExitWindowsEx faExitWindowsEx;
// 关机拦截的另外七条路（advapi32 / ntdll）。解析失败时对应钩子不装。
extern fnInitiateSystemShutdownExW faInitiateSystemShutdownExW;
extern fnInitiateSystemShutdownExA faInitiateSystemShutdownExA;
extern fnInitiateSystemShutdownW faInitiateSystemShutdownW;
extern fnInitiateSystemShutdownA faInitiateSystemShutdownA;
extern fnNtShutdownSystem faNtShutdownSystem;
extern fnNtSetSystemPowerState faNtSetSystemPowerState;
extern fnNtInitiatePowerAction faNtInitiatePowerAction;
extern fnShellExecuteW faShellExecuteW;
extern fnShellExecuteExW faShellExecuteExW;
extern fnCreateProcessA faCreateProcessA;
extern fnCreateProcessW faCreateProcessW;
extern fnDwmEnableComposition faDwmEnableComposition;
extern fnWinExec faWinExec;
extern fnCallNextHookEx faCallNextHookEx;
extern fnGetDesktopWindow faGetDesktopWindow;
extern fnGetWindowDC faGetWindowDC;
extern fnEncodeToJPEGBuffer faEncodeToJPEGBuffer;
extern fnBitBlt fpBitBlt;
// 截屏的另外三条路（移植自 R3ShieldCore/screen_guard.cpp）。
// 只钩 BitBlt 挡不住截屏：GetDC(NULL) 之后直接 GetDIBits 读像素、
// 或者 StretchBlt 缩放拷贝、或者 PrintWindow 按窗口抓，都能绕过去。
extern fnPrintWindow faPrintWindow;
extern fnGetDIBits faGetDIBits;
extern fnStretchBlt faStretchBlt;
extern fnSetWindowsHookExW faSetWindowsHookExW;
extern fnGetForegroundWindow faGetForegroundWindow;
extern fnCreateDCW faCreateDCW;
extern fnEnableMenuItem faEnableMenuItem;
extern fnSetClassLongA faSetClassLongA;
extern fnSetClassLongW faSetClassLongW;
extern fnUnhookWindowsHookEx faUnhookWindowsHookEx;
extern fnPostMessageW faPostMessageW;
extern fnSendMessageW faSendMessageW;
extern fnTerminateProcess faTerminateProcess;
extern fnFilterConnectCommunicationPort faFilterConnectCommunicationPort;

extern bool loaded;
extern HHOOK g_hhook;
extern HMODULE hLibTDMaster;
// 极域防破解驱动的设备句柄（VOpenFuckDrivers / VCloseFuckDrivers 管理）
extern HANDLE hDeviceTDKeybd, hDeviceTDProcHook, hDeviceTDNetFilter;
extern bool g_Unloading;
extern ReplaceState::ExternalSurface g_surface;
extern ReplaceState::FrameStore g_frames;

// 安装标记：seq 与编号一一对应
extern BOOL hk1, hk2, hk3, hk4, hk5, hk6, hk7, hk8, hk9, hk10;
extern BOOL hk11, hk12, hk13, hk14, hk15, hk16, hk17, hk18, hk19, hk20;
extern BOOL hk21, hk22, hk23, hk24, hk25, hk26, hk27, hk28, hk29, hk30;
extern BOOL hk31, hk32, hk33, hk34, hk35, hk36, hk37, hk38, hk39, hk40;
extern BOOL hk41;
// hk42..hk45 从 R3ShieldCore 移植：截屏另外三条路 + SetWindowsHookExW。
extern BOOL hk42, hk43, hk44, hk45;
// hk46..hk52：关机拦截。
extern BOOL hk46, hk47, hk48, hk49, hk50, hk51, hk52;

extern volatile LONG inputHookLogCount;
extern volatile LONG captureHelperProcessLogCount;
extern volatile LONG jpegHookAttempted;
extern volatile LONG jpegHookLogPending;
extern volatile LONG frameReplacementReported;
extern volatile LONG frameReplacementInvalidReported;
extern volatile LONG frameReplacementLogPending;
extern volatile LONG frameReplacementLogWidth;
extern volatile LONG frameReplacementLogHeight;
extern volatile LONG frameReplacementLogStride;
extern volatile LONG debugLogWriting;
extern volatile LONG bitBltCallCount;
extern volatile LONG jpegCallCount;
extern volatile LONG watchdogRunning;
extern SRWLOCK handleMsgLock;
extern WCHAR debugLogPath[MAX_PATH];

// ---------------------------------------------------------------------------
// DzjsTrainerHooks.h 未覆盖、但 guard 模块会调用的辅助函数
// ---------------------------------------------------------------------------
std::wstring VGetExtension(std::wstring path);
bool VIsInIllegalCanSizeWindows(HWND hWnd);
bool VIsInIllegalWindowMessage(UINT msg);
bool VIsWindowGb(HWND hWnd);
bool VIsWindowHp(HWND hWnd);
bool VIsWindowGbOrHp(HWND hWnd);
void VSwitchLockState(bool l);
void VSwitchLockState(HWND hWnd, bool l);
BOOL VInjectHookIntoProcess(HANDLE process);
void VAppendStatusLine(const std::wstring& status);
