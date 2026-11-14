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
// 只记录模式下的动作记录：按总量采样，避免热路径刷爆日志文件。
void VRecordHook(const wchar_t* name, const wchar_t* fmt, ...);
// 视频流替换是否开启（g_VideoProps 定义在 DzjsTrainerHooks.cpp，guard 模块看不到它）。
bool VVideoReplacementEnabled();
extern bool doNotShowVirusWindow, forceDisableWatchDog;
extern bool whitePreviewOnly, captureHelperForwarding;
// 「进程注入」通用宿主标记（定义在 DzjsTrainerHooks.cpp）
extern bool g_customHost, g_customHookMinimal;
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
