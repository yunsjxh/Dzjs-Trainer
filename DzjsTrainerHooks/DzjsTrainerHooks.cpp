
// JiYuKillerVirus.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "DzjsTrainerHooks.h"
#include "resource.h"
#include "StringHlp.h"
#include "StringSplit.h"
#include "HookEngine.h"
#include "ReplaceCodec.h"
#include "ReplaceState.h"
#include "HooksCommon.h"
#include "GuardWindow.h"
#include "GuardProcess.h"
#include "GuardInput.h"
#include "GuardDisplay.h"
#include <list>
#include <string>
#include <vector>
#include <set>
#include <time.h>
#include <stdio.h>
#include <Shlwapi.h>
#include <ShellAPI.h>
#include <CommCtrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <TlHelp32.h>
#include <new>
#include <gdiplus.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#define TIMER_WATCH_DOG_SRV 10011
#define TIMER_AUTO_HIDE 10012
#define TIMER_COUNTDOWN_TICK 10013
#define TIMER_LIGNT_DELAY1 10014
#define TIMER_LIGNT_DELAY2 10015
#define TIMER_UI_HEARTBEAT 10016

#define DirectorySeparatorChar L'\\'
#define AltDirectorySeparatorChar  L'/'
#define VolumeSeparatorChar  L':'

#define IDC_SW_STATUS_FAKEFULL 20001
#define IDC_SW_STATUS_LOCKED 20002
#define IDC_SW_STATUS_MAIN 20003
#define IDC_SW_STATUS_MAIN_OUT 20004
#define IDC_SW_STATUS_MAIN_FLASH 20005
#define IDC_SW_STATUS_RB_FLASH 20006
#define IDC_SW_STATUS_RB 20007
#define IDC_SW_STATUS_RB_FAIL 20008
#define IDC_SW_STATUS_RB_FAIL_FLASH 20009
#define IDC_SW_STATUS_RB_FLASH_FAST 20010

#define IDM_TOPMOST 25600
#define IDM_FULL 25601
#define IDM_HELP_GB 25602
#define IDM_CLOSE_GB 25603
#define IDM_HELP_GB2 25604

// INVALID_HHOOK_MOUSE / INVALID_HHOOK_KEYBOARD 已移到 HooksCommon.h 供 guard 模块共用

using namespace std;

extern HINSTANCE hInst;

WNDPROC jiYuWndProc;
WNDPROC jiYuTDDeskWndProc;
list<HWND>  jiYuWnds;
list<HWND>  jiYuWndCanSize;

// jiYuWnds / jiYuWndCanSize 会被**多个线程并发读写**：钩子在极域的各个线程里跑，
// VIsInIllegalWindows 遍历它们的同时，别的线程可能正在 push_back / remove。
// std::list 被并发修改会让遍历陷入死循环 —— 表现就是**目标进程挂起**（AppHangB1），
// 而且不打日志、不崩溃。加锁保护。
static CRITICAL_SECTION g_wndListCs;
static volatile LONG g_wndListCsInit = 0;
static void VEnsureWndListCs()
{
	if (InterlockedCompareExchange(&g_wndListCsInit, 1, 0) == 0)
		InitializeCriticalSection(&g_wndListCs);
}
// 临界区本身留在主翻译单元；VWndListGuard 的构造/析构通过下面两个外部函数
// 进入，这样 guard 模块也能使用同一个锁而不需要把 CRITICAL_SECTION 变成 extern。
void VLockWndList() { VEnsureWndListCs(); EnterCriticalSection(&g_wndListCs); }
void VUnlockWndList() { LeaveCriticalSection(&g_wndListCs); }
HWND jiYuGBWnd = NULL;
HWND jiYuGBDeskRdWnd = NULL;
HWND jiYuGBToolWnd = NULL;
int jiYuGBToolHeight = 0;

enum jiYuVersions {
	jiYuVersionsAuto,
	jiYuVersions40,
	jiYuVersions402016HH,
};
jiYuVersions jiYuVersion = jiYuVersionsAuto;

HWND hWndMsgCenter = NULL;
HWND hListBoxStatus = NULL;
std::vector<std::wstring> statusLines;

// 主窗口标题是展示文案：用户自改版把 MAIN_WND_NAME 换成了
// "Dzjs Trainer · 设备防护中心"，而窗口类名 MAIN_WND_CLS_NAME
// (DzjsTrainerNativeWindow) 一直没变。这里原来只按两个历史标题查找，
// 标题一改就永远返回 NULL，VSendMessageBack() 于是误判「与主程序失去联系」
// 并弹出 IDD_OUTCTL —— 即使主窗口明明在。改为优先按类名查找，
// 标题只作为兼容旧版本 / 改版文案的 fallback。
constexpr wchar_t kDzjsControlWindowClass[]   = L"DzjsTrainerNativeWindow";
constexpr wchar_t kDzjsDisplayWindowTitle[]   = L"Dzjs Trainer \u00b7 \u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3";
constexpr wchar_t kDzjsControlWindowTitle[]   = L"Dzjs Trainer Main Window";
constexpr wchar_t kLegacyControlWindowTitle[] = L"JiYu Trainer Main Window";

static HWND FindControlWindow()
{
	HWND window = FindWindowW(kDzjsControlWindowClass, nullptr);
	if (window) return window;
	window = FindWindowW(nullptr, kDzjsDisplayWindowTitle);
	if (window) return window;
	window = FindWindowW(nullptr, kDzjsControlWindowTitle);
	if (window) return window;
	return FindWindowW(nullptr, kLegacyControlWindowTitle);
}

HWND desktopWindow, fakeDesktopWindow;
HWND mainWindow;

HMENU hMenuGb = NULL;
HMENU hMenuGbP;
HANDLE hThreadMain = NULL;
HANDLE hThreadInitialize = NULL;
DWORD uiThreadId = 0;
DWORD initializeThreadId = 0;
const UINT WM_HOOK_INITIALIZED = WM_APP + 1;
const UINT WM_STATUS_APPEND = WM_APP + 2;
DWORD WINAPI VHookInitializeThread(LPVOID lpThreadParameter);
DWORD WINAPI VWatchdogThread(LPVOID lpThreadParameter);
DWORD WINAPI VSendMessageBackThread(LPVOID lpThreadParameter);
DWORD WINAPI VHandleMsgThread(LPVOID lpThreadParameter);
DWORD WINAPI VHandshakeRetryThread(LPVOID lpThreadParameter);
BOOL VInjectHookIntoProcess(HANDLE process);
LRESULT hWndOpConformRs = 0;
LRESULT hWndOutOfControlConformRs = 0;
INT screenWidth, screenHeight;
bool outlineEndJiy = false;
HWND hWndOpConformNoBtn = NULL;
bool bandAllRunOp = false, allowNextRunOp = false, allowAllRunOp  = false, ProhibitKillProcess = false, ProhibitCloseWindow = false;
bool allowMonitor = false, allowControl = false, allowGbTop = false, fakeFull = false, gbFullManual = false,
doNotShowVirusWindow = true, forceDisableWatchDog = false;
// 「是否动手」开关，默认全 0 = 只记录（见 HooksCommon.h 的说明）。
bool blockWindowOps = false, blockDriverAccess = false;
bool whitePreviewOnly = false;
// 「进程注入」功能：主程序把本 DLL 注入到用户指定的非极域进程时，
// VLoad() 走通用宿主分支。通用宿主不等待 LibTDMaster、不改教师端设置，
// 但仍需要装 hook 表，否则 DLL 只是被加载、什么都不做。
bool g_customHost = false;
// [CustomInject] Mode=minimal 时跳过极域专属的显示域/驱动通信/远程运行钩子。
bool g_customHookMinimal = false;
// 钩子调用跟踪开关，默认关（ini [JTSettings] HooksTrace=1 打开）。
// 声明必须早于 VIsInIllegalWindows —— 那里会用到它。
// 分域后 guard 模块也要读这个开关，故提升为外部链接（HooksCommon.h 里 extern）。
bool g_hookTrace = false;
bool captureHelperForwarding = false;
bool isLocked = false, gbCurrentIsTop = false, isGbFounded = false;
std::list<std::wstring> runOPWhiteList;
bool forceKill = false;
int wdCount = 0;
WCHAR mainIniPath[MAX_PATH];
WCHAR mainFullPath[MAX_PATH];
WCHAR mainDir[MAX_PATH];
WCHAR currOpCfPath[MAX_PATH];
WCHAR currOpCfPararm[MAX_PATH];
HWND gbWindow = NULL;
DWORD currentPid = 0;

fnTDAjustCreateInstance faTDAjustCreateInstance = NULL;

fnUnLockLocalInput UnLockLocalInput = NULL;
fnSetWindowPos raSetWindowPos = NULL;
fnMoveWindow raMoveWindow = NULL;
fnSetForegroundWindow raSetForegroundWindow = NULL;
fnBringWindowToTop faBringWindowToTop = NULL;
fnDeviceIoControl raDeviceIoControl = NULL;
fnCreateFileA faCreateFileA = NULL;
fnCreateFileW faCreateFileW = NULL;
fnSetWindowsHookExA faSetWindowsHookExA = NULL;
fnDeferWindowPos faDeferWindowPos = NULL;
fnSendInput faSendInput = NULL;
fnmouse_event famouse_event = NULL;
fnChangeDisplaySettingsW faChangeDisplaySettingsW = NULL;
fnTDDeskCreateInstance faTDDeskCreateInstance = NULL;
fnSetWindowLongA faSetWindowLongA = NULL;
fnSetWindowLongW faSetWindowLongW = NULL;
fnShowWindow faShowWindow = NULL;
fnExitWindowsEx faExitWindowsEx = NULL;
fnShellExecuteW faShellExecuteW = NULL;
fnShellExecuteExW faShellExecuteExW = NULL;
fnCreateProcessA faCreateProcessA = NULL;
fnCreateProcessW faCreateProcessW = NULL;
fnDwmEnableComposition faDwmEnableComposition = NULL;
fnWinExec faWinExec = NULL;
fnCallNextHookEx faCallNextHookEx = NULL;
fnGetDesktopWindow faGetDesktopWindow = NULL;
fnGetWindowDC faGetWindowDC = NULL;
fnEncodeToJPEGBuffer faEncodeToJPEGBuffer = NULL;
fnBitBlt fpBitBlt = NULL;
fnPrintWindow faPrintWindow = NULL;
fnGetDIBits faGetDIBits = NULL;
fnStretchBlt faStretchBlt = NULL;
fnSetWindowsHookExW faSetWindowsHookExW = NULL;
fnGetForegroundWindow faGetForegroundWindow = NULL;
fnCreateDCW faCreateDCW = NULL;
fnEnableMenuItem faEnableMenuItem = NULL;
fnSetClassLongA faSetClassLongA = NULL;
fnSetClassLongW faSetClassLongW = NULL;
fnUnhookWindowsHookEx faUnhookWindowsHookEx = NULL;
fnPostMessageW  faPostMessageW = NULL;
fnSendMessageW faSendMessageW = NULL;
fnTerminateProcess faTerminateProcess = NULL;
fnFilterConnectCommunicationPort faFilterConnectCommunicationPort = NULL;


bool loaded = false;

HHOOK g_hhook = NULL;

struct VideoStreamProps {
	bool m_EnableModifyVideoStream = false;
	int m_Width = 0;
	int m_Height = 0;
	HWND m_hPulseWnd = NULL;
} g_VideoProps;

// 视频流替换画面：外部传入的替换 DC（所有权在调用方）。
// 原 g_HdcMem / g_HdcMemCs / g_HdcMemCsInit / g_HdcMemRefs / g_HdcMemPending
// 五个全局收成一个对象，临界区、引用计数、延迟删除都在对象内部（见 ReplaceState.h）。
ReplaceState::ExternalSurface g_surface;
bool g_Unloading = false;

// 视频流自定义图片替换(JPEG 预览与 H.264 backing bitmap 共用的像素来源)
// 原 g_ImgData / g_ImgDibData / g_ImgW / g_ImgH / g_ImgDibStride / g_ImgRefs /
// g_ImgCs / g_ImgCsInit / g_ImgValid / g_ImgPendingBuffers 十个全局收成一个对象。
ReplaceState::FrameStore g_frames;

// 视频替换由独立 Media Foundation 线程解码；捕获热路径只读取已发布的当前帧。
HANDLE g_VideoThread = NULL;
HANDLE g_VideoStopEvent = NULL;
CRITICAL_SECTION g_VideoCs;
bool g_VideoCsInit = false;
volatile bool g_VideoFrameActive = false;
std::wstring g_VideoImagePath;
std::wstring g_VideoPath;
bool g_VideoLoop = true;
std::wstring g_VideoMode = L"video";
struct ReplacementVideoContext {
	std::wstring path;
	bool loop;
	HANDLE stopEvent;
};
static DWORD WINAPI VReplacementVideoThread(LPVOID parameter);
// 三层 JPEG 钩覆盖:实现体 inline / 多模块 IAT / GetProcAddress 拦截
struct JpegIatPatch { ULONG_PTR* entry; ULONG_PTR orig; };
std::vector<JpegIatPatch> g_JpegIatPatches;        // 被改写的 EncodeToJPEGBuffer IAT 槽(卸载还原)
std::vector<JpegIatPatch> g_JpegI422IatPatches;    // 被改写的 EncodeToJPEGBufferI422 IAT 槽
std::vector<JpegIatPatch> g_GpaIatPatches;         // 被改写的 GetProcAddress IAT 槽
volatile bool g_JpegIATHooked = false;             // 任一 EncodeToJPEGBuffer IAT 钩生效
volatile bool g_JpegI422Hooked = false;
volatile bool g_JpegImplHooked = false;            // EncodeToJPEGBuffer 实现体 inline 钩生效
volatile bool g_JpegI422ImplHooked = false;
volatile bool g_JpegGpaHooked = false;             // GetProcAddress 钩是否生效
HMODULE g_hLibJPEG20 = NULL;                        // LibJPEG20 模块基址
static FARPROC (WINAPI *g_pfGetProcAddress)(HMODULE, LPCSTR) = NULL;
// EncodeToJPEGBufferI422 原始函数指针(I422 格式编码,参数布局与 RGB 版一致:
// pixels@+8, width@+c, height@+10, stride@+14, jpegOutput@+18, jpegLength@+1c)
typedef int (__cdecl* fnEncodeToJPEGBufferI422)(const BYTE* pixels, int width, int height, int stride, BYTE* jpegOutput, DWORD* jpegLength, int quality, int reserved1, int reserved2);
static fnEncodeToJPEGBufferI422 faEncodeToJPEGBufferI422 = NULL;

// ============================================================================
// DispFilter.dll DXGI 屏幕捕获拦截("打开看"实时画面)
// IDA 实证:DispFilter.dll 仅按序号导出;ordinal 1 = DispDXGIBitBlt(真正读取指定
// RECT 的桌面像素),ordinal 4 = DispDXGIGetScreenUpdate(仅取脏矩形/更新信息,非像素)。
// DispDXGIBitBlt 把 DXGI 桌面帧以 RGBA/BGRA(每像素 4 字节)逐行 memcpy 进调用方
// 缓冲 lpbyBits(宽高由传入 RECT 计算)。原始函数返回后把整块像素填成不透明黑
// (B=G=R=0, A=0xFF) => 教师端"打开看"看到纯黑。
// 加载关系:StudentMain 经 LibDeskMonitor::TDDeskCreateInstance -> libTDDesk2
// (LoadLibraryW("DispFilter.dll")) 在"打开看"开始时动态加载 DispFilter,故 DispFilter
// 落在 StudentMain 进程内;DispcapHelper 也可能加载它。此钩在任一加载了 DispFilter
// 的进程内安装(由 GetModuleHandleW 判定),不限定具体进程。
// ============================================================================
typedef BOOL (__cdecl *fnDispDXGIBitBlt)(void* pRect, BYTE* lpbyBits);
static fnDispDXGIBitBlt g_origDispDXGIBitBlt = NULL;
volatile bool g_DispDXGIHooked = false;

// ordinal 4 = DispDXGIGetScreenUpdate(outputIndex, update)。update 只保存脏矩形，
// 不包含像素；教师端会保留上一帧中未列入脏区的桌面内容。
struct DispDXGIScreenUpdate {
	DWORD count;
	RECT rects[200];
};
static_assert(sizeof(DispDXGIScreenUpdate) == 0xC84, "unexpected DispDXGIScreenUpdate layout");
typedef BOOL (__cdecl *fnDispDXGIGetScreenUpdate)(int outputIndex, DispDXGIScreenUpdate* update);
static fnDispDXGIGetScreenUpdate g_origDispDXGIGetScreenUpdate = NULL;
volatile bool g_DispDXGIGetScreenUpdateHooked = false;
static volatile LONG g_DispDXGIInstallLock = 0;
static volatile LONG g_DispDXGIRetryStarted = 0;
// VInstallHooks 会被重复调用（运行中切换开关时补装），远程注入线程只能起一次，
// 否则每调一次就多一个线程在枚举并注入捕获辅助进程。
static volatile LONG g_remoteInjectStarted = 0;
static volatile LONG g_DispDXGILastUpdateTick[2] = { 0, 0 };
static volatile LONG g_DispDXGIFullRefreshPending[2] = { 1, 1 };
static std::set<DWORD> g_injectedCaptured;   // 已注入的捕获辅助进程 pid(幂等去重)

// LoadLibrary 钩:DispFilter.dll 一旦被 LoadLibrary 立即装入 DispDXGI 捕获钩,消除首帧空窗。
typedef HMODULE (WINAPI *fnLoadLibraryW)(LPCWSTR);
typedef HMODULE (WINAPI *fnLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static fnLoadLibraryW g_pfLoadLibraryW = NULL;
static fnLoadLibraryExW g_pfLoadLibraryExW = NULL;
static bool hkLLW = false, hkLLX = false;
static HMODULE WINAPI hkLoadLibraryW(LPCWSTR lpLibFileName);
static HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
static DWORD WINAPI VDispDXGIHookRetryThread(LPVOID);
void VStartDispDXGIHookRetryThread();

// DispcapHelper 等独立进程:等待 LibJPEG20 加载后再装入三层 JPEG 钩(声明在前,定义在 VEnsureJPEGHook 之后)
static void VStartJpegHookRetryThread();
static DWORD WINAPI VRemoteInjectThread(LPVOID);   // 枚举并注入已存在的捕获辅助进程

// ============================================================================
// LibDeskMonitor.dll GDI 抓屏拦截("打开看"实时画面)
// IDA 实证:LibDeskMonitor 静态导入 GDI 原语 BitBlt/StretchBlt/CreateCompatibleDC/
// CreateCompatibleBitmap,且 sub_10001570 把"源窗口 DC" BitBlt 进内存位图(捕获)。
// 之前的屏幕遮挡钩(GetDesktopWindow/GetWindowDC/CreateDCW)拦不到它:源窗口 DC 由
// MFC CWnd::GetDC 取自控制器直接给出的窗口句柄,不经过这几个 API。
// => 直接钩 LibDeskMonitor 自身的 BitBlt/StretchBlt IAT,在 screen->memory 捕获后
//    用 PatBlt(BLACKNESS) 把目标内存位图刷黑,教师端"打开看"即见纯黑。
// ============================================================================
typedef BOOL (WINAPI* fnLdmBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI* fnLdmStretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
static fnLdmBitBlt g_origLdmBitBlt = NULL;
static fnLdmStretchBlt g_origLdmStretchBlt = NULL;
typedef BOOL (WINAPI* fnGdiFlush)(void);
static fnGdiFlush g_origDesk2GdiFlush = NULL;
volatile LONG g_ldmCaptureReent = 0;            // 重入守卫(PatBlt 不应再进 BitBlt)
volatile bool g_LdmCaptureHooked = false;
volatile bool g_Desk2GdiFlushHooked = false;
volatile LONG g_ldmCaptureLogCount = 0;          // 调试日志节流计数
struct LdmIatPatch { ULONG_PTR* entry; ULONG_PTR orig; };
std::vector<LdmIatPatch> g_LdmIatPatches;        // 被改写的 LibDeskMonitor IAT 槽(卸载还原)
static void VEnsureLibDeskMonitorCaptureHook();  // 幂等装入 LibDeskMonitor 捕获钩
static void VEnsureFrameReplacementHooks();      // 运行中切换开关时补装帧替换相关钩子（定义在 VInstallHooks 之后）

// libTDDesk2 在最终 GdiFlush 后才完成一轮捕获位图提交。记录同线程最近一次
// screen->memory 目标 DC，等原始 flush 返回后再覆盖整张持久位图。
thread_local HDC tlsCaptureBackingDC = NULL;
thread_local bool tlsCapturePending = false;
thread_local int tlsCaptureX = 0;
thread_local int tlsCaptureY = 0;
thread_local int tlsCaptureWidth = 0;
thread_local int tlsCaptureHeight = 0;
static BOOL WINAPI hkDesk2GdiFlush(void);

HMODULE hLibTDMaster;

BOOL hk1 = 0, hk2 = 0, hk3 = 0, hk4 = 0,
hk5 = 0, hk6 = 0, hk7 = 0, hk8 = 0,
hk9 = 0, hk10 = 0, hk11 = 0, hk12 = 0,
hk13 = 0, hk14 = 0, hk15 = 0, hk16 = 0,
hk17 = 0, hk18 = 0, hk19 = 0, hk20 = 0,
hk21 = 0, hk22 = 0, hk23 = 0, hk24 = 0,
hk25 = 0, hk26 = 0, hk27 = 0, hk28 = 0,
hk29 = 0, hk30 = 0, hk31 = 0, hk32 = 0,
hk33 = 0, hk34 = 0, hk35 = 0, hk36 = 0,
hk37 = 0, hk38 = 0, hk39 = 0, hk40 = 0,
hk41 = 0;
BOOL hk42 = 0, hk43 = 0, hk44 = 0, hk45 = 0;

volatile LONG jpegHookAttempted = 0;
volatile LONG jpegHookLogPending = 0;
volatile LONG frameReplacementReported = 0;
volatile LONG frameReplacementInvalidReported = 0;
volatile LONG frameReplacementLogPending = 0;
volatile LONG frameReplacementLogWidth = 0;
volatile LONG frameReplacementLogHeight = 0;
volatile LONG frameReplacementLogStride = 0;
volatile LONG debugLogWriting = 0;
volatile LONG inputHookLogCount = 0;
volatile LONG captureHelperProcessLogCount = 0;
volatile LONG bitBltCallCount = 0;
volatile LONG jpegCallCount = 0;
thread_local bool bitBltHookActive = false;
volatile LONG watchdogRunning = 0;
SRWLOCK handleMsgLock = SRWLOCK_INIT;
WCHAR debugLogPath[MAX_PATH] = { 0 };

struct SendMessageBackContext
{
	HWND senderWindow;
	HWND preferredReceiver;
	std::wstring message;
};

thread_local HWND tlsCommandSender = NULL;

struct CommandContext
{
	HWND senderWindow;
	wchar_t* command;
};

void VDebugLog(const wchar_t* str, ...)
{
	if (InterlockedCompareExchange(&debugLogWriting, 1, 0) != 0)
		return;

	if (debugLogPath[0] == L'\0')
	{
		GetModuleFileNameW(hInst, debugLogPath, MAX_PATH);
		WCHAR* fileName = wcsrchr(debugLogPath, L'\\');
		if (fileName)
			wcscpy_s(fileName + 1, MAX_PATH - (fileName + 1 - debugLogPath), L"DzjsTrainerHooks-debug.log");
		else
			wcscpy_s(debugLogPath, L"DzjsTrainerHooks-debug.log");
	}

	WCHAR message[768] = { 0 };
	va_list args;
	va_start(args, str);
	_vsnwprintf_s(message, _countof(message), _TRUNCATE, str, args);
	va_end(args);

	SYSTEMTIME now;
	GetLocalTime(&now);
	WCHAR line[1024] = { 0 };
	swprintf_s(line, L"[%02u:%02u:%02u.%03u] [pid=%lu tid=%lu] %s\r\n",
		now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
		GetCurrentProcessId(), GetCurrentThreadId(), message);

	CHAR utf8[3072] = { 0 };
	int byteCount = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
	if (byteCount > 1)
	{
		HANDLE file = faCreateFileW
			? faCreateFileW(debugLogPath, FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL)
			: CreateFileW(debugLogPath, FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(file, utf8, byteCount - 1, &written, NULL);
			CloseHandle(file);
		}
	}

	InterlockedExchange(&debugLogWriting, 0);
}

// 统一挂钩入口。名字里的 "Hook" 保持原样：日志格式被回归脚本按关键字抓取。
// 引擎实现见 HookEngine.cpp（MinHook；批量启用 + 排空卸载）。
BOOL VSetHookLogged(LPCWSTR name, PVOID* original, PVOID replacement)
{
	VDebugLog(L"hook begin name=%s target=%p replacement=%p", name, original ? *original : NULL, replacement);
	BOOL result = HookEngine::Install(name, original, replacement) ? TRUE : FALSE;
	VDebugLog(L"hook end name=%s result=%d trampoline=%p", name, result, original ? *original : NULL);
	return result;
}

DWORD WINAPI VLoadBootstrapThread(LPVOID lpThreadParameter)
{
	UNREFERENCED_PARAMETER(lpThreadParameter);
	VDebugLog(L"VLoadBootstrapThread begin module=%p", hInst);
	VLoad();
	VDebugLog(L"VLoadBootstrapThread end");
	return 0;
}

void VUnloadAll() {

	if (loaded)
	{
		VCloseMsgCenter();
		VCloseFuckDrivers();
		{ VWndListGuard guard; jiYuWnds.clear(); }
		{ VWndListGuard guard; jiYuWndCanSize.clear(); }
		runOPWhiteList.clear();
		if (hMenuGb) DestroyMenu(hMenuGb);
		VUnInstallHooks();
		loaded = false;
	}
}
void VLoad()
{
	VDebugLog(L"VLoad begin");
	VParamInit();

	//Get main mod name
	WCHAR mainModName[MAX_PATH];
	GetModuleFileName(NULL, mainModName, MAX_PATH);

	std::wstring path(mainModName);

	int lastQ = path.find_last_of(L'\\');
	std::wstring name = path.substr(lastQ + 1, path.length() - lastQ - 1);
	VDebugLog(L"VLoad host=%s", name.c_str());
	captureHelperForwarding = name == L"StudentMain.exe";

	if (name == L"StudentMain.exe") {

		ULONGLONG dependencyStarted = GetTickCount64();
		for (int attempt = 0; attempt < 150 && !hLibTDMaster; ++attempt) {
			hLibTDMaster = GetModuleHandle(L"LibTDMaster.dll");
			if (!hLibTDMaster)
				Sleep(100);
		}
		VDebugLog(L"VLoad dependency LibTDMaster=%p elapsedMs=%llu", hLibTDMaster,
			GetTickCount64() - dependencyStarted);
		if (hLibTDMaster == NULL) {
			//这不是极域, 混入了什么奇怪的东西？

			WCHAR buf[32];
			swprintf_s(buf, L"hkb:wtf:%d", GetCurrentProcessId());
			VSendMessageBack(buf, hWndMsgCenter);
			VDebugLog(L"VLoad abort reason=LibTDMaster-timeout");
			return;
		}

		//This is target, run virus

		GetModuleFileName(hInst, mainModName, MAX_PATH);
		path = std::wstring(mainModName);
		lastQ = path.find_last_of(L'\\');
		name = path.substr(lastQ + 1, path.length() - lastQ - 1);

		if (name == L"LibTDAjust.dll") { //Current is virus stub dll , load real and alloc
			VLoadRealVirus();
			VRunMain();
		}
		else if (name == L"DzjsTrainerHooks.dll") {//Current is virus main dll
			VRunMain();
		}
	}
	else if (name == L"JiYuTrainer.exe" || name == L"DzjsTrainer.exe") {
		VLoadMainProtect();
	}
	else if (name == L"MasterHelper.exe") {
		//MasterHelper.exe 搞一些事情
		VInstallHooks(VirusModeMaster);
	}
	else if (name == L"ProcHelper64.exe") {
		//ProcHelper64.exe 搞一些事情
		VInstallHooks(VirusModeMaster);
	}
	else if (name == L"DispcapHelper.exe") {
		VRunMain();
		// 主修复:DispcapHelper 是独立进程,真正执行"打开看"的实时编码;此前从未在此装 JPEG 钩。
		// 启动重试线程,待 LibJPEG20 加载后装入三层钩(实现体/IAT/GetProcAddress)。
		VStartJpegHookRetryThread();
	}
	else {
		// 自定义注入宿主（用户通过「进程注入」指定的非极域进程）。
		// 原实现在这里什么都不做：DLL 被加载了，但 VRunMain / VInstallHooks
		// 都没跑到，等于注入无效。这里复用同一套初始化，让 34 项有序表照样安装。
		// 不等待 LibTDMaster、不做 capture-helper 转发、不改极域专属设置。
		g_customHost = true;
		VDebugLog(L"VLoad custom host pid=%lu name=%s", GetCurrentProcessId(), name.c_str());
		VRunMain();
	}

	loaded = true;
	VDebugLog(L"VLoad end loaded=1");
}
void VRunMain() {
	VDebugLog(L"VRunMain begin");
	desktopWindow = GetDesktopWindow();

	GetModuleFileNameW(hInst, mainIniPath, MAX_PATH);
	WCHAR* iniName = wcsrchr(mainIniPath, L'\\');
	if (iniName) {
		const size_t remaining = MAX_PATH - (iniName + 1 - mainIniPath);
		// New builds use DzjsTrainer.ini; keep JiYuTrainer.ini as a legacy fallback.
		wcscpy_s(iniName + 1, remaining, L"DzjsTrainer.ini");
		if (!PathFileExistsW(mainIniPath))
			wcscpy_s(iniName + 1, remaining, L"JiYuTrainer.ini");
	}
	VDebugLog(L"VRunMain settings file=%s", mainIniPath);
	if (PathFileExistsW(mainIniPath))
		VInitSettings();
	if (g_customHost) {
		// 通用宿主不能照搬极域的整段 ini 语义，但**替换开关必须照搬**：
		// 这里原先把 m_EnableModifyVideoStream 硬写成 false，理由是「TRUE 会把
		// VInstallHooks 逼进极域的 minimal 帧替换分支」。那条分支现在已经加了
		// !g_customHost 守卫，开关可以按 ini 生效 —— 效果就是「注入谁，谁的截屏
		// 就被换成那张图」，不再局限于极域那几个进程。
		// 代价是：注入一个第三方程序后，最显眼的效果就是「替换开关是全局的」，
		// 想只对极域生效就在 ini 里关掉 EnableVideoModify 再注入。
		WCHAR customMode[32] = {};
		GetPrivateProfileString(L"CustomInject", L"Mode", L"full", customMode, 32, mainIniPath);
		g_customHookMinimal = StrEqual(customMode, L"minimal") || StrEqual(customMode, L"MINIMAL");
		// allowMonitor 仍然强制 TRUE：假桌面 + DC 拦截那套（hk26/27/33）在通用宿主里
		// 没有 fakeDesktopWindow 可用，装上去只会把宿主自己的 GetDesktopWindow 打空。
		allowMonitor = true;
		whitePreviewOnly = false;
		doNotShowVirusWindow = true;   // 不在用户指定的进程里弹状态窗口
		forceDisableWatchDog = true;   // 没有极域 JPEG 状态可补装，不空转
		if (g_customHookMinimal && g_VideoProps.m_EnableModifyVideoStream)
			VDebugLog(L"VRunMain custom host minimal 模式跳过整个显示域，替换画面在 pid=%lu 里不会生效", GetCurrentProcessId());
		VDebugLog(L"VRunMain custom host mode=%s videoModify=%d (full=34 项表 / minimal=跳过极域专属)",
			g_customHookMinimal ? L"minimal" : L"full", g_VideoProps.m_EnableModifyVideoStream);
	}
	else {
		whitePreviewOnly = !allowMonitor;
	}
	VDebugLog(L"VRunMain settings path=%s allowMonitor=%d whitePreviewOnly=%d allowAllRunOp=%d videoModify=%d",
		mainIniPath, allowMonitor, whitePreviewOnly, allowAllRunOp, g_VideoProps.m_EnableModifyVideoStream);
	// 构建标记：日志里有这一行才说明跑的是本次构建的 DLL。
	// 排查"改了没生效"时先看它，省得在部署问题上兜圈子。
	VDebugLog(L"hooks build=20261114-outctl-fix");
	// 走 minimal 还是全量集，由上面两个标志决定；全量集会接管极域的窗口/进程/网络接口，
	// 把它记下来，出问题时一眼能看出走的是哪条分支。
	VDebugLog(L"hooks mode=%s", (whitePreviewOnly || g_VideoProps.m_EnableModifyVideoStream) ? L"minimal" : L"full");

	VCreateMsgCenter();
	VDebugLog(L"VRunMain message center requested");
	VDebugLog(L"VRunMain end");
}
void VLoadRealVirus() {
	if (_waccess_s(L"LibTDAjust.dll.bak.dll", 0) == 0)
	{
		HMODULE hrealTDAjust = LoadLibrary(L"LibTDAjust.dll.bak.dll");
		if (!hrealTDAjust) {
			MessageBox(0, L"!hrealTDAjust ", L"ERROR!", MB_ICONERROR);
			ExitProcess(0);
		}
		faTDAjustCreateInstance = (fnTDAjustCreateInstance)GetProcAddress(hrealTDAjust, "TDAjustCreateInstance");
		if (!faTDAjustCreateInstance) {
			MessageBox(0, L"!faTDAjustCreateInstance", L"ERROR!", MB_ICONERROR);
			ExitProcess(0);
		}
	}
	else {
		MessageBox(0, L"!LibTDAjust.dll.bak.dll", L"ERROR!", MB_ICONERROR);
		ExitProcess(0);
		
	}
}
void VLoadMainProtect() {

}

DWORD WINAPI VMsgCenterRunThread(LPVOID lpThreadParameter) {
	UNREFERENCED_PARAMETER(lpThreadParameter);
	VDebugLog(L"VMsgCenterRunThread begin");
	uiThreadId = GetCurrentThreadId();

	fakeDesktopWindow = CreateDialog(hInst, MAKEINTRESOURCE(IDD_FAKEDESKTOP), desktopWindow, FakeDesktopWndProc);
	VDebugLog(L"VMsgCenterRunThread fakeDesktopWindow=%p lastError=%lu", fakeDesktopWindow, GetLastError());
	hWndMsgCenter = CreateDialog(hInst, MAKEINTRESOURCE(IDD_MSGCT), desktopWindow, MainWndProc);
	VDebugLog(L"VMsgCenterRunThread hWndMsgCenter=%p lastError=%lu", hWndMsgCenter, GetLastError());

	hListBoxStatus = GetDlgItem(hWndMsgCenter, IDC_STATUS_LIST);

	hThreadInitialize = CreateThread(NULL, 0, VHookInitializeThread, NULL, 0, &initializeThreadId);
	VDebugLog(L"VMsgCenterRunThread initialization thread=%p", hThreadInitialize);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	VDebugLog(L"VMsgCenterRunThread message loop ended");

	hWndMsgCenter = NULL;
	fakeDesktopWindow = NULL;
	uiThreadId = 0;
	return 0;
}

DWORD WINAPI VHookInitializeThread(LPVOID lpThreadParameter) {
	UNREFERENCED_PARAMETER(lpThreadParameter);
	VDebugLog(L"VHookInitializeThread begin");
	VOpenFuckDrivers();
	VDebugLog(L"VHookInitializeThread driver handles opened");
	VInstallHooks(VirusModeHook);
	VDebugLog(L"VHookInitializeThread hooks installed hk28=%d", hk28);
	VSendMessageBack(L"hkb:succ", hWndMsgCenter);  // 通用宿主的回传在 VSendMessageBack 内被抑制
	PostMessage(hWndMsgCenter, WM_HOOK_INITIALIZED, 0, 0);
	if (!g_customHost) {
		HANDLE handshakeThread = CreateThread(NULL, 0, VHandshakeRetryThread, NULL, 0, NULL);
		if (handshakeThread)
			CloseHandle(handshakeThread);
		else
			VDebugLog(L"handshake retry thread create failed error=%lu", GetLastError());
	}
	VDebugLog(L"VHookInitializeThread control ready");
	return 0;
}

DWORD WINAPI VHandshakeRetryThread(LPVOID lpThreadParameter) {
	UNREFERENCED_PARAMETER(lpThreadParameter);
	for (int attempt = 1; attempt <= 8; ++attempt) {
		Sleep(500);
		if (!hWndMsgCenter)
			break;
		VDebugLog(L"handshake retry attempt=%d hwnd=%p", attempt, hWndMsgCenter);
		VSendMessageBack(L"hkb:succ", hWndMsgCenter);
	}
	return 0;
}

DWORD WINAPI VWatchdogThread(LPVOID lpThreadParameter) {
	UNREFERENCED_PARAMETER(lpThreadParameter);
	ULONGLONG started = GetTickCount64();
	VDebugLog(L"watchdog worker begin count=%d loaded=%d allowMonitor=%d jpegHook=%d", wdCount, loaded, allowMonitor, hk28);

	VEnsureJPEGHook();
	VEnsureLibDeskMonitorCaptureHook();   // 重试兜底:确保 LibDeskMonitor "打开看" GDI 捕获钩已装入
	VReportJPEGHookStatus();
	if (wdCount < 32768) wdCount++;
	else wdCount = 0;

	WCHAR message[21];
	swprintf_s(message, L"wcd:%d", wdCount);
	VSendMessageBack(message, hWndMsgCenter);
	if (hWndMsgCenter)
		PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_MAIN_FLASH, 0);

	VDebugLog(L"watchdog worker end count=%d elapsedMs=%llu jpegHook=%d jpegCalls=%ld",
		wdCount, GetTickCount64() - started, hk28, jpegCallCount);
	InterlockedExchange(&watchdogRunning, 0);
	return 0;
}

DWORD WINAPI VSendMessageBackThread(LPVOID lpThreadParameter) {
	SendMessageBackContext* context = static_cast<SendMessageBackContext*>(lpThreadParameter);
	if (!context)
		return 0;

	HWND receiveWindow = NULL;
	if (context->preferredReceiver && IsWindow(context->preferredReceiver))
		receiveWindow = context->preferredReceiver;
	if (!receiveWindow)
		receiveWindow = FindControlWindow();
	if (!receiveWindow) {
		VDebugLog(L"message back dropped command=%s reason=receiver-not-found", context->message.c_str());
		delete context;
		return 0;
	}

	COPYDATASTRUCT copyData = { 0 };
	copyData.lpData = const_cast<wchar_t*>(context->message.c_str());
	copyData.cbData = static_cast<DWORD>(sizeof(WCHAR) * (context->message.length() + 1));
	DWORD_PTR response = 0;
	SetLastError(ERROR_SUCCESS);
	ULONGLONG started = GetTickCount64();
	LRESULT result = SendMessageTimeoutW(receiveWindow, WM_COPYDATA,
		reinterpret_cast<WPARAM>(context->senderWindow), reinterpret_cast<LPARAM>(&copyData),
		SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &response);
	DWORD error = GetLastError();
	VDebugLog(L"message back complete command=%s target=%p result=%lld response=%Iu error=%lu elapsedMs=%llu",
		context->message.c_str(), receiveWindow, static_cast<long long>(result),
		static_cast<size_t>(response), error, GetTickCount64() - started);

	if (hWndMsgCenter)
		PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FLASH_FAST, 0);
	delete context;
	return 0;
}

DWORD WINAPI VHandleMsgThread(LPVOID lpThreadParameter) {
	CommandContext* context = static_cast<CommandContext*>(lpThreadParameter);
	if (!context || !context->command) {
		if (context) {
			delete[] context->command;
			delete context;
		}
		return 0;
	}

	ULONGLONG started = GetTickCount64();
	VDebugLog(L"message command worker begin command=%s sender=%p", context->command, context->senderWindow);
	tlsCommandSender = context->senderWindow;
	AcquireSRWLockExclusive(&handleMsgLock);
	VHandleMsg(context->command);
	ReleaseSRWLockExclusive(&handleMsgLock);
	tlsCommandSender = NULL;
	VDebugLog(L"message command worker end command=%s elapsedMs=%llu", context->command, GetTickCount64() - started);
	delete[] context->command;
	delete context;
	return 0;
}

void VParamInit() {
	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
	currentPid = GetCurrentProcessId();

	//创建广播的右键菜单

	HBITMAP hIconExit = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_EXIT));
	HBITMAP hIconHelp = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_HELP));

	hMenuGb = CreatePopupMenu();
	AppendMenu(hMenuGb, MF_STRING , IDM_HELP_GB, L"DzjsTrainer 添加的菜单");
	AppendMenu(hMenuGb, MF_STRING, IDM_HELP_GB2, L"DzjsTrainer 帮助");
	AppendMenu(hMenuGb, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenuGb, MF_STRING, IDM_FULL, L"广播窗口全屏");
	AppendMenu(hMenuGb, MF_STRING, IDM_TOPMOST, L"广播窗口置顶");
	AppendMenu(hMenuGb, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenuGb, MF_STRING, IDM_CLOSE_GB, L"关闭广播窗口");

	SetMenuItemBitmaps(hMenuGb, IDM_CLOSE_GB, MF_BITMAP, hIconExit, hIconExit);
	SetMenuItemBitmaps(hMenuGb, IDM_HELP_GB2, MF_BITMAP, hIconHelp, hIconHelp);
}
void VInitSettings()
{
	WCHAR w[32];
	GetPrivateProfileString(L"JTSettings", L"AutoForceKill", L"FALSE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) forceKill = true;
	else forceKill = false;
	// 2026-11-14 起所有开关默认「只记录不拦截」：键缺失时一律按放行处理，
	// 用户必须在界面上主动打开某一项，对应的钩子才会真的动手。
	GetPrivateProfileString(L"JTSettings", L"AllowAllRunOp", L"TRUE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) allowAllRunOp = true;
	else allowAllRunOp = false;
	GetPrivateProfileString(L"JTSettings", L"BandAllRunOp", L"TRUE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) bandAllRunOp = false;
	else bandAllRunOp = true;
	GetPrivateProfileString(L"JTSettings", L"ProhibitKillProcess", L"FALSE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) ProhibitKillProcess = false;
	else ProhibitKillProcess = true;
	GetPrivateProfileString(L"JTSettings", L"ProhibitCloseWindow", L"FALSE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) ProhibitCloseWindow = false;
	else ProhibitCloseWindow = true;
	GetPrivateProfileString(L"JTSettings", L"BlockWindowOps", L"FALSE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) blockWindowOps = true;
	else blockWindowOps = false;
	GetPrivateProfileString(L"JTSettings", L"BlockDriverAccess", L"FALSE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) blockDriverAccess = true;
	else blockDriverAccess = false;
	
	GetPrivateProfileString(L"JTSettings", L"DoNotShowVirusWindow", L"TRUE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) doNotShowVirusWindow = false;
	else doNotShowVirusWindow = true;

	GetPrivateProfileString(L"JTSettings", L"ForceDisableWatchDog", L"FALSE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) forceDisableWatchDog = false;
	else forceDisableWatchDog = true;

	GetPrivateProfileString(L"JTSettings", L"AllowGbTop", L"FALSE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) allowGbTop = true;
	else allowGbTop = false;

	// AllowMonitor: 仅在键存在时更新，避免 hk:reset 把已禁用的监视静默重新允许。
	// 键缺失时保留当前 allowMonitor，不翻转为默认的 TRUE。
	GetPrivateProfileString(L"JTSettings", L"AllowMonitor", L"", w, 32, mainIniPath);
	if (w[0] != L'\0') {
		if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) allowMonitor = true;
		else allowMonitor = false;
	}
	// whitePreviewOnly 必须跟着 allowMonitor 走：装不装假桌面 + DC 拦截钩子看的是
	// whitePreviewOnly，原先只有 VRunMain 和 setmonitor 两处会更新它，
	// 于是 hk:reset 之后 allowMonitor 变了、whitePreviewOnly 还是旧值，两者打架。
	whitePreviewOnly = !allowMonitor;

	// EnableVideoModify: 同理，键缺失时保留当前视频流保护状态，不翻转为 FALSE。
	GetPrivateProfileString(L"JTSettings", L"EnableVideoModify", L"", w, 32, mainIniPath);
	if (w[0] != L'\0') {
		if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) {
			g_VideoProps.m_Width = GetSystemMetrics(SM_CXSCREEN);
			g_VideoProps.m_Height = GetSystemMetrics(SM_CYSCREEN);
			g_VideoProps.m_EnableModifyVideoStream = true;
		} else {
			g_VideoProps.m_EnableModifyVideoStream = false;
		}
	}
	// VideoModifyImage: 自定义替换图片(BMP/PNG/JPG)。设置后视频流(含"打开看")会被替换为该图。
	// 注意：本程序 ini 为 UTF-8(无 BOM)。GetPrivateProfileStringW 会按系统默认 ANSI(GBK) 解读
	// 文件内容，导致其中文路径被误读为乱码(典型如 "娴嬭瘯")，进而找不到图片。
	// 方案 B：改用 A 版读出值的【原始字节】(无 BOM 时 A 版原样返回文件字节)，再优先按 UTF-8
	// 解析；若严格 UTF-8 解析失败(说明 ini 本身是 GBK 编码)，则回退 CP_ACP(GBK)，
	// 从而兼容任意编码的中文路径。
	{
		char iniPathA[MAX_PATH] = { 0 };
		WideCharToMultiByte(CP_ACP, 0, mainIniPath, -1, iniPathA, MAX_PATH, NULL, NULL);
		char imgPathA[MAX_PATH] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyImage", "", imgPathA, MAX_PATH, iniPathA);
		WCHAR imgPath[MAX_PATH] = { 0 };
		if (imgPathA[0] != '\0') {
			int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, imgPathA, -1, imgPath, MAX_PATH);
			if (n == 0) {
				// UTF-8 严格解析失败 -> 按系统 ANSI(GBK) 代码页回退解析
				MultiByteToWideChar(CP_ACP, 0, imgPathA, -1, imgPath, MAX_PATH);
			}
		}
		if (imgPath[0] != L'\0') VLoadReplacementImage(imgPath);
		else VUnloadReplacementImage();
		char videoPathA[MAX_PATH] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyVideo", "", videoPathA, MAX_PATH, iniPathA);
		WCHAR videoPath[MAX_PATH] = { 0 };
		if (videoPathA[0] != '\0') {
			int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, videoPathA, -1, videoPath, MAX_PATH);
			if (n == 0) MultiByteToWideChar(CP_ACP, 0, videoPathA, -1, videoPath, MAX_PATH);
		}
		char loopValue[16] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyLoop", "TRUE", loopValue, 16, iniPathA);
		char modeValue[16] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyMode", "video", modeValue, 16, iniPathA);
		bool useImage = _stricmp(modeValue, "image") == 0 || _stricmp(modeValue, "photo") == 0;
		g_VideoImagePath = imgPath; g_VideoPath = videoPath;
		g_VideoLoop = _stricmp(loopValue, "FALSE") != 0 && strcmp(loopValue, "0") != 0;
		g_VideoMode = useImage ? L"image" : L"video";
		VStopReplacementVideo();
		if (!useImage && videoPath[0] != L'\0') VStartReplacementVideo(videoPath, g_VideoLoop);
	}
	// 重载配置时丢弃外部替换画面。走 ExternalSurface 而不是直接 DeleteDC：
	// 捕获线程此刻可能正握着这个 DC，直接删会读到已释放的 GDI 对象。
	g_surface.Set(0, 0, NULL);
	GetPrivateProfileString(L"JTSettings", L"AllowControl", L"TRUE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) allowControl = true;
	else allowControl = false;
}
void VLaterInit()
{
	if (PathFileExists(mainIniPath)) 
	{
		VInitSettings();
		VLoadOpWhiteList();
	}
}
void VCreateMsgCenter() {
	hThreadMain = CreateThread(NULL, 0, VMsgCenterRunThread, NULL, 0, NULL);
	VDebugLog(L"VCreateMsgCenter thread=%p", hThreadMain);
}
void VCloseMsgCenter() {
	if (hWndMsgCenter) {
		if (GetCurrentThreadId() == uiThreadId)
			DestroyWindow(hWndMsgCenter);
		else
			PostMessage(hWndMsgCenter, WM_CLOSE, 0, 0);
	}
	if (hThreadInitialize) {
		if (GetCurrentThreadId() != initializeThreadId)
			WaitForSingleObject(hThreadInitialize, 2000);
		CloseHandle(hThreadInitialize);
		hThreadInitialize = NULL;
		initializeThreadId = 0;
	}
	if (hThreadMain) {
		if (GetCurrentThreadId() != uiThreadId)
			WaitForSingleObject(hThreadMain, 2000);
		CloseHandle(hThreadMain);
		hThreadMain = NULL;
	}
}
void VHandleMsg(LPWSTR buff) {
	wstring act(buff);
	vector<wstring> arr;
	SplitString(act, arr, L":");
	if (arr.size() >= 2) {
		if (arr[0] == L"hw")  VHookWindow(arr[1].c_str());
		else if (arr[0] == L"hwf") VHookFWindow(arr[1].c_str());
		else if (arr[0] == L"sh") {
			HWND hWnd = (HWND)_wtol(arr[1].c_str());
			if (IsWindow(hWnd)) mainWindow = hWnd;
			VOutPutStatus(L"[S] %s", buff);
		}
		else if (arr[0] == L"ss") VBoom();
		else if (arr[0] == L"ss2") {
			VDebugLog(L"ss2 terminate requested");
			if (faTerminateProcess)
				faTerminateProcess(GetCurrentProcess(), 0);
			ExitProcess(0);
		}
		else if (arr[0] == L"hk") {
			VOutPutStatus(L"[V] %s", buff);
			if (arr[1] == L"ckstat") {
				// Acknowledge control before optional version probing or driver calls.
				// Those calls can block on slower VMs and must not prevent the handshake.
				VDebugLog(L"ckstat begin version=%d", jiYuVersion);
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
				VDebugLog(L"ckstat handshake queued");
				if (jiYuVersion == jiYuVersionsAuto) {
					VDebugLog(L"ckstat version probe begin");
					VGetStudentainVersion();
					VDebugLog(L"ckstat version probe end version=%d", jiYuVersion);
				}
				VDebugLog(L"ckstat keyboard unlock begin");
				VUnHookKeyBoard();
				VDebugLog(L"ckstat keyboard unlock end");
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_MAIN, NULL);
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FLASH, 0);
				VDebugLog(L"ckstat end");
			}
			else if (arr[1] == L"ckend") VManualQuit();
			else if (arr[1] == L"path" && arr.size() >= 4) {
				wcscpy_s(mainFullPath, arr[2].c_str());
				wcscat_s(mainFullPath, L":");
				wcscat_s(mainFullPath, arr[3].c_str());
			}
			else if (arr[1] == L"inipath" && arr.size() >= 4) {
				wcscpy_s(mainIniPath, arr[2].c_str());
				wcscat_s(mainIniPath, L":");
				wcscat_s(mainIniPath, arr[3].c_str());
				VLaterInit();
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FLASH, 0);
			}
			else if (arr[1] == L"reset") {
				VInitSettings();
				// VInitSettings 只把 ini 重新读进全局变量，不会装钩子。而"要装哪些钩子"
				// 恰恰由这些变量决定（whitePreviewOnly / m_EnableModifyVideoStream），
				// 所以重读之后必须补装，否则运行中改的设置只有标志位生效、钩子没跟上。
				// 注意：这里调的是 VEnsureFrameReplacementHooks()，不是完整的 VInstallHooks ——
				// 后者会重新解析函数指针、枚举全进程模块、起注入线程，只能调用一次。
				VEnsureFrameReplacementHooks();
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FAIL_FLASH, 0);
			}
			else if (arr[1] == L"setmonitor" && arr.size() >= 3) {
				// 显式设置 allowMonitor，直接驱动 live 全局，不被 ini 默认值覆盖
				allowMonitor = (arr[2] == L"1" || arr[2] == L"TRUE" || arr[2] == L"true");
				whitePreviewOnly = !allowMonitor;
				// whitePreviewOnly 决定是否装假桌面 + DC 拦截钩子(GetDesktopWindow /
				// GetWindowDC / CreateDCW / DwmEnableComposition)，而 VInstallHooks 原先
				// 只在钩子加载时按当时的值装一次。运行中把开关改成"禁止查看屏幕"时
				// 标志位变了、钩子却没装，教师依然能看到画面 —— 必须在这里补装。
				// 注意：这里调的是 VEnsureFrameReplacementHooks()，不是完整的 VInstallHooks ——
				// 后者会重新解析函数指针、枚举全进程模块、起注入线程，只能调用一次。
				VEnsureFrameReplacementHooks();
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideo" && arr.size() >= 3) {
				// 显式设置视频流保护，直接驱动 live 全局
				bool en = (arr[2] == L"1" || arr[2] == L"TRUE" || arr[2] == L"true");
				g_VideoProps.m_Width = GetSystemMetrics(SM_CXSCREEN);
				g_VideoProps.m_Height = GetSystemMetrics(SM_CYSCREEN);
				g_VideoProps.m_EnableModifyVideoStream = en;
				if (!en) VStopReplacementVideo();
				// 同理：BitBlt / JPEG / DispFilter 钩子也是按加载时的标志装的，
				// 运行中开启视频流保护同样必须补装。
				VEnsureFrameReplacementHooks();
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideoimage" && arr.size() >= 3) {
				g_VideoImagePath = arr[2];
				for (size_t i = 3; i < arr.size(); ++i) { g_VideoImagePath += L":"; g_VideoImagePath += arr[i]; }
				VStopReplacementVideo();
				VLoadReplacementImage(g_VideoImagePath.c_str());
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideofile" && arr.size() >= 3) {
				g_VideoPath = arr[2];
				for (size_t i = 3; i < arr.size(); ++i) { g_VideoPath += L":"; g_VideoPath += arr[i]; }
				VSendMessageBack(VStartReplacementVideo(g_VideoPath.c_str(), g_VideoLoop) ? L"hkb:succ" : L"hkb:fail", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideoloop" && arr.size() >= 3) {
				g_VideoLoop = arr[2] == L"1" || arr[2] == L"TRUE" || arr[2] == L"true";
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideomode" && arr.size() >= 3) {
				g_VideoMode = (arr[2] == L"image" || arr[2] == L"photo") ? L"image" : L"video";
				if (g_VideoMode == L"image") { VStopReplacementVideo(); if (!g_VideoImagePath.empty()) VLoadReplacementImage(g_VideoImagePath.c_str()); }
				else if (!g_VideoPath.empty()) VStartReplacementVideo(g_VideoPath.c_str(), g_VideoLoop);
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"fkfull" && arr.size() >= 3) {
				fakeFull = arr[2] == L"true";
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_FAKEFULL, NULL);
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FLASH, 0);
			}
		}
		else if (arr[0] == L"ukt") {
			VOutPutStatus(L"[T] %s", buff);
			if (UnLockLocalInput) { UnLockLocalInput(); VSendMessageBack(L"ukt:succ", hWndMsgCenter); }
			else VSendMessageBack(L"ukt:fail", hWndMsgCenter);
		}
		else if (arr[0] == L"test") {
			VOutPutStatus(L"[T] %s", buff);
			VShowOpConfirmDialog(L"test", L"test");
			VSendMessageBack(L"vback:test virus", hWndMsgCenter);
		}
		else if (arr[0] == L"test2") {
			if (arr[1] == L"f") {
				VOutPutStatus(L"test2 > f");
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_MAIN_FLASH, NULL);
			}
			//WCHAR str[300]; swprintf_s(str, L"vback:hk29 %s  faavcodec_encode_video : 0x%08X", hk29 ? L"TRUE" : L"FALSE", faavcodec_encode_video);
			//VSendMessageBack(str, hWndMsgCenter);
		}
	}
}

void VAppendStatusLine(const std::wstring& status) {
	statusLines.push_back(status);
	if (statusLines.size() > 100)
		statusLines.erase(statusLines.begin(), statusLines.begin() + (statusLines.size() - 100));
	if (hWndMsgCenter)
		InvalidateRect(hWndMsgCenter, NULL, FALSE);
}

void VOutPutStatus(const wchar_t* str, ...) {
	time_t time_log = time(NULL);
	struct tm tm_log;
	localtime_s(&tm_log, &time_log);
	va_list arg;
	va_start(arg, str);
	wstring format1 = FormatString(L"[%02d:%02d:%02d] %s", tm_log.tm_hour, tm_log.tm_min, tm_log.tm_sec, str);
	wstring out = FormatString(format1.c_str(), arg);
	va_end(arg);

	if (!hWndMsgCenter)
		return;
	if (GetCurrentThreadId() == uiThreadId) {
		VAppendStatusLine(out);
		return;
	}

	wchar_t* status = new (std::nothrow) wchar_t[out.length() + 1];
	if (!status)
		return;
	wcscpy_s(status, out.length() + 1, out.c_str());
	if (!PostMessageW(hWndMsgCenter, WM_STATUS_APPEND, 0, reinterpret_cast<LPARAM>(status)))
		delete[] status;
}
bool VWindowTextIsGb(const wchar_t* text) {

	return StringHlp::StrContainsW(text, L"广播", nullptr) || StringHlp::StrContainsW(text, L"演示", nullptr)
		|| StringHlp::StrContainsW(text, L"共享", nullptr) || StringHlp::StrEqualW(text, L"屏幕演播室窗口") 
		|| StringHlp::StrContainsW(text, L"共享屏幕", nullptr);
}
void VHookFWindow(const wchar_t* hWndStr) {
	HWND hWnd = (HWND)_wtol(hWndStr);
	if (IsWindow(hWnd)) {
		//GuangBo window fix
		if (hWnd != jiYuGBWnd) {
			WCHAR text[50];
			GetWindowText(hWnd, text, 50);
			if (VWindowTextIsGb(text)) {
				VFixGuangBoWindow(hWnd);
				jiYuGBWnd = hWnd;
			}
		}
		if (!VIsInIllegalWindows(hWnd)) {
			{ VWndListGuard guard; jiYuWnds.push_back(hWnd); }
		}
	}
}
void VHookWindow(const wchar_t* hWndStr) {
	HWND hWnd = (HWND)_wtol(hWndStr);
	if (IsWindow(hWnd)) {
		//GuangBo window fix
		if (hWnd != jiYuGBWnd) {
			WCHAR text[50];
			GetWindowText(hWnd, text, 50);
			if (VWindowTextIsGb(text)) {
				VFixGuangBoWindow(hWnd);
				jiYuGBWnd = hWnd;
			}
		}
		//
		if (!VIsInIllegalWindows(hWnd)) {
			{ VWndListGuard guard; jiYuWnds.push_back(hWnd); }
			//{ VWndListGuard guard; jiYuWndCanSize.push_back(hWnd); }
		}
	}
}
void VFixGuangBoWindow(HWND hWnd)
{
	if (gbWindow != hWnd) {
		gbWindow = hWnd;
		VOutPutStatus(L"[H] Guang Bo Window : %d", hWnd);
	}
	//WNDPROC 接管
	WNDPROC oldWndProc = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
	if (oldWndProc != (WNDPROC)JiYuWndProc) {
		jiYuWndProc = (WNDPROC)oldWndProc;
		SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)JiYuWndProc);
		VOutPutStatus(L"[J] Hooked hWnd %d (0x%08x) WNDPROC", hWnd, hWnd);
		SendMessage(hWnd, WM_SHOWWINDOW, TRUE, FALSE);
	}
	LONG style = GetWindowLong(hWnd, GWL_STYLE);
	if ((style & WS_OVERLAPPEDWINDOW) != WS_OVERLAPPEDWINDOW)
		style |= WS_OVERLAPPEDWINDOW;
	if ((style & WS_SYSMENU) != WS_SYSMENU)
		style |= WS_SYSMENU;
	SetWindowLong(hWnd, GWL_STYLE, style);
	//增加菜单
	GetSystemMenu(gbWindow, TRUE);
	HMENU hMenu = GetSystemMenu(gbWindow, FALSE);
	AppendMenu(hMenu, MF_SEPARATOR, MF_SEPARATOR, L"");
	AppendMenu(hMenu, MF_STRING, IDM_FULL, L"广播窗口全屏");
	AppendMenu(hMenu, MF_STRING, IDM_TOPMOST, L"广播窗口置顶");

	LONG oldLong = GetWindowLong(gbWindow, GWL_EXSTYLE);
	if ((oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST) {
		CheckMenuItem(hMenu, IDM_TOPMOST, MF_CHECKED);
		CheckMenuItem(hMenuGb, IDM_TOPMOST, MF_CHECKED);
		gbCurrentIsTop = true;
	}
	else {
		CheckMenuItem(hMenu, IDM_TOPMOST, MF_UNCHECKED);
		CheckMenuItem(hMenuGb, IDM_TOPMOST, MF_UNCHECKED);
		gbCurrentIsTop = false;
	}
	if (gbFullManual) {
		CheckMenuItem(hMenu, IDM_FULL, MF_CHECKED);
		CheckMenuItem(hMenuGb, IDM_FULL, MF_CHECKED);
	}
	else {
		CheckMenuItem(hMenu, IDM_FULL, MF_UNCHECKED);
		CheckMenuItem(hMenuGb, IDM_FULL, MF_UNCHECKED);
	}

	//类属性
	LONG oldWndClsLong = GetClassLong(gbWindow, GCL_STYLE);
	oldWndClsLong ^= CS_NOCLOSE;
	SetClassLong(gbWindow, GCL_STYLE, oldWndClsLong);

	isGbFounded = true;
}
bool VIsInIllegalWindows(HWND hWnd) {
	VWndListGuard guard;
	list<HWND>::iterator testiterator;
	for (testiterator = jiYuWnds.begin(); testiterator != jiYuWnds.end(); testiterator++)
	{
		if ((*testiterator) == hWnd)
		{
			// 全量集里多个窗口钩子对这些窗口直接 return TRUE（吞掉极域的操作）。
			// 限流记录：卡死时日志能指出是哪个窗口、被拦了多少次。
			static volatile LONG s_illegalLogCount = 0;
			LONG n = InterlockedIncrement(&s_illegalLogCount);
			if (g_hookTrace && (n <= 200 || (n % 500 == 0)))
				VDebugLog(L"VIsInIllegalWindows hit hwnd=%p count=%ld", hWnd, n);
			return true;
		}
	}
	return false;
}
bool VIsInIllegalCanSizeWindows(HWND hWnd) {
	VWndListGuard guard;
	list<HWND>::iterator testiterator;
	for (testiterator = jiYuWndCanSize.begin(); testiterator != jiYuWndCanSize.end(); testiterator++)
	{
		if ((*testiterator) == hWnd)
			return true;
	}
	return false;
}
bool VIsInIllegalWindowMessage(UINT msg) {
	return msg == WM_CLOSE || msg == WM_DESTROY;
}
void VBoom() {

	PostQuitMessage(0);
	ExitProcess(0);

	/*
	CHAR*P = 0;
	*P = 0;
	*/
}
void VSendMessageBack(LPCWSTR buff, HWND hDlg) {
	// 通用宿主（用户指定进程）不参与主程序的 hk: 状态协议：不回传、不握手，
	// 也不会因为找不到主窗口误判失联而弹 IDD_OUTCTL。下行命令仍可正常接收。
	if (g_customHost) {
		VDebugLog(L"message back suppressed custom-host command=%s", buff ? buff : L"(null)");
		return;
	}
	HWND receiveWindow = NULL;
	if (tlsCommandSender && IsWindow(tlsCommandSender))
		receiveWindow = tlsCommandSender;
	if (!receiveWindow)
		receiveWindow = FindControlWindow();
	if (!receiveWindow) {
		// 启动竞态：主窗口可能比 hooks 注入晚几十到几百毫秒才建好。
		// 原来第一次 FindWindow 失败就立刻弹 IDD_OUTCTL 模态框，既卡住
		// 消息线程，又会误触发 -r3 重启主程序。先做有限重试（最多 1 秒）。
		int retries = 0;
		for (; retries < 5 && !receiveWindow; ++retries) {
			Sleep(200);
			receiveWindow = FindControlWindow();
		}
		if (receiveWindow)
			VDebugLog(L"message back recovered after %d control-window retries", retries);
	}
	if (receiveWindow) {
		SendMessageBackContext* context = new (std::nothrow) SendMessageBackContext();
		if (!context) {
			VDebugLog(L"message back enqueue failed command=%s reason=allocation-failed", buff ? buff : L"(null)");
			return;
		}
		context->senderWindow = hDlg;
		context->preferredReceiver = tlsCommandSender;
		context->message = buff ? buff : L"";
		VDebugLog(L"message back queue requested command=%s receiver=%p preferred=%p", context->message.c_str(), receiveWindow, context->preferredReceiver);
		HANDLE sendThread = CreateThread(NULL, 0, VSendMessageBackThread, context, 0, NULL);
		if (sendThread) {
			CloseHandle(sendThread);
		}
		else {
			VDebugLog(L"message back enqueue failed command=%s error=%lu", context->message.c_str(), GetLastError());
			delete context;
		}
	}
	else if(!outlineEndJiy) {
		VDebugLog(L"message back control window not found class=%s display=%s legacy=%s command=%s",
			kDzjsControlWindowClass, kDzjsDisplayWindowTitle, kLegacyControlWindowTitle,
			buff ? buff : L"(null)");
		PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_MAIN_OUT, 0);
		PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FAIL, 0);
		outlineEndJiy = true;
		if (forceKill) {
			if (_waccess_s(mainFullPath, 0) == 0)
				ShellExecute(hWndMsgCenter, L"open", mainFullPath, L"-r2", NULL, SW_NORMAL);
			VBoom();
		}
		else {
			VShowOutOfControlDlg();
			if (hWndOutOfControlConformRs == IDC_KILL) {
				PostQuitMessage(0);
				ExitProcess(0);
			}
			else if (hWndOutOfControlConformRs != IDCANCEL)
			{
				outlineEndJiy = false;
				if (_waccess_s(mainFullPath, 0) == 0)
					ShellExecute(hWndMsgCenter, L"open", mainFullPath, L"-r3", NULL, SW_NORMAL);
			}
		}
	}
}
void VManualQuit()
{
	VCloseFuckDrivers();
	VCloseMsgCenter();
	{ VWndListGuard guard; jiYuWnds.clear(); }
	loaded = false;
}

bool VIsWindowGbOrHp(HWND hWnd) {
	WCHAR text[50];
	GetWindowText(hWnd, text, 50);
	return VWindowTextIsGb(text) || StrEqual(text, L"BlackScreen Window");
}
bool VIsWindowGb(HWND hWnd) {
	WCHAR text[50]; GetWindowText(hWnd, text, 50);
	return VWindowTextIsGb(text);
}
bool VIsWindowHp(HWND hWnd) {
	WCHAR text[50];
	GetWindowText(hWnd, text, 50);
	return StrEqual(text, L"BlackScreen Window");
}
void VSwitchLockState(bool l) {
	if (isLocked != l) {
		isLocked = l;
		PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_LOCKED, NULL);
		WCHAR str[32]; swprintf_s(str, L"hkb:jyk:%s", l ? L"1" : L"0");
		VSendMessageBack(str, hWndMsgCenter);
		if (jiYuGBDeskRdWnd) {
			RECT rc, rcTool; GetWindowRect(jiYuGBDeskRdWnd, &rc);
			if (jiYuGBToolWnd) {
				GetClientRect(jiYuGBToolWnd, &rcTool);
				jiYuGBToolHeight = rcTool.bottom - rcTool.top;
			}
			raMoveWindow(jiYuGBDeskRdWnd, 0, isLocked ? 0 : jiYuGBToolHeight,
				rc.right - rc.left, rc.bottom - rc.top - (isLocked ? 0 : jiYuGBToolHeight), TRUE);
		}
	}
}
void VSwitchLockState(HWND hWnd, bool l) {
	if (l) {
		if (!isLocked && VIsWindowGbOrHp(hWnd)) {
			if (VIsWindowGb(hWnd))
				isGbFounded = true;
			VSwitchLockState(l);
		}
	}
	else {
		if (isLocked && VIsWindowGbOrHp(hWnd)) {
			if (VIsWindowGb(hWnd))
				isGbFounded = false;
			VSwitchLockState(l);
		}
	}
}
void VUnHookKeyBoard() {
	HMODULE hTDMaster = GetModuleHandle(L"LibTDMaster.dll");
	if (hTDMaster) {
		fnDoneHook DoneHook = (fnDoneHook)GetProcAddress(hTDMaster, "DoneHook");
		UnLockLocalInput = (fnUnLockLocalInput)GetProcAddress(hTDMaster, "UnLockLocalInput");
		if (UnLockLocalInput) {
			UnLockLocalInput();
			VOutPutStatus(L"[V] Unlocked Local Input. ");
		}
		else if (DoneHook) {
			DoneHook();
			VOutPutStatus(L"[V] Forece call DoneHook .");
		}
	}
}
void VGetStudentainVersion() 
{
	//Get main mod name
	WCHAR mainModName[MAX_PATH];
	GetModuleFileName(NULL, mainModName, MAX_PATH);
	WCHAR mainModVersion[64];

	VOutPutStatus(L"[V] VGetExeInfo %s ", mainModName);
	//获取极域版本
	if (VGetExeInfo(mainModName, L"ProductVersion", mainModVersion, 64)) {

		VOutPutStatus(L"[V] JiYu Version : %s", mainModVersion);

		if (wcscmp(mainModVersion, L"5.01 Baseline") == 0) {
			jiYuVersion = jiYuVersions::jiYuVersions40;
			VOutPutStatus(L"[V] 当前是：极域 V4 2010 专业版");
		}
		else if (wcscmp(mainModVersion, L"2.07 CMPC") == 0) {
			jiYuVersion = jiYuVersions::jiYuVersions402016HH;
			VOutPutStatus(L"[V] 当前是：极域 V6.0 2016 豪华版");
		}

	}else VOutPutStatus(L"[V] VGetExeInfo err !");

}
BOOL VGetExeInfo(LPWSTR strFilePath, LPCWSTR InfoItem, LPWSTR str, int maxCount)
{
	/*
	CompanyName
	FileDescription
	FileVersion
	InternalName
	LegalCopyright
	OriginalFilename
	ProductName
	ProductVersion
	Comments
	LegalTrademarks
	PrivateBuild
	SpecialBuild
	*/

	TCHAR   szResult[256];
	TCHAR   szGetName[256];
	LPWSTR  lpVersion = { 0 };        // String pointer to Item text
	DWORD   dwVerInfoSize;    // Size of version information block
	DWORD   dwVerHnd = 0;        // An 'ignored' parameter, always '0'
	UINT    uVersionLen;
	BOOL    bRetCode;

	dwVerInfoSize = GetFileVersionInfoSize(strFilePath, &dwVerHnd);
	if (dwVerInfoSize) {
		LPSTR   lpstrVffInfo;
		HANDLE  hMem;
		hMem = GlobalAlloc(GMEM_MOVEABLE, dwVerInfoSize);
		lpstrVffInfo = (LPSTR)GlobalLock(hMem);
		GetFileVersionInfo(strFilePath, dwVerHnd, dwVerInfoSize, lpstrVffInfo);
		lstrcpy(szGetName, L"\\VarFileInfo\\Translation");
		uVersionLen = 0;
		lpVersion = NULL;
		bRetCode = VerQueryValue((LPVOID)lpstrVffInfo,
			szGetName,
			(void **)&lpVersion,
			(UINT *)&uVersionLen);
		if (bRetCode && uVersionLen && lpVersion)
			wsprintf(szResult, L"%04x%04x", (WORD)(*((DWORD *)lpVersion)),
			(WORD)(*((DWORD *)lpVersion) >> 16));
		else lstrcpy(szResult, L"041904b0");
		wsprintf(szGetName, L"\\StringFileInfo\\%s\\", szResult);
		lstrcat(szGetName, InfoItem);
		uVersionLen = 0;
		lpVersion = NULL;
		bRetCode = VerQueryValue((LPVOID)lpstrVffInfo,
			szGetName,
			(void **)&lpVersion,
			(UINT *)&uVersionLen);
		if (bRetCode && uVersionLen && lpVersion) {
			if (str) {
				wcscpy_s(str, maxCount, lpVersion);
				return TRUE;
			}
		}
	}
	return FALSE;
}

INT_PTR CALLBACK VShowOpConfirmWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = 0;
	switch (message)
	{
	case WM_INITDIALOG: {

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));

		SetDlgItemText(hDlg, IDC_EDIT_OP_PATH, currOpCfPath);
		SetDlgItemText(hDlg, IDC_EDIT_OP_PARARM, currOpCfPararm);

		SetWindowLongPtr(GetDlgItem(hDlg, IDNO), GWLP_USERDATA, (LPARAM)30);
		SetTimer(hDlg, TIMER_COUNTDOWN_TICK, 1000, NULL);

		lResult = TRUE;
		break;
	}
	case WM_TIMER: {
		if (wParam == TIMER_COUNTDOWN_TICK) {
			int w = (int)GetWindowLongPtr(GetDlgItem(hDlg, IDNO), GWLP_USERDATA);
			if (w > 0) {
				WCHAR str[18]; swprintf_s(str, L"拒绝 (%d)", w); 
				SetWindowLongPtr(GetDlgItem(hDlg, IDNO), GWLP_USERDATA, (LPARAM)(w - 1));
				SetDlgItemText(hDlg, IDNO, str);
			}
			else SendMessage(hDlg, WM_COMMAND, IDNO, NULL);
		}
		break;
	}
	case WM_COMMAND: {
		if (wParam == IDYES || wParam == IDNO || wParam == IDCANCEL) { 
			hWndOpConformRs = wParam; 
			lResult = wParam;
			DestroyWindow(hDlg);
		} 
		if (wParam == IDC_CHECK_BANDALL) {
			bandAllRunOp = IsDlgButtonChecked(hDlg, IDC_CHECK_BANDALL);
		}
		if (wParam == IDC_CHECK_ALOW) {
			allowNextRunOp = IsDlgButtonChecked(hDlg, IDC_CHECK_ALOW);
		}
		break;
	}
	case WM_DESTROY: {
		PostQuitMessage(0);
		break;
	}
	}
	return lResult;
}
INT_PTR CALLBACK VShowOutOfControlWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = 0;
	switch (message)
	{
	case WM_INITDIALOG: {

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));

		SetDlgItemText(hDlg, IDC_EDIT_OP_PATH, currOpCfPath);
		SetDlgItemText(hDlg, IDC_EDIT_OP_PARARM, currOpCfPararm);

		SetWindowLongPtr(GetDlgItem(hDlg, IDC_RESTART), GWLP_USERDATA, (LPARAM)7);
		SetTimer(hDlg, TIMER_COUNTDOWN_TICK, 1000, NULL);

		lResult = TRUE;
		break;
	}
	case WM_TIMER: {
		if (wParam == TIMER_COUNTDOWN_TICK) {
			int w = (int)GetWindowLongPtr(GetDlgItem(hDlg, IDC_RESTART), GWLP_USERDATA);
			if (w > 0) {
				WCHAR str[18]; swprintf_s(str, L"重启主程序 (%d)", w);
				SetWindowLongPtr(GetDlgItem(hDlg, IDC_RESTART), GWLP_USERDATA, (LPARAM)(w - 1));
				SetDlgItemText(hDlg, IDC_RESTART, str);
			}
			else SendMessage(hDlg, WM_COMMAND, IDC_RESTART, NULL);
		}
		break;
	}
	case WM_COMMAND: {
		if (wParam == IDC_RESTART || wParam == IDNO || wParam == IDC_KILL || wParam == IDCANCEL) {
			// 「不理会」按钮是 IDNO，而 VSendMessageBack 只把 IDCANCEL 当作
			// "不动作"，原来点它照样会 -r3 重启主程序。统一映射成 IDCANCEL。
			hWndOutOfControlConformRs = (wParam == IDNO) ? IDCANCEL : wParam;
			lResult = hWndOutOfControlConformRs;
			DestroyWindow(hDlg);
		}
		break;
	}
	case WM_CLOSE: {
		// 本过程不走 DefDlgProc，原来点 X / Esc 完全没反应。
		hWndOutOfControlConformRs = IDCANCEL;
		DestroyWindow(hDlg);
		break;
	}
	case WM_DESTROY: {
		PostQuitMessage(0);
		break;
	}
	}
	return lResult;
}
std::wstring VGetExtension(std::wstring path)
{
	if (path == L"")
		return nullptr;

	size_t length = path.size();
	size_t num = length;
	while (--num >= 0)
	{
		wchar_t c = (path)[num];
		if (c == L'.')
		{
			if (num != length - 1)
			{
				return std::wstring(path.substr(num, length - num));
			}
			return nullptr;
		}
		else if (c == DirectorySeparatorChar || c == AltDirectorySeparatorChar || c == VolumeSeparatorChar)
		{
			break;
		}
	}
	return nullptr;
}
BOOL VShowOutOfControlDlg() {

	hWndOutOfControlConformRs = 0;

	HWND hWnd = CreateDialog(hInst, MAKEINTRESOURCE(IDD_OUTCTL), NULL, VShowOutOfControlWndProc);
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return hWndOutOfControlConformRs == IDYES;
}
BOOL VShowOpConfirmDialog(LPCWSTR file, LPCWSTR pararm) {
	if (file) wcsncpy_s(currOpCfPath, file, MAX_PATH); else wcscpy_s(currOpCfPath, L"");
	if (pararm) wcsncpy_s(currOpCfPararm, pararm, MAX_PATH); else wcscpy_s(currOpCfPararm, L"");

	allowNextRunOp = false;
	hWndOpConformRs = 0;

	HWND hWnd = CreateDialog(hInst, MAKEINTRESOURCE(IDD_OPASK), NULL, VShowOpConfirmWndProc);
	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (allowNextRunOp) {

		LPCWSTR exePath = NULL;
		if (!StrEmepty(file) && _waccess_s(file, 0) == 0) exePath = file;
		else if (!StrEmepty(pararm) && _waccess_s(pararm, 0) == 0) exePath = pararm;
		else if (!StrEmepty(file)) exePath = file;
		else if (!StrEmepty(pararm)) exePath = pararm;

		VAddOpToWhiteList(exePath);
	}

	return hWndOpConformRs == IDYES;
}	

LRESULT CALLBACK VCBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
	return TRUE;
}

void VLoadOpWhiteList() {
	WCHAR mainWhiteListPath[MAX_PATH];
	wcscpy_s(mainWhiteListPath, mainFullPath);
	PathRemoveFileSpec(mainWhiteListPath);
	wcscat_s(mainWhiteListPath, L"\\JiYuKillerOpWhiteList.txt");

	if (PathFileExists(mainWhiteListPath)) {
		FILE * fp;
		int linenum;
		WCHAR * p, buf[1024];

	   _wfopen_s(&fp, mainWhiteListPath, L"r");
		if (fp == NULL) {
			return;
		}
		
		for (linenum = 1; fgetws(buf, sizeof(buf), fp) != NULL; ++linenum) {
			if ((p = wcschr(buf, L'\n')) == NULL) {
				p = buf + wcslen(buf);
			}
			if (p > buf && p[-1] == L'\r') {
				--p;
			}
			*p = L'\0';
			for (p = buf; *p != L'\0' && isspace((int)*p); ++p) {
				;
			}
			if (*p == L'\0' || *p == L'#') {
				continue;
			}

			VAddOpToWhiteList(p);
		}
		fclose(fp);
	}
}
void VAddOpToWhiteList(const wchar_t* cmd) {
	wstring wcmd = wstring(cmd);
	wcmd = StringHlp::StrLoW(wcmd.data());
	if (!VIsOpInWhiteList(wcmd.c_str()))
		runOPWhiteList.push_back(wcmd);
}
bool VIsOpInWhiteList(const wchar_t* cmd) {
	wstring wcmd = wstring(cmd);
	wcmd = StringHlp::StrLoW(wcmd.data());
	std::list<std::wstring>::iterator iter;
	for (iter = runOPWhiteList.begin(); iter != runOPWhiteList.end(); iter++)
	{
		if ((*iter) == wcmd)
			return true;
	}
	return false;
}

// ===================== 视频流自定义图片替换 =====================
// JPEG 钩覆盖缩略预览；实时 H.264 "打开看"由下方 backing bitmap GDI 绘制路径覆盖。
// 两条路径共享同一次图片解码结果。

// 解码 BMP/PNG/JPG 为 RGB24(R,G,B) 缓冲,行无对齐(stride=w*3)。返回缓冲用 delete[] 释放。
// 用系统自带 GDI+(自带 BMP/PNG/JPG 解码器,无需 WIC/COM)。
static bool GDIPlusDecodeToRGB24(LPCWSTR path, BYTE*& outBuf, int& outW, int& outH)
{
	outBuf = NULL; outW = 0; outH = 0;
	Gdiplus::GdiplusStartupInput gsi;
	ULONG_PTR token = 0;
	if (Gdiplus::GdiplusStartup(&token, &gsi, NULL) != Gdiplus::Ok) return false;
	bool ok = false;
	Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path);
	if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
		UINT w = bmp->GetWidth();
		UINT h = bmp->GetHeight();
		if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
			Gdiplus::BitmapData bd;
			Gdiplus::Rect r(0, 0, w, h);
			if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &bd) == Gdiplus::Ok) {
				// GDI+ 24bpp 为 BGR 顺序,转为 RGB24 并去掉行对齐(stride 可能更大)。
				BYTE* rgb = new (std::nothrow) BYTE[(size_t)w * 3 * h];
				if (rgb) {
					for (UINT y = 0; y < h; y++) {
						const BYTE* s = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
						BYTE* d = rgb + (size_t)y * w * 3;
					for (UINT x = 0; x < w; x++) {
						d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
						s += 3; d += 3;
					}
					}
					outBuf = rgb; outW = (int)w; outH = (int)h;
					ok = true;
				}
				bmp->UnlockBits(&bd);
			}
		}
		delete bmp;
	}
	Gdiplus::GdiplusShutdown(token);
	return ok;
}

static bool VPublishReplacementFrame(BYTE* rgbData, BYTE* dibData, int width, int height, int dibStride)
{
	// 所有权一并移交 FrameStore：失败路径由它负责释放，调用点不必再写两个 delete 分支
	// （原来是"自己判参数、自己 delete、自己加锁、自己处理 pending"，四件事混在一处）。
	return g_frames.Publish(rgbData, dibData, width, height, dibStride);
}

void VUnloadReplacementImage()
{
	g_frames.Clear();
}

bool VLoadReplacementImage(LPCWSTR path)
{
	if (!path || path[0] == L'\0') { VUnloadReplacementImage(); return false; }
	BYTE* buf = NULL; int w = 0, h = 0;
	if (!GDIPlusDecodeToRGB24(path, buf, w, h)) {
		VDebugLog(L"VideoModifyImage decode failed path=%s", path);
		return false;
	}
	const int dibStride = ReplaceCodec::DibStride(w);
	BYTE* dibBuf = new (std::nothrow) BYTE[(size_t)dibStride * h];
	if (!dibBuf) {
		delete[] buf;
		VDebugLog(L"VideoModifyImage DIB allocation failed path=%s w=%d h=%d", path, w, h);
		return false;
	}
	// 先清零：Rgb24ToDib 只写每行 width*3 字节，行末 padding 留在零上。
	ZeroMemory(dibBuf, (size_t)dibStride * h);
	if (ReplaceCodec::Rgb24ToDib(buf, w, h, dibBuf, dibStride) == 0) {
		delete[] buf; delete[] dibBuf;
		VDebugLog(L"VideoModifyImage DIB conversion rejected path=%s w=%d h=%d stride=%d", path, w, h, dibStride);
		return false;
	}
	if (!VPublishReplacementFrame(buf, dibBuf, w, h, dibStride))
		return false;
	VDebugLog(L"VideoModifyImage loaded path=%s w=%d h=%d dibStride=%d", path, w, h, dibStride);
	return true;
}

template <typename T> static void VSafeRelease(T*& value) { if (value) { value->Release(); value = NULL; } }

static bool VConvertVideoSample(IMFSample* sample, int width, int height,
	BYTE*& rgbData, BYTE*& dibData, int& dibStride)
{
	rgbData = NULL; dibData = NULL; dibStride = 0;
	if (!sample || width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
	IMFMediaBuffer* buffer = NULL; IMF2DBuffer* buffer2D = NULL;
	BYTE* scanline0 = NULL; BYTE* flatData = NULL; LONG pitch = 0;
	bool locked2D = false, bufferLocked = false, ok = false;
	DWORD bufferCount = 0;
	HRESULT hr = sample->GetBufferCount(&bufferCount);
	if (SUCCEEDED(hr) && bufferCount == 1) hr = sample->GetBufferByIndex(0, &buffer);
	else if (SUCCEEDED(hr)) hr = sample->ConvertToContiguousBuffer(&buffer);
	if (SUCCEEDED(hr) && buffer) hr = buffer->QueryInterface(IID_PPV_ARGS(&buffer2D));
	if (SUCCEEDED(hr) && buffer2D) { hr = buffer2D->Lock2D(&scanline0, &pitch); locked2D = SUCCEEDED(hr); }
	if (!locked2D && buffer) {
		DWORD maxLength = 0, currentLength = 0; hr = buffer->Lock(&flatData, &maxLength, &currentLength);
		bufferLocked = SUCCEEDED(hr);
		if (SUCCEEDED(hr) && currentLength >= (DWORD)((size_t)width * 4 * height)) { scanline0 = flatData + (size_t)(height - 1) * width * 4; pitch = -(LONG)(width * 4); }
	}
	if (scanline0 && pitch != 0) {
		dibStride = ReplaceCodec::DibStride(width);
		rgbData = new (std::nothrow) BYTE[(size_t)width * 3 * height];
		dibData = new (std::nothrow) BYTE[(size_t)dibStride * height];
		if (rgbData && dibData) {
			ZeroMemory(dibData, (size_t)dibStride * height);
			for (int y = 0; y < height; ++y) {
				const BYTE* src = scanline0 + (ptrdiff_t)(height - 1 - y) * pitch;
				BYTE* rgb = rgbData + (size_t)y * width * 3; BYTE* dib = dibData + (size_t)y * dibStride;
				for (int x = 0; x < width; ++x) { dib[0]=src[0]; dib[1]=src[1]; dib[2]=src[2]; rgb[0]=src[2]; rgb[1]=src[1]; rgb[2]=src[0]; src+=4; rgb+=3; dib+=3; }
			}
			ok = true;
		}
	}
	if (!ok) { delete[] rgbData; delete[] dibData; rgbData=NULL; dibData=NULL; dibStride=0; }
	if (locked2D) buffer2D->Unlock2D(); if (bufferLocked) buffer->Unlock();
	VSafeRelease(buffer2D); VSafeRelease(buffer); return ok;
}

static bool VGetVideoFrameSize(IMFSourceReader* reader, int& width, int& height)
{
	width=0; height=0; IMFMediaType* type=NULL; UINT32 w=0,h=0;
	HRESULT hr=reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type);
	if(SUCCEEDED(hr)) hr=MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&w,&h); VSafeRelease(type);
	if(FAILED(hr)||w==0||h==0||w>16384||h>16384) return false; width=(int)w; height=(int)h; return true;
}

static DWORD WINAPI VReplacementVideoThread(LPVOID parameter)
{
	ReplacementVideoContext* context = static_cast<ReplacementVideoContext*>(parameter); if (!context) return ERROR_INVALID_PARAMETER;
	HRESULT coHr=CoInitializeEx(NULL,COINIT_MULTITHREADED), hr=S_OK; bool comInit=SUCCEEDED(coHr), mfInit=false;
	IMFAttributes* attrs=NULL; IMFSourceReader* reader=NULL; IMFMediaType* outType=NULL; int width=0,height=0;
	do {
		if(FAILED(coHr)){hr=coHr;break;} hr=MFStartup(MF_VERSION,MFSTARTUP_FULL); if(FAILED(hr))break; mfInit=true;
		hr=MFCreateAttributes(&attrs,1); if(FAILED(hr))break; attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
		hr=MFCreateSourceReaderFromURL(context->path.c_str(),attrs,&reader); if(FAILED(hr))break;
		reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE); reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
		hr=MFCreateMediaType(&outType); if(FAILED(hr))break; outType->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); outType->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32); outType->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
		hr=reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,NULL,outType); if(FAILED(hr)||!VGetVideoFrameSize(reader,width,height)) break;
		while(WaitForSingleObject(context->stopEvent,0)==WAIT_TIMEOUT){
			DWORD stream=0,flags=0; LONGLONG ts=0; IMFSample* sample=NULL; hr=reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&stream,&flags,&ts,&sample); if(FAILED(hr)){VSafeRelease(sample);break;}
			if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) VGetVideoFrameSize(reader,width,height);
			if(flags&MF_SOURCE_READERF_ENDOFSTREAM){VSafeRelease(sample); if(!context->loop)break; PROPVARIANT pos; PropVariantInit(&pos); pos.vt=VT_I8; pos.hVal.QuadPart=0; hr=reader->SetCurrentPosition(GUID_NULL,pos); PropVariantClear(&pos); if(FAILED(hr))break; continue;}
			if(sample){ BYTE* rgb=NULL; BYTE* dib=NULL; int stride=0; if(VConvertVideoSample(sample,width,height,rgb,dib,stride)&&VPublishReplacementFrame(rgb,dib,width,height,stride)){g_VideoFrameActive=true; for(int i=0;i<2;++i)InterlockedExchange(&g_DispDXGIFullRefreshPending[i],1);} VSafeRelease(sample); }
		}
	} while(false);
	VSafeRelease(outType); VSafeRelease(reader); VSafeRelease(attrs); if(mfInit)MFShutdown(); if(comInit)CoUninitialize(); g_VideoFrameActive=false; delete context; return SUCCEEDED(hr)?0:(DWORD)hr;
}

void VStopReplacementVideo()
{
	if(!g_VideoCsInit)return; EnterCriticalSection(&g_VideoCs); if(g_VideoThread){if(g_VideoStopEvent)SetEvent(g_VideoStopEvent); WaitForSingleObject(g_VideoThread,INFINITE); CloseHandle(g_VideoThread); g_VideoThread=NULL;} if(g_VideoStopEvent){CloseHandle(g_VideoStopEvent);g_VideoStopEvent=NULL;} g_VideoFrameActive=false; LeaveCriticalSection(&g_VideoCs);
}

bool VStartReplacementVideo(LPCWSTR path,bool loop)
{
	VStopReplacementVideo(); if(!path||!*path||!PathFileExistsW(path))return false; if(!g_VideoCsInit){InitializeCriticalSection(&g_VideoCs);g_VideoCsInit=true;}
	ReplacementVideoContext* c=new(std::nothrow)ReplacementVideoContext(); if(!c)return false; c->path=path;c->loop=loop;c->stopEvent=CreateEventW(NULL,TRUE,FALSE,NULL); if(!c->stopEvent){delete c;return false;}
	EnterCriticalSection(&g_VideoCs);g_VideoStopEvent=c->stopEvent;g_VideoThread=CreateThread(NULL,0,VReplacementVideoThread,c,0,NULL); if(!g_VideoThread){CloseHandle(g_VideoStopEvent);g_VideoStopEvent=NULL;delete c;LeaveCriticalSection(&g_VideoCs);return false;} LeaveCriticalSection(&g_VideoCs); return true;
}

// 线程安全读取替换图缓冲(引用计数防止在读期间被释放)
static bool VAcquireImage(BYTE*& data, BYTE*& dibData, int& w, int& h, int& dibStride)
{
	ReplaceState::Frame frame;
	if (!g_frames.Acquire(frame))
		return false;
	data = frame.rgb; dibData = frame.dib;
	w = frame.width; h = frame.height; dibStride = frame.dibStride;
	return true;
}
static void VReleaseImage()
{
	g_frames.Release();
}

// JPEG 预览输入缓冲按 bottom-up 行序解释。将顶向下的替换图按反向源行写入，
// 避免编码后的缩略图上下颠倒；H.264 backing bitmap 使用独立的顶向下 DIB 路径。
// 行序翻转与 RGB→BGR 两件事都由 ReplaceCodec::BlitScaled 的参数表达。
static void OverlayImage(BYTE* dst, int dw, int dh, int dstride, const BYTE* src, int sw, int sh)
{
	ReplaceCodec::BlitParams params;
	params.dstWidth = dw;
	params.dstHeight = dh;
	params.dstStride = dstride;
	params.srcWidth = sw;
	params.srcHeight = sh;
	params.swapRB = true;
	params.flipVertical = true;
	ReplaceCodec::BlitScaled(dst, src, params);
}

// IAT hook:改写当前进程导入表中 LibJPEG20.dll 的 EncodeToJPEGBuffer 条目,指向我们的钩子。
// 绕开 Mhook 对实现体 prologue 的拆解(本机 result=0),且调用方走 IAT 故必命中。
static bool VPatchModuleIATEncode(HMODULE hMod)
{
	if (!hMod) return false;
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
	DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!impRva) return false;
	PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impRva);
	for (; imp->Name; ++imp) {
		LPCSTR dllName = (LPCSTR)((BYTE*)hMod + imp->Name);
		if (lstrcmpiA(dllName, "LibJPEG20.dll") != 0) continue;
		PIMAGE_THUNK_DATA oft = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->OriginalFirstThunk);
		PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->FirstThunk);
		if (!oft || !ft) continue;
		for (; oft->u1.AddressOfData && ft->u1.Function; ++oft, ++ft) {
			if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
			PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + oft->u1.AddressOfData);
			if (lstrcmpiA((LPCSTR)ibn->Name, "EncodeToJPEGBuffer") != 0) continue;
			if (ft->u1.Function == (ULONG_PTR)(void*)hkEncodeToJPEGBuffer) continue; // 已补丁,跳过(防重复记录)
			if (!faEncodeToJPEGBuffer)
				faEncodeToJPEGBuffer = (fnEncodeToJPEGBuffer)(ULONG_PTR)ft->u1.Function; // 保存原始(仅 impl 钩未设置时)
			DWORD oldProt = 0;
			MEMORY_BASIC_INFORMATION mbi;
			VirtualQuery(&ft->u1.Function, &mbi, sizeof(mbi));
			if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
				ULONG_PTR* slot = &ft->u1.Function;
				ULONG_PTR orig = *slot;
				*slot = (ULONG_PTR)(void*)hkEncodeToJPEGBuffer;
				VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), slot, sizeof(ULONG_PTR));
				JpegIatPatch rec; rec.entry = slot; rec.orig = orig;
				g_JpegIatPatches.push_back(rec);
				g_JpegIATHooked = true;
				VDebugLog(L"JPEG IAT hooked at %p orig=%p", (void*)slot, (void*)orig);
				return true;
			}
			return false;
		}
	}
	return false;
}

// 同上,针对 EncodeToJPEGBufferI422(部分版本"打开看"可能走 I422 编码路径)。
static bool VPatchModuleIATEncodeI422(HMODULE hMod)
{
	if (!hMod) return false;
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
	DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!impRva) return false;
	PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impRva);
	for (; imp->Name; ++imp) {
		LPCSTR dllName = (LPCSTR)((BYTE*)hMod + imp->Name);
		if (lstrcmpiA(dllName, "LibJPEG20.dll") != 0) continue;
		PIMAGE_THUNK_DATA oft = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->OriginalFirstThunk);
		PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->FirstThunk);
		if (!oft || !ft) continue;
		for (; oft->u1.AddressOfData && ft->u1.Function; ++oft, ++ft) {
			if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
			PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + oft->u1.AddressOfData);
			if (lstrcmpiA((LPCSTR)ibn->Name, "EncodeToJPEGBufferI422") != 0) continue;
			if (ft->u1.Function == (ULONG_PTR)(void*)hkEncodeToJPEGBufferI422) continue;
			if (!faEncodeToJPEGBufferI422)
				faEncodeToJPEGBufferI422 = (fnEncodeToJPEGBufferI422)(ULONG_PTR)ft->u1.Function;
			DWORD oldProt = 0;
			MEMORY_BASIC_INFORMATION mbi;
			VirtualQuery(&ft->u1.Function, &mbi, sizeof(mbi));
			if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
				ULONG_PTR* slot = &ft->u1.Function;
				ULONG_PTR orig = *slot;
				*slot = (ULONG_PTR)(void*)hkEncodeToJPEGBufferI422;
				VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), slot, sizeof(ULONG_PTR));
				JpegIatPatch rec; rec.entry = slot; rec.orig = orig;
				g_JpegI422IatPatches.push_back(rec);
				g_JpegI422Hooked = true;
				VDebugLog(L"JPEG I422 IAT hooked at %p orig=%p", (void*)slot, (void*)orig);
				return true;
			}
			return false;
		}
	}
	return false;
}

// 拦截运行时对 LibJPEG20 编码导出的 GetProcAddress 解析(仅在实现体 inline 钩失败时启用,降低侵入性)。
static FARPROC WINAPI hkGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	FARPROC r = g_pfGetProcAddress(hModule, lpProcName);
	if (r && g_hLibJPEG20 && hModule == g_hLibJPEG20 && lpProcName) {
		if ((ULONG_PTR)lpProcName < 0x10000) {
			USHORT ord = (USHORT)(ULONG_PTR)lpProcName;
			if (ord == 4 && g_JpegImplHooked) return (FARPROC)hkEncodeToJPEGBuffer;
			if (ord == 5 && g_JpegI422ImplHooked) return (FARPROC)hkEncodeToJPEGBufferI422;
		}
		else {
			if (lstrcmpiA(lpProcName, "EncodeToJPEGBuffer") == 0 && g_JpegImplHooked)
				return (FARPROC)hkEncodeToJPEGBuffer;
			if (lstrcmpiA(lpProcName, "EncodeToJPEGBufferI422") == 0 && g_JpegI422ImplHooked)
				return (FARPROC)hkEncodeToJPEGBufferI422;
		}
	}
	return r;
}

// DispFilter.dll ordinal 1 导出:DispDXGIBitBlt(RECT* pRect, BYTE* lpbyBits)
// 钩子:原始函数把屏幕 RGBA 填入 lpbyBits 后,把整块清零(RGBA,尺寸=(right-left)*(bottom-top)*4)。
static BOOL __cdecl hkDispDXGIBitBlt(void* pRect, BYTE* lpbyBits)
{
	HookEngine::DetourScope detourScope;   // 卸载时等这个计数归零才释放 trampoline
	BOOL r = g_origDispDXGIBitBlt(pRect, lpbyBits);
	// 诊断:记录 ordinal 1 是否真的被调用、RECT 维度与返回,确认桌面背景是否走这条路。
	static volatile LONG s_cnt = 0;
	LONG n = InterlockedIncrement(&s_cnt);
	if (n <= 40 || (n % 2000 == 0)) {
		RECT* rc = (RECT*)pRect;
		LONG w = (rc && pRect) ? rc->right - rc->left : -1;
		LONG h = (rc && pRect) ? rc->bottom - rc->top : -1;
		VDebugLog(L"DispDXGIBitBlt enter r=%d w=%d h=%d lpbyBits=%p", (int)r, w, h, lpbyBits);
	}
	if (r && lpbyBits && pRect) {
		RECT* rc = (RECT*)pRect;
		LONG w = rc->right - rc->left;
		LONG h = rc->bottom - rc->top;
		if (w > 0 && h > 0 && w < 8192 && h < 8192) {
			// 不透明黑:B=G=R=0, A=0xFF。按 DWORD 填充,覆盖 RGBA/BGRA 两种布局
			// (三色字节恒为 0,alpha 强制不透明),避免消费端按 alpha 合成时透出真实桌面。
			ReplaceCodec::ApplyOpaqueBlackRgba((DWORD*)lpbyBits, static_cast<size_t>(w) * static_cast<size_t>(h));
			VDebugLog(L"DispDXGIBitBlt buffer blacked w=%d h=%d", w, h);
		}
	}
	return r;
}

static bool VGetDispDXGIOutputSize(int outputIndex, LONG* width, LONG* height)
{
	if (!width || !height) return false;
	*width = 0;
	*height = 0;

	// libTDDesk2::sub_1000FF30 uses the physical desktop resolution from the
	// screen DC. Match that first so the forced dirty rect covers its backing DIB.
	HDC screen = GetDC(NULL);
	if (screen) {
		*width = GetDeviceCaps(screen, DESKTOPHORZRES);
		*height = GetDeviceCaps(screen, DESKTOPVERTRES);
		ReleaseDC(NULL, screen);
		if (*width > 0 && *height > 0) return true;
	}

	int activeOutput = 0;
	for (DWORD deviceIndex = 0;; ++deviceIndex) {
		DISPLAY_DEVICEW display;
		ZeroMemory(&display, sizeof(display));
		display.cb = sizeof(display);
		if (!EnumDisplayDevicesW(NULL, deviceIndex, &display, 0)) break;
		if (!(display.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
		if (activeOutput++ != outputIndex) continue;

		DEVMODEW mode;
		ZeroMemory(&mode, sizeof(mode));
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
			*width = (LONG)mode.dmPelsWidth;
			*height = (LONG)mode.dmPelsHeight;
			if (*width > 0 && *height > 0) return true;
		}
		break;
	}

	DEVMODEW primaryMode;
	ZeroMemory(&primaryMode, sizeof(primaryMode));
	primaryMode.dmSize = sizeof(primaryMode);
	if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &primaryMode)) {
		*width = (LONG)primaryMode.dmPelsWidth;
		*height = (LONG)primaryMode.dmPelsHeight;
		if (*width > 0 && *height > 0) return true;
	}

	*width = GetSystemMetrics(SM_CXSCREEN);
	*height = GetSystemMetrics(SM_CYSCREEN);
	return *width > 0 && *height > 0;
}

static DWORD VFillDispDXGIFullRefresh(DispDXGIScreenUpdate* update, LONG width, LONG height)
{
	if (!update || width <= 0 || height <= 0) return 0;
	SetRect(&update->rects[0], 0, 0, width, height);
	update->count = 1;
	return 1;
}

// 教师端使用增量帧。首次打开或捕获停顿后重新打开时，强制把整屏列为脏区，
// 让后续捕获一次覆盖整张持久位图，而不是沿用教师端缓存的旧桌面背景。
static BOOL __cdecl hkDispDXGIGetScreenUpdate(int outputIndex, DispDXGIScreenUpdate* update)
{
	HookEngine::DetourScope detourScope;
	BOOL r = g_origDispDXGIGetScreenUpdate(outputIndex, update);
	static volatile LONG s_cnt = 0;
	LONG n = InterlockedIncrement(&s_cnt);
	if (n <= 10 || (n % 2000 == 0))
		VDebugLog(L"DispDXGIGetScreenUpdate enter #%d r=%d index=%d update=%p count=%u",
		          (int)n, (int)r, outputIndex, update, update ? update->count : 0);

	if (r && update && outputIndex >= 0 && outputIndex < 2) {
		DWORD now = GetTickCount();
		LONG previous = InterlockedExchange(&g_DispDXGILastUpdateTick[outputIndex], (LONG)now);
		if (previous == 0 || (DWORD)(now - (DWORD)previous) > 1500)
			InterlockedExchange(&g_DispDXGIFullRefreshPending[outputIndex], 1);

		if (InterlockedCompareExchange(&g_DispDXGIFullRefreshPending[outputIndex], 0, 1) == 1) {
			LONG width = 0, height = 0;
			if (VGetDispDXGIOutputSize(outputIndex, &width, &height)) {
				DWORD rectCount = VFillDispDXGIFullRefresh(update, width, height);
				VDebugLog(L"DispDXGI full refresh injected index=%d width=%d height=%d rects=%u",
				          outputIndex, width, height, rectCount);
			}
			else {
				InterlockedExchange(&g_DispDXGIFullRefreshPending[outputIndex], 1);
				VDebugLog(L"DispDXGI full refresh deferred: output size unavailable index=%d", outputIndex);
			}
		}
	}
	return r;
}

// 安装 DXGI 屏幕捕获钩(覆盖"打开看"实时画面)。
// DispFilter.dll 仅按序号导出,故用 GetProcAddress(hDisp, MAKEINTRESOURCE(1)) 取原始地址再 Mhook。
// 仅当 DispFilter.dll 已加载时才尝试——DispFilter 由 libTDDesk2 在"打开看"开始时
// 动态 LoadLibraryW,故必须在它加载后(甚至加载瞬间)才装得上。幂等:已装则直接返回。
static void VEnsureDispDXGIHook()
{
	if (g_Unloading || (g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked)) return;
	if (InterlockedCompareExchange(&g_DispDXGIInstallLock, 1, 0) != 0)
		return;

	HMODULE hDisp = GetModuleHandleW(L"DispFilter.dll");
	if (!hDisp) {
		VDebugLog(L"DispDXGI hook: DispFilter.dll not loaded yet, retry later");
		InterlockedExchange(&g_DispDXGIInstallLock, 0);
		return;
	}
	if (!g_DispDXGIHooked) {
		if (g_origDispDXGIBitBlt == NULL)
			g_origDispDXGIBitBlt = (fnDispDXGIBitBlt)GetProcAddress(hDisp, (LPCSTR)MAKEINTRESOURCE(1));
		if (!g_origDispDXGIBitBlt) {
			VDebugLog(L"DispDXGI hook: ordinal 1 export missing");
			InterlockedExchange(&g_DispDXGIInstallLock, 0);
			return;
		}
		if (VSetHookLogged(L"DispDXGIBitBlt", (PVOID*)&g_origDispDXGIBitBlt, (PVOID)hkDispDXGIBitBlt)) {
			g_DispDXGIHooked = true;
			VDebugLog(L"DispDXGI screen-capture hook installed (open-to-view blacked)");
		}
	}

	if (!g_DispDXGIGetScreenUpdateHooked) {
		if (g_origDispDXGIGetScreenUpdate == NULL)
			g_origDispDXGIGetScreenUpdate = (fnDispDXGIGetScreenUpdate)GetProcAddress(hDisp, (LPCSTR)MAKEINTRESOURCE(4));
		if (g_origDispDXGIGetScreenUpdate &&
			VSetHookLogged(L"DispDXGIGetScreenUpdate", (PVOID*)&g_origDispDXGIGetScreenUpdate, (PVOID)hkDispDXGIGetScreenUpdate)) {
			g_DispDXGIGetScreenUpdateHooked = true;
			VDebugLog(L"DispDXGI GetScreenUpdate hook installed (full-refresh dirty rects)");
		}
	}

	InterlockedExchange(&g_DispDXGIInstallLock, 0);
}

// ---- LoadLibrary 钩:捕获 DispFilter.dll 加载瞬间,立即装入 DispDXGI 钩 ----
static bool VIsDispFilterName(LPCWSTR name)
{
	if (!name) return false;
	for (const wchar_t* p = name; *p; ++p) {
		if ((*p == L'D' || *p == L'd') && _wcsnicmp(p, L"DispFilter", 10) == 0)
			return true;
	}
	return false;
}

// 在 loader-lock 之外安装钩(避免 Mhook 在 LoadLibrary 内死锁):由 LoadLibrary 钩起一次性线程。
static DWORD WINAPI VDispDXGIInstallNowThread(LPVOID)
{
	VEnsureDispDXGIHook();
	return 0;
}

static HMODULE WINAPI hkLoadLibraryW(LPCWSTR lpLibFileName)
{
	HMODULE h = g_pfLoadLibraryW(lpLibFileName);
	if (h && !(g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked) && VIsDispFilterName(lpLibFileName)) {
		HANDLE ht = CreateThread(NULL, 0, VDispDXGIInstallNowThread, NULL, 0, NULL);
		if (ht) CloseHandle(ht);
	}
	return h;
}

static HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE h = g_pfLoadLibraryExW(lpLibFileName, hFile, dwFlags);
	if (h && !(g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked) && VIsDispFilterName(lpLibFileName)) {
		HANDLE ht = CreateThread(NULL, 0, VDispDXGIInstallNowThread, NULL, 0, NULL);
		if (ht) CloseHandle(ht);
	}
	return h;
}

// ---- 无限重试:覆盖"打开看"在任意时刻(甚至数小时后)才开始、DispFilter 才加载的场景 ----
static DWORD WINAPI VDispDXGIHookRetryThread(LPVOID)
{
	for (int i = 0; i < 18000 && !g_Unloading &&
		!(g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked); ++i) {   // 约 10 小时,每 2 秒一轮
		VEnsureDispDXGIHook();
		Sleep(2000);
	}
	if (g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked)
		VDebugLog(L"DispDXGI hook retry finished (installed)");
	else if (g_Unloading)
		VDebugLog(L"DispDXGI hook retry stopped (unloading)");
	else
		VDebugLog(L"DispDXGI hook retry exhausted (DispFilter never loaded)");
	return 0;
}

void VStartDispDXGIHookRetryThread()
{
	if (InterlockedCompareExchange(&g_DispDXGIRetryStarted, 1, 0) != 0)
		return;
	HANDLE h = CreateThread(NULL, 0, VDispDXGIHookRetryThread, NULL, 0, NULL);
	if (h)
		CloseHandle(h);
	else
		InterlockedExchange(&g_DispDXGIRetryStarted, 0);
}

void VEnsureJPEGHook()
{
	// DispcapHelper 进程:DXGI 屏幕捕获钩("打开看"实时画面)。
	// 必须放在 LibJPEG20 早退之前——DispcapHelper 可能根本不加载 LibJPEG20。
	if (!(g_DispDXGIHooked && g_DispDXGIGetScreenUpdateHooked))
		VEnsureDispDXGIHook();

	// 解析 LibJPEG20(可能晚加载,故每次调用都尝试)
	if (!g_hLibJPEG20) {
		g_hLibJPEG20 = GetModuleHandleW(L"LibJPEG20.dll");
		if (!g_hLibJPEG20)
			g_hLibJPEG20 = LoadLibraryW(L"LibJPEG20.dll");
	}
	if (!g_hLibJPEG20) {
		VDebugLog(L"JPEG hook: LibJPEG20 not loaded yet, retry later");
		return;
	}

	// 解析原始导出地址
	if (!faEncodeToJPEGBuffer)
		faEncodeToJPEGBuffer = (fnEncodeToJPEGBuffer)GetProcAddress(g_hLibJPEG20, "EncodeToJPEGBuffer");
	if (!faEncodeToJPEGBufferI422)
		faEncodeToJPEGBufferI422 = (fnEncodeToJPEGBufferI422)GetProcAddress(g_hLibJPEG20, "EncodeToJPEGBufferI422");

	if (faEncodeToJPEGBuffer && !g_JpegImplHooked) {
		if (VSetHookLogged(L"EncodeToJPEGBuffer", (PVOID*)&faEncodeToJPEGBuffer, (PVOID)hkEncodeToJPEGBuffer))
			g_JpegImplHooked = true;
	}
	if (faEncodeToJPEGBufferI422 && !g_JpegI422ImplHooked) {
		if (VSetHookLogged(L"EncodeToJPEGBufferI422", (PVOID*)&faEncodeToJPEGBufferI422, (PVOID)hkEncodeToJPEGBufferI422))
			g_JpegI422ImplHooked = true;
	}

	// 这一段枚举本进程所有模块、逐个改它们的 IAT。单个模块的补丁是幂等的
	// （VPatchModuleIATEncode 内有"已补丁则跳过"判断），但**枚举本身**必须只做一次：
	// Module32Next 给的是快照，若枚举期间某个模块被卸载（极域进程会反复加载/卸载
	// LibDeskMonitor、DispFilter 等），就会对已释放的内存做 VirtualQuery/VirtualProtect
	// —— 直接崩掉目标进程。加载时跑一次就够，之后靠 GetProcAddress 钩兜住新加载的模块。
	if (!g_JpegIATHooked && !g_JpegI422Hooked) {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snap != INVALID_HANDLE_VALUE) {
			MODULEENTRY32 me; me.dwSize = sizeof(me);
			if (Module32First(snap, &me)) {
				do {
					VPatchModuleIATEncode(me.hModule);
					VPatchModuleIATEncodeI422(me.hModule);
				} while (Module32Next(snap, &me));
			}
			CloseHandle(snap);
		}
	}
	if (!g_JpegImplHooked && !g_JpegGpaHooked) {
		HMODULE hK = GetModuleHandleW(L"kernel32.dll");
		if (hK) {
			FARPROC gpa = GetProcAddress(hK, "GetProcAddress");
			if (gpa && g_pfGetProcAddress == NULL) {
				g_pfGetProcAddress = (FARPROC(WINAPI*)(HMODULE, LPCSTR))gpa;
				if (VSetHookLogged(L"GetProcAddress", (PVOID*)&g_pfGetProcAddress, (PVOID)hkGetProcAddress))
					g_JpegGpaHooked = true;
			}
		}
	}

	bool ok = g_JpegImplHooked || g_JpegIATHooked || g_JpegI422ImplHooked || g_JpegI422Hooked || g_JpegGpaHooked;
	if (ok && !hk28) {
		hk28 = 1;
		VDebugLog(L"JPEG hook installed impl=%d iat=%d i422impl=%d i422iat=%d gpa=%d",
			g_JpegImplHooked, g_JpegIATHooked, g_JpegI422ImplHooked, g_JpegI422Hooked, g_JpegGpaHooked);
		InterlockedExchange(&jpegHookLogPending, 1);
	}
	VReportJPEGHookStatus();
}

static DWORD WINAPI VJpegHookRetryThread(LPVOID)
{
	for (int i = 0; i < 120; ++i) {            // 最多约 60 秒等待 LibJPEG20 加载
		VEnsureJPEGHook();
		if (g_JpegImplHooked || g_JpegIATHooked || g_JpegI422ImplHooked || g_JpegI422Hooked)
			break;
		Sleep(500);
	}
	VDebugLog(L"DispcapHelper JPEG hook retry done impl=%d iat=%d i422impl=%d i422iat=%d",
		g_JpegImplHooked, g_JpegIATHooked, g_JpegI422ImplHooked, g_JpegI422Hooked);
	return 0;
}

void VStartJpegHookRetryThread()
{
	HANDLE h = CreateThread(NULL, 0, VJpegHookRetryThread, NULL, 0, NULL);
	if (h) CloseHandle(h);
}

// 枚举并注入已存在的捕获辅助进程(DispcapHelper)。
// 原因:DispcapHelper 常驻或由极域在 DzjsTrainer 注入 StudentMain 之前创建,或由其他进程创建,
//       HK CreateProcessW 无法捕获,故主动扫描注入——无论该进程何时启动、由谁创建都能覆盖。
// 仅注入名为 DispcapHelper.exe 的进程;并记录 pid 实现幂等(避免重复 LoadLibrary/DllMain)。
static DWORD WINAPI VRemoteInjectThread(LPVOID)
{
	bool firstScan = true;
	for (int i = 0; i < 900; ++i) {            // 最多 30 分钟,每 2 秒一轮
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
			DWORD found = 0, injected = 0;
			if (Process32First(snap, &pe)) {
				do {
					if (firstScan) {
						// 诊断:打印疑似捕获进程名,便于确认真实进程名(万一不是 DispcapHelper.exe)
						std::wstring nm = pe.szExeFile;
						if (nm.find(L"disp") != std::wstring::npos || nm.find(L"cap") != std::wstring::npos ||
							nm.find(L"screen") != std::wstring::npos || nm.find(L"mirror") != std::wstring::npos ||
							nm.find(L"myth") != std::wstring::npos)
							VDebugLog(L"capture-helper scan candidate name=%s pid=%lu", pe.szExeFile, pe.th32ProcessID);
					}
					if (_wcsicmp(pe.szExeFile, L"DispcapHelper.exe") == 0) {
						++found;
						if (g_injectedCaptured.find(pe.th32ProcessID) == g_injectedCaptured.end()) {
							HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
							if (h) {
								BOOL ok = VInjectHookIntoProcess(h);
								if (ok) { g_injectedCaptured.insert(pe.th32ProcessID); ++injected; }
								else VDebugLog(L"capture-helper scan inject failed pid=%lu", pe.th32ProcessID);
								CloseHandle(h);
							}
							else VDebugLog(L"capture-helper scan open failed pid=%lu error=%lu", pe.th32ProcessID, GetLastError());
						}
					}
				} while (Process32Next(snap, &pe));
			}
			CloseHandle(snap);
			if (firstScan || injected > 0 || found > 0)
				VDebugLog(L"capture-helper scan found=%lu injected=%lu", found, injected);
			firstScan = false;
		}
		Sleep(2000);
	}
	return 0;
}

void VReportJPEGHookStatus()
{
	LONG hookStatus = InterlockedExchange(&jpegHookLogPending, 0);
	if (hookStatus == 1)
		VSendMessageBack(L"vback:JPEG hook installed", hWndMsgCenter);
	else if (hookStatus == -1)
		VSendMessageBack(L"vback:JPEG hook install failed", hWndMsgCenter);
	else if (hookStatus == -2)
		VSendMessageBack(L"vback:EncodeToJPEGBuffer export not found", hWndMsgCenter);

	LONG previewStatus = InterlockedExchange(&frameReplacementLogPending, 0);
	if (previewStatus != 0)
	{
		WCHAR status[128];
		if (previewStatus == 1)
			swprintf_s(status, L"vback:White preview active width=%ld height=%ld stride=%ld", frameReplacementLogWidth, frameReplacementLogHeight, frameReplacementLogStride);
		else if (previewStatus == 2)
			swprintf_s(status, L"vback:Black video replacement active width=%ld height=%ld stride=%ld", frameReplacementLogWidth, frameReplacementLogHeight, frameReplacementLogStride);
		else if (previewStatus == -1)
			swprintf_s(status, L"vback:Frame replacement invalid args width=%ld height=%ld stride=%ld", frameReplacementLogWidth, frameReplacementLogHeight, frameReplacementLogStride);
		else if (previewStatus == 3)
			swprintf_s(status, L"vback:Image overlay active width=%ld height=%ld stride=%ld", frameReplacementLogWidth, frameReplacementLogHeight, frameReplacementLogStride);
		else
			wcscpy_s(status, L"vback:Frame replacement buffer allocation failed");
		VSendMessageBack(status, hWndMsgCenter);
	}
}

// 调试用二分开关：ini 里 [JTSettings] HooksLimit=N 时，全量集只装前 N 个钩子，
// 0 或缺省 = 全部。用来二分定位"是哪个钩子把极域卡死的"。
// 判定点在 VInstallHooks 开头从 ini 读取，见 VLoadHookLimit()。
static int g_hookLimit = 0;
static int g_hookSeq = 0;
bool VHookAllowed()
{
	++g_hookSeq;
	return g_hookLimit <= 0 || g_hookSeq <= g_hookLimit;
}

// 调用跟踪：全量集的钩子逐个记录调用。卡死时日志里**最后一条 HOOK 记录**
// 就是卡住的地方 —— 比二分法快得多。
//
// **默认关闭**：每次钩子调用都写一次日志，在 BitBlt / CreateFileW 这类热路径上
// 开销很大，还会把日志刷到几万行。排查时在 ini 里打开：
//     [JTSettings]
//     HooksTrace=1
static volatile LONG g_hookTraceCount = 0;
void VTraceHook(const wchar_t* name)
{
	if (!g_hookTrace) return;
	LONG n = InterlockedIncrement(&g_hookTraceCount);
	if (n <= 20000)
		VDebugLog(L"HOOK %s #%ld", name, n);
}

// 只记录模式下的动作记录。
//
// 这些调用点全都在热路径上（SetWindowPos / SendInput / DeviceIoControl 每秒可能上百次），
// 而 VDebugLog 每次都要拿自旋锁 + 开文件，无条件写会把被注入的进程拖垮。
// 所以按总量采样：前 500 条全记，之后每 100 条记一条。要看完整序列打开 HooksTrace。
void VRecordHook(const wchar_t* name, const wchar_t* fmt, ...)
{
	static volatile LONG total = 0;
	const LONG n = InterlockedIncrement(&total);
	if (n > 20000) return;
	if (n > 500 && (n % 100) != 0) return;

	WCHAR detail[512] = { 0 };
	if (fmt && fmt[0]) {
		va_list args;
		va_start(args, fmt);
		_vsnwprintf_s(detail, _countof(detail), _TRUNCATE, fmt, args);
		va_end(args);
	}
	VDebugLog(L"RECORD %s %s%s%s", name, detail,
		detail[0] ? L" " : L"", L"(未拦截)");
}

bool VVideoReplacementEnabled()
{
	return g_VideoProps.m_EnableModifyVideoStream;
}

void VInstallHooks(VirusMode mode) {
	VDebugLog(L"VInstallHooks begin mode=%d", mode);

	// 批量安装作用域：本函数有多个提前 return（最小帧替换模式等），
	// 由 RAII 保证每条路径都在返回前一次性启用本批钩子 ——
	// 不会出现"装到一半已经生效"的半套状态。
	HookEngine::ScopedBatch hookBatch;

	// 调试用二分开关：[JTSettings] HooksLimit=N 时全量集只装前 N 个钩子（0/缺省 = 全部）。
	// 每装一个钩子 VHookAllowed() 自增一次，超限的直接跳过（hkN 保持 0 = 未装）。
	{
		WCHAR w[32] = {};
		GetPrivateProfileStringW(L"JTSettings", L"HooksLimit", L"", w, 32, mainIniPath);
		g_hookLimit = w[0] ? _wtoi(w) : 0;
		// 钩子调用跟踪，默认关；见 VTraceHook 上方说明。
		GetPrivateProfileStringW(L"JTSettings", L"HooksTrace", L"", w, 32, mainIniPath);
		g_hookTrace = w[0] == L'1' || w[0] == L'T' || w[0] == L't';
		if (g_hookTrace) VDebugLog(L"VInstallHooks HooksTrace=1 (钩子调用跟踪已打开)");
		if (g_hookLimit > 0)
			VDebugLog(L"VInstallHooks HooksLimit=%d -> 全量集只装前 %d 个钩子", g_hookLimit, g_hookLimit);
	}

	//HookEngine::Install（原 Mhook_SetHook）
	HMODULE hUser32 = GetModuleHandle(L"user32.dll");
	HMODULE hKernel32 = GetModuleHandle(L"kernel32.dll");
	HMODULE hShell32 = GetModuleHandle(L"shell32.dll");
	HMODULE hDwmApi = GetModuleHandle(L"dwmapi.dll");
	HMODULE hGdi32 = GetModuleHandle(L"gdi32.dll");
	HMODULE hLibAVCodec52 = GetModuleHandle(L"LibAVCodec52.dll");
	HMODULE hFltLib = GetModuleHandle(L"FltLib.dll");
	VDebugLog(L"modules user32=%p kernel32=%p gdi32=%p dwmapi=%p fltlib=%p", hUser32, hKernel32, hGdi32, hDwmApi, hFltLib);


	raSetWindowPos = (fnSetWindowPos)GetProcAddress(hUser32, "SetWindowPos");
	raMoveWindow = (fnMoveWindow)GetProcAddress(hUser32, "MoveWindow");
	raSetForegroundWindow = (fnSetForegroundWindow)GetProcAddress(hUser32, "SetForegroundWindow");
	faSetWindowsHookExA = (fnSetWindowsHookExA)GetProcAddress(hUser32, "SetWindowsHookExA");
	faDeferWindowPos = (fnDeferWindowPos)GetProcAddress(hUser32, "DeferWindowPos");
	faBringWindowToTop = (fnBringWindowToTop)GetProcAddress(hUser32, "BringWindowToTop");
	faSendInput = (fnSendInput)GetProcAddress(hUser32, "SendInput");
	faChangeDisplaySettingsW = (fnChangeDisplaySettingsW)GetProcAddress(hUser32, "ChangeDisplaySettingsW");
	famouse_event = (fnmouse_event)GetProcAddress(hUser32, "mouse_event");
	faSetWindowLongA = (fnSetWindowLongA)GetProcAddress(hUser32, "SetWindowLongA");
	faSetWindowLongW = (fnSetWindowLongW)GetProcAddress(hUser32, "SetWindowLongW");
	faShowWindow = (fnShowWindow)GetProcAddress(hUser32, "ShowWindow");
	faCallNextHookEx = (fnCallNextHookEx)GetProcAddress(hUser32, "CallNextHookEx");
	faUnhookWindowsHookEx = (fnUnhookWindowsHookEx)GetProcAddress(hUser32, "UnhookWindowsHookEx");
	faGetDesktopWindow = (fnGetDesktopWindow)GetProcAddress(hUser32, "GetDesktopWindow");
	faGetWindowDC = (fnGetWindowDC)GetProcAddress(hUser32, "GetWindowDC");
	faGetForegroundWindow = (fnGetForegroundWindow)GetProcAddress(hUser32, "GetForegroundWindow");
	faEnableMenuItem = (fnEnableMenuItem)GetProcAddress(hUser32, "EnableMenuItem");
	faSetClassLongA = (fnSetClassLongA)GetProcAddress(hUser32, "SetClassLongA");
	faSetClassLongW = (fnSetClassLongW)GetProcAddress(hUser32, "SetClassLongW");

	faPostMessageW = (fnPostMessageW)GetProcAddress(hUser32, "PostMessageW");
	faSendMessageW = (fnSendMessageW)GetProcAddress(hUser32, "SendMessageW");
	faTerminateProcess = (fnTerminateProcess)GetProcAddress(hKernel32, "TerminateProcess");

	raDeviceIoControl = (fnDeviceIoControl)GetProcAddress(hKernel32, "DeviceIoControl");
	faCreateFileA = (fnCreateFileA)GetProcAddress(hKernel32, "CreateFileA");
	faCreateFileW = (fnCreateFileW)GetProcAddress(hKernel32, "CreateFileW");
	faCreateProcessW = (fnCreateProcessW)GetProcAddress(hKernel32, "CreateProcessW");
	faCreateProcessA = (fnCreateProcessA)GetProcAddress(hKernel32, "CreateProcessA");
	faWinExec = (fnWinExec)GetProcAddress(hKernel32, "WinExec");

	faExitWindowsEx = (fnExitWindowsEx)GetProcAddress(hUser32, "ExitWindowsEx");
	//faShellExecuteW = (fnShellExecuteW)GetProcAddress(hShell32, "ShellExecuteW");
	//faShellExecuteExW = (fnShellExecuteExW)GetProcAddress(hShell32, "ShellExecuteExW");

	faCreateDCW = (fnCreateDCW)GetProcAddress(hGdi32, "CreateDCW");
	fpBitBlt = (fnBitBlt)GetProcAddress(hGdi32, "BitBlt");

	// 截屏另外三条路 + SetWindowsHookExW（hk42..hk45）。
	// 注意 gdi32 的 BitBlt/StretchBlt 是**转发导出**，真实实现在 gdi32full.dll；
	// GetProcAddress 会自动跟进转发，返回真实地址，MinHook 改的就是它 ——
	// 调用方从哪条路径进来都会命中。
	faPrintWindow = (fnPrintWindow)GetProcAddress(hUser32, "PrintWindow");
	faStretchBlt = (fnStretchBlt)GetProcAddress(hGdi32, "StretchBlt");
	faGetDIBits = (fnGetDIBits)GetProcAddress(hGdi32, "GetDIBits");
	faSetWindowsHookExW = (fnSetWindowsHookExW)GetProcAddress(hUser32, "SetWindowsHookExW");

	if (hDwmApi) faDwmEnableComposition = (fnDwmEnableComposition)GetProcAddress(hDwmApi, "DwmEnableComposition");
	if (hFltLib) faFilterConnectCommunicationPort = (fnFilterConnectCommunicationPort)GetProcAddress(hFltLib, "FilterConnectCommunicationPort");

	//HMODULE hTDDesk2 = GetModuleHandle(L"libtddesk2.dll");
	//if (hTDDesk2) {
	//	faTDDeskCreateInstance = (fnTDDeskCreateInstance)GetProcAddress(hTDDesk2, "TDDeskCreateInstance");
	//}

	if (mode == VirusModeHook) {
		// 极域宿主的「最小帧替换」分支：开替换或禁监看时只装帧替换必需的那几个钩子，
		// 34 项表不装。通用宿主**不能**走这里 —— 那条路只装 JPEG/BitBlt/LibDeskMonitor/
		// DispFilter，全是极域自己的模块，在第三方进程里一个都不存在，替换必然空转。
		if (!g_customHost && (whitePreviewOnly || g_VideoProps.m_EnableModifyVideoStream))
		{
			VDebugLog(L"VInstallHooks minimal frame-replacement mode color=%s",
				whitePreviewOnly ? L"white" : L"black");
			VEnsureJPEGHook();
			if (captureHelperForwarding)
			{
				// !hkNN 守卫不能省：VInstallHooks 现在会被重复调用（运行中切换开关时补装），
				// 重复挂钩会在原函数指针上再套一层，链越套越长，最终栈溢出把目标进程打崩。
				if (faCreateProcessW && !hk22)
					hk22 = VSetHookLogged(L"CreateProcessW(capture-helper)", (PVOID*)&faCreateProcessW, (PVOID)hkCreateProcessW);
				if (faCreateProcessA && !hk30)
					hk30 = VSetHookLogged(L"CreateProcessA(capture-helper)", (PVOID*)&faCreateProcessA, (PVOID)hkCreateProcessA);
			}
		// 屏幕遮挡(禁止教师看屏):仅在 whitePreviewOnly(即 !allowMonitor)时启用假桌面 + DC 拦截。
		// 这些钩子不依赖 BitBlt,不会引起卡死。注意:只开 EnableVideoModify 但允许监看时不应装它们,否则会误挡教师画面。
		if (whitePreviewOnly) {
			if (!hk27) hk27 = VSetHookLogged(L"GetDesktopWindow", (PVOID*)&faGetDesktopWindow, (PVOID)hkGetDesktopWindow);
			if (!hk26) hk26 = VSetHookLogged(L"GetWindowDC", (PVOID*)&faGetWindowDC, (PVOID)hkGetWindowDC);
			if (!hk33) hk33 = VSetHookLogged(L"CreateDCW", (PVOID*)&faCreateDCW, (PVOID)hkCreateDCW);
			if (!hk23 && faDwmEnableComposition) hk23 = VSetHookLogged(L"DwmEnableComposition", (PVOID*)&faDwmEnableComposition, (PVOID)hkDwmEnableComposition);
		}
		// 视频流修改(替换帧):仅在显式启用 EnableVideoModify 时安装 BitBlt 钩子。
		// 已做线程安全加固(临界区 + 引用计数);未提供替换帧时退化为纯透传,不会卡死。
		if (g_VideoProps.m_EnableModifyVideoStream && fpBitBlt && !hk41)
			hk41 = VSetHookLogged(L"BitBlt(video-modify)", (PVOID*)&fpBitBlt, (PVOID)Hook_BitBlt);
	// 视频流"纯黑"模式接线:设捕获尺寸参考(无外部替换画面 → 触发纯黑)。
	// Hook_BitBlt 在捕获辅助进程(DispcapHelper)会对所有 BitBlt 刷黑,
	// 在学生端主进程(StudentMain)仅对与捕获尺寸匹配的 BitBlt 刷黑,以防破坏 UI。
	if (hk41 && g_VideoProps.m_EnableModifyVideoStream && !g_surface.HasSurface()) {
			g_VideoProps.m_Width = GetSystemMetrics(SM_CXSCREEN);
			g_VideoProps.m_Height = GetSystemMetrics(SM_CYSCREEN);
		}
		// 新方案(视频流修改):拦截 LibDeskMonitor 的 GDI 抓屏("打开看"),在捕获进内存位图后刷黑。
		if (g_VideoProps.m_EnableModifyVideoStream)
			VEnsureLibDeskMonitorCaptureHook();
		// 安装 DispFilter.dll 加载检测(LoadLibraryW/ExW 钩)+ 无限重试:DispFilter 由 libTDDesk2
		// 在"打开看"开始时动态加载,必须在其加载瞬间/之后装上 DispDXGIBitBlt 捕获钩,否则漏黑。
		if (g_VideoProps.m_EnableModifyVideoStream) {
			if (!g_pfLoadLibraryW)
				g_pfLoadLibraryW = (fnLoadLibraryW)GetProcAddress(hKernel32, "LoadLibraryW");
			if (g_pfLoadLibraryW && !hkLLW)
				hkLLW = VSetHookLogged(L"LoadLibraryW(dispfilter-detect)", (PVOID*)&g_pfLoadLibraryW, (PVOID)hkLoadLibraryW);
			if (!g_pfLoadLibraryExW)
				g_pfLoadLibraryExW = (fnLoadLibraryExW)GetProcAddress(hKernel32, "LoadLibraryExW");
			if (g_pfLoadLibraryExW && !hkLLX)
				hkLLX = VSetHookLogged(L"LoadLibraryExW(dispfilter-detect)", (PVOID*)&g_pfLoadLibraryExW, (PVOID)hkLoadLibraryExW);
			VEnsureDispDXGIHook();               // 若 DispFilter 此刻已加载,立即装
			VStartDispDXGIHookRetryThread();     // 否则无限重试直到装上
		}
		if (captureHelperForwarding &&
			InterlockedCompareExchange(&g_remoteInjectStarted, 1, 0) == 0) {
			// 同样只能起一次：重复调用会重复枚举并注入捕获辅助进程。
			HANDLE hRI = CreateThread(NULL, 0, VRemoteInjectThread, NULL, 0, NULL);
			if (hRI) CloseHandle(hRI);
			else InterlockedExchange(&g_remoteInjectStarted, 0);
		}
		VDebugLog(L"VInstallHooks end mode=minimal hk28=%d hk27=%d hk26=%d hk33=%d hk23=%d hk41=%d", hk28, hk27, hk26, hk33, hk23, hk41);
		return;
		}

		//HMODULE hTDMaster = GetModuleHandle(L"libTDMaster.dll");
		//UnHookLocalInput = (fnUnHookLocalInput)GetProcAddress(hTDMaster, "UnHookLocalInput");
		//if (UnHookLocalInput) UnHookLocalInput();

		VOutPutStatus(L"[D] Install hook in mode VirusModeHook");

		// 下面这一整套钩子没有 !hkNN 守卫。VInstallHooks 现在会被"运行中切换开关"
		// 重复调用（见 VHandleMsg 的 reset / setmonitor / setvideo 分支），
		// 若重复执行会在原函数指针上再套一层，链越套越长最终栈溢出把目标进程打崩。
		// 已经装过就直接返回；hk1 是本套的第一个。
		if (hk1) return;

		// 全量集改由 HookRegistry 的有序表安装：每项带域标签、原始指针、detour
		// 与安装标记，顺序由 seq 固定（HooksLimit=N 的语义不会因分域而变化）。
		// detour 实现分别位于 GuardWindow/GuardProcess/GuardInput，显示域见 GuardDisplay.h。
		HookRegistry::InstallFullSet();

		// 通用宿主：全量集装完之后再补帧替换钩子。
		// BitBlt 不在有序表里（表里 hk41 是 SkipWithLog —— 全量钩会误伤极域自己的 UI 绘制），
		// 而第三方截屏绝大多数就是 GetDC(NULL) + BitBlt，所以这里按需单独装。
		// hk42/43/44（PrintWindow / GetDIBits / StretchBlt）已由全量集装上，它们自己
		// 按 PlanForCapture 决定透传还是替换，开关关着时只写一条 RECORD 日志。
		// minimal 模式连显示域都跳过了，装 BitBlt 会让「minimal 不碰显示」这条承诺失效，
		// 所以只在 full 模式下补。
		if (g_customHost && g_VideoProps.m_EnableModifyVideoStream && !g_customHookMinimal)
			VEnsureFrameReplacementHooks();

		VDebugLog(L"VInstallHooks end mode=hook hk28=%d installed=%d window=%d process=%d input=%d display=%d",
			hk28, HookRegistry::TotalInstalled(), GuardWindow::HookCount(),
			GuardProcess::HookCount(), GuardInput::HookCount(), GuardDisplay::HookCount());
	}
	if (mode == VirusModeMaster) {

		g_hhook = SetWindowsHookExA(WH_CBT, VCBTProc, hInst, GetCurrentThreadId());
		
		VOutPutStatus(L"[D] Install hook in mode VirusModeMaster");

		hk8 = VSetHookLogged(L"SetWindowsHookExA(master)", (PVOID*)&faSetWindowsHookExA, (PVOID)hkSetWindowsHookExA);
		hk19 = VSetHookLogged(L"ExitWindowsEx(master)", (PVOID*)&faExitWindowsEx, (PVOID)hkExitWindowsEx);
		hk22 = VSetHookLogged(L"CreateProcessW(master)", (PVOID*)&faCreateProcessW, (PVOID)hkCreateProcessW);
		VDebugLog(L"VInstallHooks end mode=master");
	}
}

// 运行中切换开关时补装钩子。
//
// 这里刻意**不调用完整的 VInstallHooks**：那个函数除了下面这几个钩子，还会做
// 全进程模块枚举（逐个改 IAT）、起远程注入线程、以及在两个标志都为假时落到整套
// hk1..hk40 —— 这些都不适合在消息处理里反复执行。之前正是因为从消息处理里调用
// 完整 VInstallHooks，让模块枚举跑了第二次，极域进程随即闪退。
//
// 只补"当前标志需要、且尚未安装"的那几个；每处都有 !hkNN 守卫，重复调用安全。
static void VEnsureFrameReplacementHooks()
{
	if (g_Unloading) return;

	// 禁止教师看屏：假桌面 + DC 拦截
	if (whitePreviewOnly) {
		if (!hk27 && faGetDesktopWindow)
			hk27 = VSetHookLogged(L"GetDesktopWindow", (PVOID*)&faGetDesktopWindow, (PVOID)hkGetDesktopWindow);
		if (!hk26 && faGetWindowDC)
			hk26 = VSetHookLogged(L"GetWindowDC", (PVOID*)&faGetWindowDC, (PVOID)hkGetWindowDC);
		if (!hk33 && faCreateDCW)
			hk33 = VSetHookLogged(L"CreateDCW", (PVOID*)&faCreateDCW, (PVOID)hkCreateDCW);
		if (!hk23 && faDwmEnableComposition)
			hk23 = VSetHookLogged(L"DwmEnableComposition", (PVOID*)&faDwmEnableComposition, (PVOID)hkDwmEnableComposition);
	}

	// 视频流替换：帧替换钩子
	if (g_VideoProps.m_EnableModifyVideoStream && fpBitBlt && !hk41)
		hk41 = VSetHookLogged(L"BitBlt(video-modify)", (PVOID*)&fpBitBlt, (PVOID)Hook_BitBlt);
}

void VUnInstallHooks() {
	g_Unloading = true;
	while (InterlockedCompareExchange(&g_DispDXGIInstallLock, 1, 0) != 0)
		Sleep(1);

	// inline 钩不在这里逐个还原：HookEngine::DisableAll() 按 hook 身份一次性恢复原始指令，
	// 运行中补装的那些（LoadLibraryW/ExW、DispDXGI、LibDeskMonitor）不会漏。
	// 下面先处理引擎管不到的**手工 IAT 补丁**。

	{
		// 1) JPEG 三层钩改写过的多模块 IAT 槽
		for (size_t i = 0; i < g_JpegIatPatches.size(); ++i) {
			JpegIatPatch& p = g_JpegIatPatches[i];
			DWORD oldProt = 0; MEMORY_BASIC_INFORMATION mbi;
			VirtualQuery(p.entry, &mbi, sizeof(mbi));
			if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
				*p.entry = p.orig;
				VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), p.entry, sizeof(ULONG_PTR));
			}
		}
		g_JpegIatPatches.clear();

		// 2) JPEG I422 变体
		for (size_t i = 0; i < g_JpegI422IatPatches.size(); ++i) {
			JpegIatPatch& p = g_JpegI422IatPatches[i];
			DWORD oldProt = 0; MEMORY_BASIC_INFORMATION mbi;
			VirtualQuery(p.entry, &mbi, sizeof(mbi));
			if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
				*p.entry = p.orig;
				VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), p.entry, sizeof(ULONG_PTR));
			}
		}
		g_JpegI422IatPatches.clear();
	}

	// 停用全部 inline 钩，并等在飞 detour 归零 —— 之后才允许 MH_Uninitialize 释放 trampoline。
	const int disableFailures = HookEngine::DisableAll();
	const bool drained = HookEngine::WaitForDrain(3000);
	VDebugLog(L"VUnInstallHooks engineHooks=%d disableFailures=%d drained=%d",
		HookEngine::Count(), disableFailures, drained ? 1 : 0);

	// 钩子已停用，状态位统一复位（不再逐个 Unhook；hkNN 现在只是"装过没"的记录）。
	g_JpegIATHooked = false; g_JpegI422Hooked = false;
	g_JpegImplHooked = false; g_JpegI422ImplHooked = false; g_JpegGpaHooked = false;
	hkLLW = false; hkLLX = false; hk28 = 0;
	if (g_DispDXGIHooked) { g_DispDXGIHooked = false; g_origDispDXGIBitBlt = NULL; }
	if (g_DispDXGIGetScreenUpdateHooked) { g_DispDXGIGetScreenUpdateHooked = false; g_origDispDXGIGetScreenUpdate = NULL; }
	InterlockedExchange(&g_DispDXGIInstallLock, 0);
	InterlockedExchange(&g_DispDXGIRetryStarted, 0);
	InterlockedExchange(&g_remoteInjectStarted, 0);
	for (int i = 0; i < 2; ++i) {
		InterlockedExchange(&g_DispDXGILastUpdateTick[i], 0);
		InterlockedExchange(&g_DispDXGIFullRefreshPending[i], 1);
	}

	// LibDeskMonitor.dll 捕获钩(BitBlt/StretchBlt IAT)还原:退出前必须把导入表槽指回真实 gdi32,
	// 否则 LibDeskMonitor 后续调用 BitBlt 会跳转到已释放的钩子代码而崩溃。
	if (!g_LdmIatPatches.empty()) {
		for (size_t i = 0; i < g_LdmIatPatches.size(); ++i) {
			LdmIatPatch& p = g_LdmIatPatches[i];
			DWORD oldProt = 0; MEMORY_BASIC_INFORMATION mbi;
			VirtualQuery(p.entry, &mbi, sizeof(mbi));
			if (VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
				*p.entry = p.orig;
				VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), p.entry, sizeof(ULONG_PTR));
			}
		}
		g_LdmIatPatches.clear();
		g_LdmCaptureHooked = false;
		g_Desk2GdiFlushHooked = false;
		g_origLdmBitBlt = NULL;
		g_origLdmStretchBlt = NULL;
		g_origDesk2GdiFlush = NULL;
	}

	// g_Unloading 已置位,Hook_BitBlt 不会再进入替换路径,此处可安全清理。
	// 顺序:先停解码线程(它还会 Publish 帧),再销毁两个状态对象
	// (Shutdown 各自释放当前内容与 pending,并销毁内部临界区)。
	VStopReplacementVideo();
	g_surface.Shutdown();
	g_frames.Shutdown();
	if (g_VideoCsInit) { DeleteCriticalSection(&g_VideoCs); g_VideoCsInit = false; }
	// 释放全部 trampoline。必须放在上面所有仍可能调进钩子的清理动作之后。
	HookEngine::Uninitialize();

	VOutPutStatus(L"[D] UnInstall all hook");

	if (g_hhook) {
		UnhookWindowsHookEx(g_hhook);
		g_hhook = NULL;
	}
}

//Fuck driver devices
HANDLE hDeviceTDKeybd = NULL;
HANDLE hDeviceTDProcHook = NULL;
HANDLE hDeviceTDNetFilter = NULL;

void VOpenFuckDrivers() {

	hDeviceTDNetFilter = CreateFileW(L"\\\\.\\TDNetFilter", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	hDeviceTDKeybd = CreateFileW(L"\\\\.\\TDKeybd", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	hDeviceTDProcHook = CreateFileW(L"\\\\.\\TDProcHook", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}
void VCloseFuckDrivers()
{
	if (hDeviceTDNetFilter) CloseHandle(hDeviceTDNetFilter);
	if (hDeviceTDProcHook) CloseHandle(hDeviceTDProcHook);
	if (hDeviceTDKeybd) CloseHandle(hDeviceTDKeybd);
}

//Fake hook for TDMaster





//Hook stubs

int __cdecl hkEncodeToJPEGBuffer(const BYTE *pixels, int width, int height, int stride, BYTE *jpegOutput, DWORD *jpegLength, int quality, int reserved1, int reserved2)
{
	HookEngine::DetourScope detourScope;
	LONG callIndex = InterlockedIncrement(&jpegCallCount);
	bool traceCall = callIndex <= 3;

	// 三态决策（透传 / 覆盖替换图 / 填充纯色）与颜色口径统一由 codec 回答：
	//   allowMonitor=false 且无图 ⇒ 白；启用替换但允许监看 ⇒ 黑。
	ReplaceCodec::Request request;
	request.allowMonitor = allowMonitor;
	request.videoModify = g_VideoProps.m_EnableModifyVideoStream;
	const ReplaceCodec::Plan plan = ReplaceCodec::PlanJpegFrame(request);

	if (traceCall)
		VDebugLog(L"JPEG call enter index=%ld pixels=%p width=%d height=%d stride=%d output=%p lengthPtr=%p quality=%d allowMonitor=%d videoModify=%d action=%d",
			callIndex, pixels, width, height, stride, jpegOutput, jpegLength, quality, allowMonitor,
			g_VideoProps.m_EnableModifyVideoStream, (int)plan.action);

	if (plan.action != ReplaceCodec::Action::Passthrough)
	{
		if (!faEncodeToJPEGBuffer || !jpegOutput || !jpegLength ||
			!ReplaceCodec::ValidateRgb24(width, height, stride))
		{
			if (traceCall) VDebugLog(L"JPEG call rejected index=%ld reason=invalid-arguments", callIndex);
			if (InterlockedCompareExchange(&frameReplacementInvalidReported, 1, 0) == 0)
			{
				InterlockedExchange(&frameReplacementLogWidth, width);
				InterlockedExchange(&frameReplacementLogHeight, height);
				InterlockedExchange(&frameReplacementLogStride, stride);
				InterlockedExchange(&frameReplacementLogPending, -1);
			}
			return FALSE;
		}

		// 优先:自定义图片覆盖源像素(覆盖预览与打开看两条路径)
		ReplaceState::Frame frame;
		if (request.videoModify && g_frames.Acquire(frame))
		{
			ReplaceCodec::BlitParams blit;
			blit.dstWidth = width;
			blit.dstHeight = height;
			blit.dstStride = stride;
			blit.srcWidth = frame.width;
			blit.srcHeight = frame.height;
			blit.swapRB = true;        // EncodeToJPEGBuffer 消费 BGR24
			blit.flipVertical = true;  // 该缓冲按 bottom-up 行序解释
			const bool overlaid = ReplaceCodec::BlitScaled(const_cast<BYTE*>(pixels), frame.rgb, blit);
			const int imageW = frame.width, imageH = frame.height;
			g_frames.Release();

			if (overlaid)
			{
				if (traceCall) VDebugLog(L"JPEG image overlay index=%ld imgW=%d imgH=%d", callIndex, imageW, imageH);
				if (InterlockedCompareExchange(&frameReplacementReported, 1, 0) == 0)
				{
					InterlockedExchange(&frameReplacementLogWidth, width);
					InterlockedExchange(&frameReplacementLogHeight, height);
					InterlockedExchange(&frameReplacementLogStride, stride);
					InterlockedExchange(&frameReplacementLogPending, 3);
				}
				ULONGLONG encodeStart = GetTickCount64();
				int result = faEncodeToJPEGBuffer(const_cast<BYTE*>(pixels), width, height, stride,
					jpegOutput, jpegLength, quality, reserved1, reserved2);
				if (traceCall) VDebugLog(L"JPEG image call end index=%ld result=%d elapsedMs=%llu", callIndex, result, GetTickCount64() - encodeStart);
				return result;
			}
			// 覆盖失败（尺寸不可用）⇒ 落到下面的纯色填充，失败方向仍是"更像假"。
		}

		const BYTE replacementValue = plan.solidValue;
		const size_t frameSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
		static thread_local std::vector<BYTE> replacementFrame;
		static thread_local BYTE currentReplacementValue = 0;
		try
		{
			if (replacementFrame.size() != frameSize || currentReplacementValue != replacementValue)
			{
				replacementFrame.assign(frameSize, replacementValue);
				currentReplacementValue = replacementValue;
			}
		}
		catch (...)
		{
			if (traceCall) VDebugLog(L"JPEG call rejected index=%ld reason=allocation-failed frameSize=%Iu", callIndex, frameSize);
			InterlockedExchange(&frameReplacementLogPending, -2);
			return FALSE;
		}
		if (traceCall) VDebugLog(L"JPEG replacement buffer ready index=%ld color=%s frameSize=%Iu buffer=%p", callIndex,
			replacementValue ? L"white" : L"black", frameSize, replacementFrame.data());

		if (InterlockedCompareExchange(&frameReplacementReported, 1, 0) == 0)
		{
			InterlockedExchange(&frameReplacementLogWidth, width);
			InterlockedExchange(&frameReplacementLogHeight, height);
			InterlockedExchange(&frameReplacementLogStride, stride);
			InterlockedExchange(&frameReplacementLogPending, replacementValue ? 1 : 2);
		}

		ULONGLONG encodeStart = GetTickCount64();
		if (traceCall) VDebugLog(L"JPEG original call begin index=%ld", callIndex);
		int result = faEncodeToJPEGBuffer(replacementFrame.data(), width, height, stride,
			jpegOutput, jpegLength, quality, reserved1, reserved2);
		if (traceCall) VDebugLog(L"JPEG original call end index=%ld result=%d elapsedMs=%llu", callIndex, result, GetTickCount64() - encodeStart);
		return result;
	}

	ULONGLONG encodeStart = GetTickCount64();
	int result = faEncodeToJPEGBuffer(pixels, width, height, stride,
		jpegOutput, jpegLength, quality, reserved1, reserved2);
	if (traceCall) VDebugLog(L"JPEG passthrough end index=%ld result=%d elapsedMs=%llu", callIndex, result, GetTickCount64() - encodeStart);
	return result;
}
int __cdecl hkEncodeToJPEGBufferI422(const BYTE *pixels, int width, int height, int stride, BYTE *jpegOutput, DWORD *jpegLength, int quality, int reserved1, int reserved2)
{
	HookEngine::DetourScope detourScope;
	LONG callIndex = InterlockedIncrement(&jpegCallCount);
	bool traceCall = callIndex <= 3;
	if (traceCall)
		VDebugLog(L"I422 call enter index=%ld pixels=%p width=%d height=%d stride=%d", callIndex, pixels, width, height, stride);

	// 这条路径只做纯色填充，不支持替换图 —— 用独立的决策函数表达，
	// 避免"有图时覆盖"悄悄生效在一条从未验证过的路径上。
	ReplaceCodec::Request request;
	request.allowMonitor = allowMonitor;
	request.videoModify = g_VideoProps.m_EnableModifyVideoStream;
	const ReplaceCodec::Plan plan = ReplaceCodec::PlanI422Frame(request);

	if (plan.action == ReplaceCodec::Action::Passthrough) {
		if (faEncodeToJPEGBufferI422)
			return faEncodeToJPEGBufferI422(pixels, width, height, stride, jpegOutput, jpegLength, quality, reserved1, reserved2);
		return -1;
	}
	if (!faEncodeToJPEGBufferI422 || !jpegOutput || !jpegLength ||
		width <= 0 || height <= 0 || stride <= 0 ||
		static_cast<SIZE_T>(height) > SIZE_MAX / static_cast<SIZE_T>(stride)) {
		if (faEncodeToJPEGBufferI422)
			return faEncodeToJPEGBufferI422(pixels, width, height, stride, jpegOutput, jpegLength, quality, reserved1, reserved2);
		return -1;
	}
	// 格式无关黑屏:构造全 0 缓冲(YUV I422 时 Y=0 为黑,RGB 全 0 亦为黑),不改写调用方缓冲。
	static thread_local std::vector<BYTE> blackBuf;
	SIZE_T frameSize = static_cast<SIZE_T>(stride) * static_cast<SIZE_T>(height);
	if (blackBuf.size() != frameSize) {
		try { blackBuf.assign(frameSize, plan.solidValue); }
		catch (...) {
			return faEncodeToJPEGBufferI422(pixels, width, height, stride, jpegOutput, jpegLength, quality, reserved1, reserved2);
		}
	}
	if (traceCall) VDebugLog(L"I422 blackened index=%ld frameSize=%Iu", callIndex, frameSize);
	if (InterlockedCompareExchange(&frameReplacementReported, 1, 0) == 0) {
		InterlockedExchange(&frameReplacementLogWidth, width);
		InterlockedExchange(&frameReplacementLogHeight, height);
		InterlockedExchange(&frameReplacementLogStride, stride);
		InterlockedExchange(&frameReplacementLogPending, 2);
	}
	int result = faEncodeToJPEGBufferI422(blackBuf.data(), width, height, stride,
		jpegOutput, jpegLength, quality, reserved1, reserved2);
	return result;
}


//HOOK Virus stub
EXTERN_C HRESULT __declspec(dllexport) __cdecl TDAjustCreateInstance(CLSID *rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, IID *riid, LPVOID *ppv)
{
	return faTDAjustCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

HBITMAP hIconRed, hIconGreen, hIconGrey;
HWND hStatusFakeFull, hStatusMain, hStatusLock;


INT_PTR CALLBACK FakeDesktopWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message)
	{
	case WM_INITDIALOG: {
		if(raSetWindowPos) raSetWindowPos(hDlg, HWND_BOTTOM, -screenWidth, -screenHeight, screenWidth, screenHeight, SWP_NOACTIVATE);
		else SetWindowPos(hDlg, HWND_BOTTOM, -screenWidth, -screenHeight, screenWidth, screenHeight, SWP_NOACTIVATE);
		return TRUE;
	}
	case WM_DESTROY: {

		break;
	}
	case WM_DISPLAYCHANGE: {
		VParamInit();
		SendMessage(hDlg, WM_INITDIALOG, NULL, NULL);
		break;
	}
	case WM_QUERYENDSESSION: {
		DestroyWindow(hDlg);
		break;
	}
	default:
		break;
	}
	return 0;
}
INT_PTR CALLBACK MainWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_HOOK_INITIALIZED: {
		SetTimer(hDlg, TIMER_WATCH_DOG_SRV, 6666, NULL);
		SetTimer(hDlg, TIMER_AUTO_HIDE, 2600, NULL);
		SetTimer(hDlg, TIMER_UI_HEARTBEAT, 10000, NULL);
		// 通用宿主只把消息中心当消息泵用，不显示状态窗口/假桌面，
		// 避免在用户指定的进程里凭空冒出一个窗口。
		if (!g_customHost) {
			ShowWindow(fakeDesktopWindow, SW_SHOW);
			UpdateWindow(fakeDesktopWindow);
			ShowWindow(hDlg, SW_SHOW);
			UpdateWindow(hDlg);
		}
		VDebugLog(L"MainWndProc initialization completed customHost=%d windowShown=%d",
			g_customHost, !g_customHost);
		return TRUE;
	}
	case WM_INITDIALOG: {
		SetWindowText(hDlg, L"JiYu Trainer Virus Window");
		HWND statusList = GetDlgItem(hDlg, IDC_STATUS_LIST);
		if (statusList)
			DestroyWindow(statusList);
		hListBoxStatus = NULL;

		hIconRed = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_BITMAP_RED));
		hIconGreen = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_BITMAP_GREEN));
		hIconGrey = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_BITMAP_GRRY));

		hStatusMain = GetDlgItem(hDlg, IDC_STATUS_RUNNING);
		hStatusLock = GetDlgItem(hDlg, IDC_STATUS_LOCK);
		hStatusFakeFull = GetDlgItem(hDlg, IDC_STATUS_FAKEFULL);

		break;
	}
	case WM_PAINT: {
		PAINTSTRUCT paint = { 0 };
		HDC dc = BeginPaint(hDlg, &paint);
		RECT client = { 0 };
		GetClientRect(hDlg, &client);
		RECT logRect = { 4, 3, client.right - 4, max(4, client.bottom - 18) };
		FillRect(dc, &logRect, GetSysColorBrush(COLOR_WINDOW));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

		TEXTMETRICW metrics = { 0 };
		GetTextMetricsW(dc, &metrics);
		int lineHeight = max(1, metrics.tmHeight + metrics.tmExternalLeading);
		int capacity = max(1, (logRect.bottom - logRect.top) / lineHeight);
		size_t first = statusLines.size() > static_cast<size_t>(capacity)
			? statusLines.size() - capacity : 0;
		int y = logRect.top;
		for (size_t index = first; index < statusLines.size(); ++index) {
			RECT lineRect = { logRect.left, y, logRect.right, y + lineHeight };
			DrawTextW(dc, statusLines[index].c_str(), -1, &lineRect,
				DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			y += lineHeight;
		}
		EndPaint(hDlg, &paint);
		return TRUE;
	}
	case WM_DESTROY: {
		DeleteBitmap(hIconRed);
		DeleteBitmap(hIconGreen);
		DeleteBitmap(hIconGrey);
		KillTimer(hDlg, TIMER_WATCH_DOG_SRV);
		KillTimer(hDlg, TIMER_UI_HEARTBEAT);
		PostQuitMessage(0);
		break;
	}
	case WM_CLOSE: {
		DestroyWindow(hDlg);
		return TRUE;
	}
	case WM_SYSCOMMAND: {
		if (wParam == SC_CLOSE)
			DestroyWindow(hWndMsgCenter);
		break;
	}
	case WM_DISPLAYCHANGE: {
		VParamInit();
		break;
	}
	case WM_COMMAND: {
		switch (wParam)
		{
		case IDC_KILL: {
			PostQuitMessage(0);
			ExitProcess(0);
			break;
		}
		case IDC_SMINSIZE: {
			ShowWindow(hDlg, SW_MINIMIZE);
			break;
		}
		case IDC_SHIDE: {
			RECT rc; GetWindowRect(hDlg, &rc);
			SetWindowPos(hDlg, 0, rc.left, -(rc.bottom - rc.top - 8), 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
			break;
		}
		case IDC_SW_STATUS_FAKEFULL:
		case IDC_SW_STATUS_LOCKED: {
			if (isLocked) {
				if (fakeFull) SendMessage(hStatusLock, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGreen);
				else SendMessage(hStatusLock, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconRed);
			}
			else SendMessage(hStatusLock, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGrey);
			break;
		}
		case IDC_SW_STATUS_MAIN_FLASH: {
			SendMessage(hStatusMain, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGreen);
			SetTimer(hDlg, TIMER_LIGNT_DELAY1, 1000, NULL);
			break;
		}
		case IDC_SW_STATUS_MAIN: {
			SendMessage(hStatusMain, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGrey);
			break;
		}
		case IDC_SW_STATUS_MAIN_OUT: {
			SendMessage(hStatusMain, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconRed);
			break;
		}
		case IDC_SW_STATUS_RB_FLASH: {
			SendMessage(hStatusFakeFull, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGreen);
			SetTimer(hDlg, TIMER_LIGNT_DELAY2, 500, NULL);
			break;
		}
		case IDC_SW_STATUS_RB: {
			SendMessage(hStatusFakeFull, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGrey);
			break;
		}
		case IDC_SW_STATUS_RB_FAIL: {
			SendMessage(hStatusFakeFull, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconRed);
			break;
		}
		case IDC_SW_STATUS_RB_FAIL_FLASH: {
			SendMessage(hStatusFakeFull, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconRed);
			SetTimer(hDlg, TIMER_LIGNT_DELAY2, 500, NULL);
			break;
		}
		case IDC_SW_STATUS_RB_FLASH_FAST: {
			SendMessage(hStatusFakeFull, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hIconGreen);
			SetTimer(hDlg, TIMER_LIGNT_DELAY2, 200, NULL);
		}
		default:
			break;
		}
		break;
	}
	case WM_COPYDATA: {
		PCOPYDATASTRUCT  pCopyDataStruct = (PCOPYDATASTRUCT)lParam;
		ULONGLONG started = GetTickCount64();
		VDebugLog(L"WM_COPYDATA begin bytes=%lu sender=%p", pCopyDataStruct ? pCopyDataStruct->cbData : 0, reinterpret_cast<HWND>(wParam));
		if (pCopyDataStruct && pCopyDataStruct->lpData && pCopyDataStruct->cbData >= sizeof(WCHAR))
		{
			WCHAR recvData[256] = { 0 };
			size_t charCount = pCopyDataStruct->cbData / sizeof(WCHAR);
			if (charCount >= _countof(recvData)) charCount = _countof(recvData) - 1;
			wcsncpy_s(recvData, _countof(recvData), static_cast<WCHAR*>(pCopyDataStruct->lpData), charCount);
			recvData[charCount] = L'\0';
			wchar_t* command = new (std::nothrow) wchar_t[charCount + 1];
			if (command) {
				wcscpy_s(command, charCount + 1, recvData);
				CommandContext* context = new (std::nothrow) CommandContext();
				if (!context) {
					VDebugLog(L"WM_COPYDATA queue failed command=%s reason=allocation-failed", recvData);
					delete[] command;
				}
				else {
					context->senderWindow = reinterpret_cast<HWND>(wParam);
					context->command = command;
					VDebugLog(L"WM_COPYDATA queued command=%s sender=%p", recvData, context->senderWindow);
					HANDLE commandThread = CreateThread(NULL, 0, VHandleMsgThread, context, 0, NULL);
					if (commandThread)
						CloseHandle(commandThread);
					else {
						VDebugLog(L"WM_COPYDATA queue failed command=%s error=%lu", recvData, GetLastError());
						delete[] command;
						delete context;
					}
				}
			}
			else
				VDebugLog(L"WM_COPYDATA queue failed command=%s reason=allocation-failed", recvData);
		}
		VDebugLog(L"WM_COPYDATA end elapsedMs=%llu", GetTickCount64() - started);
		break;
	}
	case WM_STATUS_APPEND: {
		wchar_t* status = reinterpret_cast<wchar_t*>(lParam);
		VDebugLog(L"WM_STATUS_APPEND begin text=%s", status ? status : L"(null)");
		if (status)
			VAppendStatusLine(status);
		delete[] status;
		VDebugLog(L"WM_STATUS_APPEND end");
		return TRUE;
	}
	case WM_LBUTTONDOWN: {
		RECT rc; GetWindowRect(hDlg, &rc);
		if (rc.top < 0) {
			SetWindowPos(hDlg, 0, rc.left, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
		}
		else {
			ReleaseCapture();
			SendMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
		}
		break;
	}
	case WM_TIMER: {
		if (wParam == TIMER_AUTO_HIDE) {
			VDebugLog(L"TIMER_AUTO_HIDE forceDisableWatchDog=%d", forceDisableWatchDog);
			KillTimer(hDlg, TIMER_AUTO_HIDE);
			SendMessage(hDlg, WM_COMMAND, IDC_SHIDE, NULL);
			if (doNotShowVirusWindow)
				ShowWindow(hDlg, SW_HIDE);
			if (forceDisableWatchDog)
				KillTimer(hDlg, TIMER_WATCH_DOG_SRV);
		}
		if (wParam == TIMER_WATCH_DOG_SRV) {
			if (InterlockedCompareExchange(&watchdogRunning, 1, 0) == 0) {
				HANDLE watchdogThread = CreateThread(NULL, 0, VWatchdogThread, NULL, 0, NULL);
				if (watchdogThread)
					CloseHandle(watchdogThread);
				else {
					VDebugLog(L"watchdog worker create failed error=%lu", GetLastError());
					InterlockedExchange(&watchdogRunning, 0);
				}
			}
			else
				VDebugLog(L"watchdog tick skipped previous worker still running");
		}
		if (wParam == TIMER_UI_HEARTBEAT)
			VDebugLog(L"UI heartbeat hwnd=%p visible=%d watchdogRunning=%ld", hDlg, IsWindowVisible(hDlg), watchdogRunning);
		if (wParam == TIMER_LIGNT_DELAY1) {
			KillTimer(hDlg, TIMER_LIGNT_DELAY1);
			SendMessage(hDlg, WM_COMMAND, IDC_SW_STATUS_MAIN, NULL);
		}
		if (wParam == TIMER_LIGNT_DELAY2) {
			KillTimer(hDlg, TIMER_LIGNT_DELAY2);
			SendMessage(hDlg, WM_COMMAND, IDC_SW_STATUS_RB, NULL);
		}
		break;
	}
	case WM_QUERYENDSESSION: {
		VCloseMsgCenter();
		break;
	}
	default:
		break;
	}
	return 0;
}
INT_PTR CALLBACK JiYuWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	/*
	if (message == WM_TIMER) {
		KillTimer(hWnd, wParam);
		return 0;
	}
	*/
	if (message == WM_GETMINMAXINFO) {
		PMINMAXINFO pMinMaxInfo = (PMINMAXINFO)lParam;
		pMinMaxInfo->ptMinTrackSize.x = 0;
		pMinMaxInfo->ptMinTrackSize.y = 0;
		return 0;
	}
	else if (message == WM_SIZE) {
		RECT rcWindow;
		RECT rcClient;
		GetWindowRect(hWnd, &rcWindow);
		GetClientRect(hWnd, &rcClient);
	REGIET:
		if (jiYuGBDeskRdWnd == NULL) {
			jiYuGBDeskRdWnd = FindWindowExW(hWnd, NULL, NULL, L"TDDesk Render Window");
			if (jiYuGBDeskRdWnd == NULL) goto JOUT;

			//处理新版极域的广播工具栏
			if (jiYuVersion == jiYuVersions::jiYuVersions402016HH) {
				jiYuGBToolWnd = FindWindowExW(hWnd, jiYuGBDeskRdWnd, NULL, L"");
				if (jiYuGBToolWnd != NULL) {
					RECT rcTool; GetClientRect(jiYuGBToolWnd, &rcTool);
					jiYuGBToolHeight = rcTool.bottom - rcTool.top;
					VOutPutStatus(L"[D] jiYuGBToolWnd : 0x%08x", jiYuGBToolWnd);
					VOutPutStatus(L"[D] jiYuGBToolHeight : %d", jiYuGBToolHeight);
					//Find tool bar window
					//HWND hBtnTop = GetDlgItem(jiYuGBToolWnd, 0x07E5);
					//HWND hBtnFull = GetDlgItem(jiYuGBToolWnd, 0x03EC);
					//EnableWindow(hBtnFull, FALSE);
				}
			}

			//HOOK TDDesk Render Window for WM_SIZE
			WNDPROC oldWndProc = (WNDPROC)GetWindowLongPtr(jiYuGBDeskRdWnd, GWLP_WNDPROC);
			if (oldWndProc != (WNDPROC)JiYuTDDeskWndProc) {
				jiYuTDDeskWndProc = (WNDPROC)oldWndProc;
				SetWindowLongPtr(jiYuGBDeskRdWnd, GWLP_WNDPROC, (LONG_PTR)JiYuTDDeskWndProc);
				VOutPutStatus(L"[J] Hooked jiYuGBDeskRdWnd %d (0x%08x) WNDPROC", jiYuGBDeskRdWnd, jiYuGBDeskRdWnd);
			}
		}
		if (!IsWindow(jiYuGBDeskRdWnd) || GetParent(jiYuGBDeskRdWnd) != hWnd) {
			jiYuGBDeskRdWnd = NULL;
			goto REGIET;
		}
		if (jiYuGBDeskRdWnd != NULL) {
			ShowWindow(jiYuGBDeskRdWnd, SW_SHOW);
			raMoveWindow(jiYuGBDeskRdWnd, 0, isLocked ? 0 : jiYuGBToolHeight, rcClient.right - rcClient.left,
				rcClient.bottom - rcClient.top - (isLocked ? 0 : jiYuGBToolHeight), TRUE);
		}
	}
	else if (message == WM_SHOWWINDOW)
	{
		if (wParam)
		{
			SendMessage(hWnd, WM_SIZE, NULL, NULL);
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			if (raSetWindowPos) raSetWindowPos(hWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
			else SetWindowPos(hWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
		}
		else { VWndListGuard guard; jiYuWndCanSize.push_back(hWnd); }
	}
	else if (message == WM_DESTROY)
	{
		{ VWndListGuard guard; jiYuWnds.remove(hWnd); }
		{ VWndListGuard guard; jiYuWndCanSize.remove(hWnd); }
		VSwitchLockState(false);
		isGbFounded = false;
	}
	else if (message == WM_COMMAND) {
		switch (wParam)
		{
		case IDM_FULL:
		case IDM_TOPMOST: return SendMessage(hWnd, WM_SYSCOMMAND, wParam, NULL);
		case IDM_HELP_GB: {
			MessageBox(hWnd, L"这个右键菜单是 DzjsTrainer 为方便您控制广播全屏和窗口添加的。\n如果您需要打开极域的右键菜单，请在窗口中心右键。\n关于更多帮助，请查看 DzjsTrainer 帮助文档。 ", L"DzjsTrainer 提示", MB_ICONINFORMATION);
			return 0;
		}
		case IDM_HELP_GB2: {
			VSendMessageBack(L"hkb:showhelp", hWndMsgCenter);
			return 0;
		}
		case IDM_CLOSE_GB: return SendMessage(hWnd, WM_SYSCOMMAND, SC_CLOSE, NULL);
		}
		if (jiYuVersion == jiYuVersions::jiYuVersions402016HH) {
			switch (wParam)
			{
			case 0x07E5://Top
				return SendMessage(hWnd, WM_SYSCOMMAND, IDM_TOPMOST, (LPARAM)-1);
			case 0x03EC://Full
				return SendMessage(hWnd, WM_SYSCOMMAND, IDM_FULL, NULL);
			default: break;
			}
		}
	}
	else if (message == WM_SYSCOMMAND) {
		switch (wParam)
		{
		case IDM_TOPMOST: {
			LONG oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);
			if ((oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
			{
				gbCurrentIsTop = false;

				CheckMenuItem(hMenuGb, IDM_TOPMOST, MF_UNCHECKED);
				CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_TOPMOST, MF_UNCHECKED);
				VSendMessageBack(L"hkb:gbuntop", hWndMsgCenter);
			}
			else {
				//通知允许置顶设置
				if (!allowGbTop) VSendMessageBack(L"hkb:algbtop", hWndMsgCenter);
				gbCurrentIsTop = true;

				CheckMenuItem(hMenuGb, IDM_TOPMOST, MF_CHECKED);
				CheckMenuItem(GetSystemMenu(hWnd, FALSE), IDM_TOPMOST, MF_CHECKED);
				VSendMessageBack(L"hkb:gbtop", hWndMsgCenter);
			}
			if (jiYuVersion == jiYuVersions::jiYuVersions402016HH)
				CheckDlgButton(jiYuGBToolWnd, 0x07E5, gbCurrentIsTop);
			break;
		}
		case IDM_FULL: {
			HMENU hMenu = GetSystemMenu(hWnd, FALSE);
			gbFullManual = !gbFullManual;
			if (gbFullManual) {
				CheckMenuItem(hMenuGb, IDM_FULL, MF_CHECKED);
				CheckMenuItem(hMenu, IDM_FULL, MF_CHECKED);
				VSendMessageBack(L"hkb:gbmfull", hWndMsgCenter);
			}
			else {
				CheckMenuItem(hMenuGb, IDM_FULL, MF_UNCHECKED);
				CheckMenuItem(hMenu, IDM_FULL, MF_UNCHECKED);
				VSendMessageBack(L"hkb:gbmnofull", hWndMsgCenter);
			}
			break;
		}
		case SC_CLOSE: {
			if (MessageBox(hWnd, L"您真的要关闭广播窗口吗？", L"DzjsTrainer - 提示", MB_YESNO | MB_ICONEXCLAMATION) == IDYES)
			{
				CloseWindow(hWnd);
				DestroyWindow(hWnd);
			}
			break;
		}
		default: break;
		}
	}
	else if (message == WM_KEYUP) {
		if (wParam == VK_ESCAPE && fakeFull)
			SendMessage(hWnd, WM_SYSCOMMAND, IDM_FULL, NULL);
	}
	else if (message == WM_RBUTTONUP) {

		bool showMenu = true;
		POINT pos;
		pos.x = GET_X_LPARAM(lParam);
		pos.y = GET_Y_LPARAM(lParam);

		if (jiYuVersion == jiYuVersions::jiYuVersions40 && (pos.x > 60 || pos.y > 20)) 
			showMenu = false;
		if (showMenu) {
			ClientToScreen(hWnd, &pos);
			int id = TrackPopupMenu(hMenuGb, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pos.x + 10, pos.y + 10, NULL, hWnd, NULL);
			SendMessage(hWnd, WM_COMMAND, id, NULL);
		}
	}
JOUT:
	if (jiYuWndProc) return jiYuWndProc(hWnd, message, wParam, lParam);
	else return DefWindowProc(hWnd, message, wParam, lParam);
}
INT_PTR CALLBACK JiYuTDDeskWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_SIZE) {
		RECT rcParent;
		GetClientRect(GetParent(hWnd), &rcParent);
		int w = LOWORD(lParam), h = HIWORD(lParam),
			rw = rcParent.right - rcParent.left, rh = rcParent.bottom - rcParent.top - (isLocked ? 0 : jiYuGBToolHeight);
		if (w != rw || h != rh) 
			raMoveWindow(hWnd, 0, isLocked ? 0 : jiYuGBToolHeight, rw, rh, TRUE);
	}
	else if (message == WM_RBUTTONUP) SendMessage(GetParent(hWnd), message, wParam, lParam);
	else if (message == WM_KEYUP) SendMessage(GetParent(hWnd), message, wParam, lParam);
	if (jiYuTDDeskWndProc) return jiYuTDDeskWndProc(hWnd, message, wParam, lParam);
	else return DefWindowProc(hWnd, message, wParam, lParam);
}

// ---- LibDeskMonitor.dll 捕获钩("打开看"实时画面,GDI BitBlt) ----
// 解析目标模块的导入表,把指定(导入 DLL,函数)的 IAT 槽改写为 pNew,并记录原值以便卸载还原。
static BOOL VPatchLdmOne(HMODULE hMod, LPCSTR pszImportDll, LPCSTR pszFunc, PVOID pNew, PVOID* ppOrig)
{
	ULONG_PTR base = (ULONG_PTR)hMod;
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
	IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!impDir.Size) return FALSE;
	PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + impDir.VirtualAddress);
	for (; imp->Name; ++imp)
	{
		LPCSTR dllName = (LPCSTR)(base + imp->Name);
		if (_stricmp(dllName, pszImportDll) != 0) continue;
		PIMAGE_THUNK_DATA oft = (PIMAGE_THUNK_DATA)(base + imp->OriginalFirstThunk);
		PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
		if (!oft) oft = ft;
		for (; ft->u1.Ordinal || oft->u1.Ordinal; ++oft, ++ft)
		{
			if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
			PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)(base + oft->u1.AddressOfData);
			if (ibn && _stricmp((LPCSTR)ibn->Name, pszFunc) == 0)
			{
				DWORD oldProt = 0;
				if (VirtualProtect(&ft->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &oldProt))
				{
					if (ppOrig) *ppOrig = (PVOID)ft->u1.Function;
					ft->u1.Function = (ULONG_PTR)pNew;
					VirtualProtect(&ft->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
					FlushInstructionCache(GetCurrentProcess(), &ft->u1.Function, sizeof(ULONG_PTR));
					// 记录 IAT 槽以便卸载时还原
					LdmIatPatch rec;
					rec.entry = &ft->u1.Function;
					rec.orig = (ULONG_PTR)pNew; // 占位,下面用真实原值覆盖
					rec.orig = (ULONG_PTR)(ppOrig ? *ppOrig : 0);
					g_LdmIatPatches.push_back(rec);
					return TRUE;
				}
				return FALSE;
			}
		}
	}
	return FALSE;
}

static BOOL VDrawReplacementImage(HDC hdc, int x, int y, int width, int height)
{
	ReplaceState::Frame frame;
	if (!g_frames.Acquire(frame)) return FALSE;

	// 绘制与 stretch mode 的保存/恢复都在 codec 里：目标 DC 是**调用者的**，
	// 改了状态不还原的症状是"某程序截图之后界面变色"，极难归因。
	const BOOL rendered = ReplaceCodec::PaintDib(hdc, x, y, width, height,
		frame.dib, frame.width, frame.height, frame.dibStride) ? TRUE : FALSE;

	g_frames.Release();
	return rendered;
}

// 增量捕获只更新 dirty rect，但 H.264 编码读取的是同一张持久位图。命中捕获后必须
// 重绘目标 DC 当前选中的整张 bitmap，否则未变化区域会继续保留上一次的桌面像素。
// 配置了 VideoModifyImage 时铺满自定义图片；图片不可用或绘制失败时回退纯黑。
static BOOL VRenderSelectedCaptureBitmap(HDC hdc, int fallbackX, int fallbackY,
	                                     int fallbackWidth, int fallbackHeight,
	                                     LONG* bitmapWidth, LONG* bitmapHeight,
	                                     bool* usedCustomImage)
{
	if (bitmapWidth) *bitmapWidth = 0;
	if (bitmapHeight) *bitmapHeight = 0;
	if (usedCustomImage) *usedCustomImage = false;

	HGDIOBJ selectedBitmap = GetCurrentObject(hdc, OBJ_BITMAP);
	BITMAP bitmapInfo;
	ZeroMemory(&bitmapInfo, sizeof(bitmapInfo));
	if (selectedBitmap &&
		GetObjectW(selectedBitmap, sizeof(bitmapInfo), &bitmapInfo) == sizeof(bitmapInfo) &&
		bitmapInfo.bmWidth > 0 && bitmapInfo.bmHeight != 0 &&
		bitmapInfo.bmWidth < 8192 && bitmapInfo.bmHeight > -8192 && bitmapInfo.bmHeight < 8192)
	{
		LONG height = bitmapInfo.bmHeight < 0 ? -bitmapInfo.bmHeight : bitmapInfo.bmHeight;
		if (bitmapWidth) *bitmapWidth = bitmapInfo.bmWidth;
		if (bitmapHeight) *bitmapHeight = height;
		if (VDrawReplacementImage(hdc, 0, 0, bitmapInfo.bmWidth, height)) {
			if (usedCustomImage) *usedCustomImage = true;
			return TRUE;
		}
		return PatBlt(hdc, 0, 0, bitmapInfo.bmWidth, height, BLACKNESS);
	}

	if (VDrawReplacementImage(hdc, fallbackX, fallbackY, fallbackWidth, fallbackHeight)) {
		if (usedCustomImage) *usedCustomImage = true;
		return TRUE;
	}
	return PatBlt(hdc, fallbackX, fallbackY, fallbackWidth, fallbackHeight, BLACKNESS);
}

// BitBlt 的 CAPTUREBLT/NOMIRRORBITMAP 等高位标志不改变基础 ROP。
static bool VIsScreenCopyRop(DWORD rop)
{
	return (rop & 0x00FFFFFFu) == SRCCOPY;
}

// 捕获特征:目标是内存 DC、源是窗口 DC、SRCCOPY => 屏幕正被抓进内存位图。
// 显示路径(dest 为窗口 DC)与 mem->mem 合成均不会命中。
static BOOL WINAPI hkLdmBitBlt(HDC hdc, int x, int y, int cx, int cy,
                               HDC hdcSrc, int sx, int sy, DWORD rop)
{
	HookEngine::DetourScope detourScope;
	if (InterlockedCompareExchange(&g_ldmCaptureReent, 1, 0) == 1)
		return g_origLdmBitBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, rop); // 重入透传
	BOOL r = g_origLdmBitBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, rop);
	if (g_LdmCaptureHooked && VIsScreenCopyRop(rop) &&
		GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC)
	{
		tlsCaptureBackingDC = hdc;
		tlsCapturePending = true;
		tlsCaptureX = x;
		tlsCaptureY = y;
		tlsCaptureWidth = cx;
		tlsCaptureHeight = cy;
		LONG bitmapWidth = 0, bitmapHeight = 0;
		bool usedCustomImage = false;
		VRenderSelectedCaptureBitmap(hdc, x, y, cx, cy, &bitmapWidth, &bitmapHeight, &usedCustomImage);
		LONG c = InterlockedIncrement(&g_ldmCaptureLogCount);
		if (c <= 30)
			VDebugLog(L"LdmCapture BitBlt backing bitmap rendered mode=%s bitmapW=%d bitmapH=%d dirtyX=%d dirtyY=%d dirtyW=%d dirtyH=%d srcDC=%p dstDC=%p",
			          usedCustomImage ? L"image" : L"black", bitmapWidth, bitmapHeight,
			          x, y, cx, cy, hdcSrc, hdc);
	}
	InterlockedExchange(&g_ldmCaptureReent, 0);
	return r;
}

static BOOL WINAPI hkLdmStretchBlt(HDC hdc, int x, int y, int cx, int cy,
                                   HDC hdcSrc, int sx, int sy, int cw, int ch, DWORD rop)
{
	HookEngine::DetourScope detourScope;
	if (InterlockedCompareExchange(&g_ldmCaptureReent, 1, 0) == 1)
		return g_origLdmStretchBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, cw, ch, rop);
	BOOL r = g_origLdmStretchBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, cw, ch, rop);
	if (g_LdmCaptureHooked && VIsScreenCopyRop(rop) &&
		GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC)
	{
		tlsCaptureBackingDC = hdc;
		tlsCapturePending = true;
		tlsCaptureX = x;
		tlsCaptureY = y;
		tlsCaptureWidth = cx;
		tlsCaptureHeight = cy;
		LONG bitmapWidth = 0, bitmapHeight = 0;
		bool usedCustomImage = false;
		VRenderSelectedCaptureBitmap(hdc, x, y, cx, cy, &bitmapWidth, &bitmapHeight, &usedCustomImage);
		LONG c = InterlockedIncrement(&g_ldmCaptureLogCount);
		if (c <= 30)
			VDebugLog(L"LdmCapture StretchBlt backing bitmap rendered mode=%s bitmapW=%d bitmapH=%d dirtyX=%d dirtyY=%d dirtyW=%d dirtyH=%d srcDC=%p dstDC=%p",
			          usedCustomImage ? L"image" : L"black", bitmapWidth, bitmapHeight,
			          x, y, cx, cy, hdcSrc, hdc);
	}
	InterlockedExchange(&g_ldmCaptureReent, 0);
	return r;
}

// 幂等装入:仅当"视频流修改"(m_EnableModifyVideoStream)时生效(对应新方案)。
// LibDeskMonitor 为静态导入,进程启动即已加载;若此时尚未就绪,看门狗线程会再次尝试。
static void VEnsureLibDeskMonitorCaptureHook()
{
	// "打开看"GDI 抓屏拦截:仅在视频流修改模式下生效(禁止监看走稳定版方案,不在此挂钩)。
	if (!g_VideoProps.m_EnableModifyVideoStream) return;
	if (!g_LdmCaptureHooked) {
		HMODULE hLDM = GetModuleHandle(L"LibDeskMonitor.dll");
		if (hLDM) {
			BOOL okB = VPatchLdmOne(hLDM, "GDI32.dll", "BitBlt", (PVOID)hkLdmBitBlt, (PVOID*)&g_origLdmBitBlt);
			BOOL okS = VPatchLdmOne(hLDM, "GDI32.dll", "StretchBlt", (PVOID)hkLdmStretchBlt, (PVOID*)&g_origLdmStretchBlt);
			VDebugLog(L"VEnsureLibDeskMonitorCaptureHook BitBlt=%d StretchBlt=%d hMod=%p", okB, okS, hLDM);
			if (okB || okS)
				g_LdmCaptureHooked = true;
		} else {
			VDebugLog(L"VEnsureLibDeskMonitorCaptureHook: LibDeskMonitor.dll not loaded yet, will retry");
		}
	}

	// libTDDesk2 的捕获函数在最终 GdiFlush 返回后才完成 backing bitmap 提交。
	// 这个钩必须独立重试，不能被上面的 LibDeskMonitor IAT 状态短路。
	if (!g_Desk2GdiFlushHooked) {
		HMODULE hDesk2 = GetModuleHandle(L"libTDDesk2.dll");
		if (hDesk2) {
			BOOL okF = VPatchLdmOne(hDesk2, "GDI32.dll", "GdiFlush",
			                        (PVOID)hkDesk2GdiFlush, (PVOID*)&g_origDesk2GdiFlush);
			VDebugLog(L"VEnsureLibDeskMonitorCaptureHook GdiFlush=%d hMod=%p", okF, hDesk2);
			if (okF)
				g_Desk2GdiFlushHooked = true;
		} else {
			VDebugLog(L"VEnsureLibDeskMonitorCaptureHook: libTDDesk2.dll not loaded yet, will retry");
		}
	}
}

static BOOL WINAPI hkDesk2GdiFlush(void)
{
	HookEngine::DetourScope detourScope;
	BOOL result = g_origDesk2GdiFlush ? g_origDesk2GdiFlush() : TRUE;
	if (!tlsCapturePending || g_Unloading || !g_Desk2GdiFlushHooked)
		return result;

	HDC backing = tlsCaptureBackingDC;
	int x = tlsCaptureX, y = tlsCaptureY;
	int width = tlsCaptureWidth, height = tlsCaptureHeight;
	tlsCapturePending = false;
	tlsCaptureBackingDC = NULL;

	if (backing) {
		LONG bitmapWidth = 0, bitmapHeight = 0;
		bool usedCustomImage = false;
		bool previousBitBltHookActive = bitBltHookActive;
		bitBltHookActive = true;
		BOOL rendered = VRenderSelectedCaptureBitmap(backing, x, y, width, height,
		                                             &bitmapWidth, &bitmapHeight,
		                                             &usedCustomImage);
		if (g_origDesk2GdiFlush)
			g_origDesk2GdiFlush();
		bitBltHookActive = previousBitBltHookActive;
		static volatile LONG s_finalFlushLogCount = 0;
		LONG logIndex = InterlockedIncrement(&s_finalFlushLogCount);
		if (logIndex <= 40 || (logIndex % 2000 == 0))
			VDebugLog(L"libTDDesk2 GdiFlush final backing bitmap rendered mode=%s bitmapW=%d bitmapH=%d result=%d",
			          usedCustomImage ? L"image" : L"black", bitmapWidth, bitmapHeight, (int)rendered);
	}
	return result;
}

// ---- BitBlt 截屏拦截 ----

BOOL WINAPI Hook_BitBlt(HDC hdc, int x, int y, int cx, int cy,
                        HDC hdcSrc, int x1, int y1, DWORD rop)
{
	HookEngine::DetourScope detourScope;
	if (!fpBitBlt)
		return FALSE;
	// 线程内重入保护:同一线程递归调用 BitBlt 直接透传,避免自嵌套死循环。
	if (bitBltHookActive)
		return fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

	// 快速路径:未启用视频流修改,或正在卸载 —— 纯透传,不做任何额外 GDI 调用,
	// 把对进程内其它 BitBlt(UI 绘制等)的影响降到最低,避免无谓开销与潜在卡死。
	// 当前仓库从未调用 VSetVideoStreamModify 提供替换帧,因此外部替换画面恒为空,
	// 走的就是这条快速路径:钩子存在但行为等同透传,绝不会卡死。
	if (g_Unloading || !g_VideoProps.m_EnableModifyVideoStream)
		return fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

	// 视频流修改:两种模式
	//  (a) 替换帧模式:有外部替换画面且与捕获尺寸匹配 → 用替换帧 BitBlt(原逻辑)
	//  (b) 图片/纯黑模式:无替换画面 → 自定义图片铺满目标捕获位图，
	//      未配置或绘制失败时回退纯黑。
	//      防破坏学生端 UI:仅当捕获辅助进程(DispcapHelper,基本只做屏幕捕获)对所有
	//      BitBlt 刷黑;学生端主进程(StudentMain)只对与捕获尺寸匹配的 BitBlt 刷黑。
	HDC replacement = NULL;
	bool blackFill = false;
	bool hasSurface = false;
	// 借用判定（"尺寸匹配 + 源是窗口/屏幕 DC"）与"有没有外部画面"必须在同一次取锁内完成：
	// 分开判会出现"借不到 + 不知道要不要刷黑"的错配。
	replacement = g_surface.Borrow(cx, cy, GetObjectType(hdcSrc) == OBJ_DC, &hasSurface);
	if (!replacement && !hasSurface) {
		if (g_customHost) {
			// 通用宿主没有「捕获辅助进程 / 学生端主进程」这层身份信息，不能像极域那样
			// 一刀切刷黑 —— 那会把宿主自己的界面绘制（窗口 DC ← 内存 DC）也一起盖掉。
			// 只认一种形状：**屏幕 DC → 内存 DC 的整块 SRCCOPY**。
			// 判据与 hk42/43/44 共用 GuardWindow::IsScreenCaptureInto：屏幕 DC 既认
			// WindowFromDC == 桌面的窗口 DC，也认 CreateDCW(L"DISPLAY") 出来的显示 DC
			// （后者 WindowFromDC 返回 NULL，只看窗口会把它当内存 DC 放行）。
			// 所以宿主自己的双缓冲、mem→mem 合成、截图之后的 mem→窗口回贴全部放行。
			// ★ 不能用 GetObjectType(hdcSrc) == OBJ_DC 代替：那只说明源是个 DC，
			//   宿主自己的窗口 DC 也满足，替换图会跟着窗口一起被画到屏幕上。
			blackFill = VIsScreenCopyRop(rop) &&
			            GetObjectType(hdc) == OBJ_MEMDC &&
			            GuardWindow::IsScreenCaptureInto(hdcSrc, hdc);
			static volatile LONG s_customCaptureLogCount = 0;
			if (blackFill && InterlockedIncrement(&s_customCaptureLogCount) <= 20)
				VDebugLog(L"Hook_BitBlt custom-host capture cx=%d cy=%d rop=%08X srcDC=%p dstDC=%p",
				          cx, cy, rop, hdcSrc, hdc);
		}
		else if (!captureHelperForwarding)
			blackFill = true;
		else {
			// StudentMain: 仅刷黑"屏幕/窗口 → 内存位图"的捕获,避免误伤 UI(mem→窗口 绘制,目标不是 MEMDC)。
			// 整屏尺寸(视频流预览)与窗口尺寸(打开看:LibDeskMonitor 把源窗口 DC BitBlt 进内存位图)均命中。
			bool isFullScreen = (cx == g_VideoProps.m_Width && cy == g_VideoProps.m_Height);
			if (isFullScreen)
				blackFill = true;
			else if (VIsScreenCopyRop(rop) &&
			         GetObjectType(hdc) == OBJ_MEMDC &&
			         GetObjectType(hdcSrc) == OBJ_DC) {
				blackFill = true;
				VDebugLog(L"Hook_BitBlt blackFill cx=%d cy=%d rop=%08X srcDC=%p dstDC=%p screenToMem=1",
				          cx, cy, rop, hdcSrc, hdc);
			}
		}
	}

	BOOL result;
	if (replacement)
		result = fpBitBlt(hdc, x, y, cx, cy, replacement, x1, y1, rop);
	else if (blackFill) {
		if (GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC &&
		    VIsScreenCopyRop(rop)) {
			tlsCaptureBackingDC = hdc;
			tlsCapturePending = true;
			tlsCaptureX = x;
			tlsCaptureY = y;
			tlsCaptureWidth = cx;
			tlsCaptureHeight = cy;
		}
		LONG bitmapWidth = 0, bitmapHeight = 0;
		bool usedCustomImage = false;
		result = VRenderSelectedCaptureBitmap(hdc, x, y, cx, cy, &bitmapWidth, &bitmapHeight, &usedCustomImage);
		static volatile LONG s_backingBitmapLogCount = 0;
		LONG logIndex = InterlockedIncrement(&s_backingBitmapLogCount);
		if (logIndex <= 40 || (logIndex % 2000 == 0))
			VDebugLog(L"Hook_BitBlt backing bitmap rendered mode=%s bitmapW=%d bitmapH=%d dirtyX=%d dirtyY=%d dirtyW=%d dirtyH=%d srcDC=%p dstDC=%p result=%d",
			          usedCustomImage ? L"image" : L"black", bitmapWidth, bitmapHeight,
			          x, y, cx, cy, hdcSrc, hdc, (int)result);
	}
	else
		result = fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

	if (replacement) {
		// 用完了:归还引用;若归零且有待删除的旧 DC,由 Return 内部安全删除
		// (引用计数归零意味着已没有任何线程正在使用该 DC)。
		g_surface.Return();
	}
	return result;
}

void VSetVideoStreamModify(int width, int height, HDC hdcMem, bool enable)
{
	g_VideoProps.m_Width = width;
	g_VideoProps.m_Height = height;
	g_VideoProps.m_EnableModifyVideoStream = enable;

	// 停用或没给 DC ⇒ 清空；给出 DC ⇒ 换新（旧的安全延迟删除）。
	// ★ 与原实现的差异：旧代码在 hdcMem 与当前 DC **相同**时反而置 NULL
	//   （`hdcMem != old` 条件写反了方向），重复设置同一个 DC 会把替换画面丢掉。
	//   这条路径在本仓库从未被调用（主程序不调 VSetVideoStreamModify），
	//   这里按正确语义实现：相同即保持不变。
	g_surface.Set(width, height, (enable && hdcMem) ? hdcMem : NULL);
}

void VSetPulseWindow(HWND hWnd)
{
	g_VideoProps.m_hPulseWnd = hWnd;
}
