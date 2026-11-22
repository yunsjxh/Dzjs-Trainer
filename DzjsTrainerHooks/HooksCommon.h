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
// 关机拦截（2026-11-21）。与 ProhibitKillProcess 分开是刻意的：那个开关默认关，
// 而且只管「结束进程」。关机/重启/注销单独一根开关，默认**开** —— 学生机上
// 被教师端关掉一次就得重来，代价比误拦一次大得多。ini 里写 BlockShutdown=FALSE 可关。
extern bool blockShutdown;
// 强制窗口化（2026-11-22）。默认开，不依赖 BlockWindowOps：电子教室把窗口摆成
// 整屏时，把请求改写成工作区内的一块窗口再放行，而不是吞掉 —— 吞掉会让宿主
// 以为自己是全屏，底下的绘制层照旧铺满，屏幕上只剩一片黑。
//   WindowedPercent  窗口占工作区的百分比（25..95，默认 70）
//   BlockTopmost     摘掉 WS_EX_TOPMOST / HWND_TOPMOST（默认开）
//   AskFullScreen    全屏请求先弹框问一次；答「是」放行，答「否」才改成窗口。
//                    默认开。关掉就是「直接改窗口」，不打扰。
extern bool forceWindowed, blockTopmost, askFullScreen;
extern int  windowedPercent;
// 采样被强制窗口化的窗口，把用户手动拖过的尺寸按窗口类记下来。消息中心定时器调用。
void VEduSampleTrackedWindows();
// 巡查：本进程里「创建时就铺满整屏」的窗口不经过 SetWindowPos，钩子看不见，
// 只能靠这一趟枚举把它们也改成窗口。判据见 GuardWindow.cpp。
void VEduSweepFullScreenWindows();
// 窗口已经是整屏尺寸、这会儿才让它出现（ShowWindow）时，同一套判据同步补一刀，
// 免得屏幕上先铺满一整块、等 1.2 秒后的巡查才改小。
void VEduWindowIfFullScreen(HWND hWnd);
// 卸载时清掉跟踪表，避免把旧 hwnd 带到下一次注入。
void VEduResetWindowedState();
// 把「窗口被改成窗口化」这件事告诉主程序，进主日志。异步，不阻塞钩子。
void VNotifyWindowed(HWND hWnd, const WCHAR* cls, int fromW, int fromH, int toW, int toH);
// 全屏请求问过之后的结论也进主日志：用户反馈「全屏还是铺满」时，先看有没有这一行。
void VNotifyFullScreenAsk(const WCHAR* cls, bool allow, int cx, int cy);
// 告诉主程序「刚刚挡下了一次关机」。内部会起线程走 WM_COPYDATA，不阻塞调用方。
void VNotifyShutdownBlocked(const wchar_t* api);
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
