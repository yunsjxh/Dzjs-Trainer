
// JiYuKillerVirus.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "JiYuTrainerHooks.h"
#include "../WindowCaptureProtection.h"
#include "resource.h"
#include "StringHlp.h"
#include "StringSplit.h"
#include "mhook-lib/mhook.h"
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

#define INVALID_HHOOK_MOUSE 0x0001
#define INVALID_HHOOK_KEYBOARD 0x0002

using namespace std;

extern HINSTANCE hInst;

WNDPROC jiYuWndProc;
WNDPROC jiYuTDDeskWndProc;
list<HWND>  jiYuWnds;
list<HWND>  jiYuWndCanSize;
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
bool whitePreviewOnly = false;
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

HDC g_HdcMem = NULL;
// 视频流替换帧的同步原语:避免 VSetVideoStreamModify 在捕获线程正用 g_HdcMem 调 fpBitBlt 时
// 调用 DeleteDC 造成 GDI 竞争/卡死。用临界区 + 引用计数保证"正在使用的 DC 不会被删除"。
CRITICAL_SECTION g_HdcMemCs;
bool g_HdcMemCsInit = false;
volatile LONG g_HdcMemRefs = 0;
std::vector<HDC> g_HdcMemPending;   // 引用计数归零前不能立即删除的旧 DC
bool g_Unloading = false;

// 视频流自定义图片替换(JPEG 预览与 H.264 backing bitmap 共用的像素来源)
BYTE* g_ImgData = NULL;           // RGB24(R,G,B) 缓存,行无对齐(stride=w*3)
BYTE* g_ImgDibData = NULL;        // BGR24 顶向下 DIB 缓存,供 StretchDIBits 直接绘制
int g_ImgW = 0, g_ImgH = 0;
int g_ImgDibStride = 0;
volatile LONG g_ImgRefs = 0;
CRITICAL_SECTION g_ImgCs;
bool g_ImgCsInit = false;
volatile bool g_ImgValid = false;
std::vector<BYTE*> g_ImgPendingBuffers; // 引用计数归零前不能立即释放的旧缓冲

// MP4/WMV 等视频替换由独立 Media Foundation 线程解码。捕获热路径只读取
// 已发布的 RGB24/BGR24 当前帧，不执行文件 I/O 或视频解码。
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
			wcscpy_s(fileName + 1, MAX_PATH - (fileName + 1 - debugLogPath), L"JiYuTrainerHooks-debug.log");
		else
			wcscpy_s(debugLogPath, L"JiYuTrainerHooks-debug.log");
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

BOOL VSetHookLogged(LPCWSTR name, PVOID* original, PVOID replacement)
{
	VDebugLog(L"Mhook begin name=%s target=%p replacement=%p", name, original ? *original : NULL, replacement);
	BOOL result = Mhook_SetHook(original, replacement);
	VDebugLog(L"Mhook end name=%s result=%d trampoline=%p", name, result, original ? *original : NULL);
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
		jiYuWnds.clear();
		jiYuWndCanSize.clear();
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
		else if (name == L"JiYuTrainerHooks.dll") {//Current is virus main dll
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

	loaded = true;
	VDebugLog(L"VLoad end loaded=1");
}
void VRunMain() {
	VDebugLog(L"VRunMain begin");
	desktopWindow = GetDesktopWindow();

	GetModuleFileNameW(hInst, mainIniPath, MAX_PATH);
	WCHAR* iniName = wcsrchr(mainIniPath, L'\\');
	if (iniName)
		wcscpy_s(iniName + 1, MAX_PATH - (iniName + 1 - mainIniPath), L"JiYuTrainer.ini");
	if (PathFileExistsW(mainIniPath))
		VInitSettings();
	whitePreviewOnly = !allowMonitor;
	VDebugLog(L"VRunMain settings path=%s allowMonitor=%d whitePreviewOnly=%d allowAllRunOp=%d videoModify=%d",
		mainIniPath, allowMonitor, whitePreviewOnly, allowAllRunOp, g_VideoProps.m_EnableModifyVideoStream);

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
	VSendMessageBack(L"hkb:succ", hWndMsgCenter);
	PostMessage(hWndMsgCenter, WM_HOOK_INITIALIZED, 0, 0);
	HANDLE handshakeThread = CreateThread(NULL, 0, VHandshakeRetryThread, NULL, 0, NULL);
	if (handshakeThread)
		CloseHandle(handshakeThread);
	else
		VDebugLog(L"handshake retry thread create failed error=%lu", GetLastError());
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
		receiveWindow = FindWindow(NULL, L"JiYu Trainer Main Window");
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
	AppendMenu(hMenuGb, MF_STRING , IDM_HELP_GB, L"JiYuTrainer 添加的菜单");
	AppendMenu(hMenuGb, MF_STRING, IDM_HELP_GB2, L"JiYuTrainer 帮助");
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
	GetPrivateProfileString(L"JTSettings", L"AllowAllRunOp", L"FALSE", w, 32, mainIniPath);
	if (StrEqual(w, L"TRUE") || StrEqual(w, L"true") || StrEqual(w, L"1")) allowAllRunOp = true;
	else allowAllRunOp = false;
	GetPrivateProfileString(L"JTSettings", L"BandAllRunOp", L"TRUE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) bandAllRunOp = false;
	else bandAllRunOp = true;
	GetPrivateProfileString(L"JTSettings", L"ProhibitKillProcess", L"TRUE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) ProhibitKillProcess = false;
	else ProhibitKillProcess = true;
	GetPrivateProfileString(L"JTSettings", L"ProhibitCloseWindow", L"TRUE", w, 32, mainIniPath);
	if (!StrEqual(w, L"TRUE") && !StrEqual(w, L"true") && !StrEqual(w, L"1")) ProhibitCloseWindow = false;
	else ProhibitCloseWindow = true;
	
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
		char videoPathA[MAX_PATH] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyVideo", "", videoPathA, MAX_PATH, iniPathA);
		WCHAR videoPath[MAX_PATH] = { 0 };
		if (videoPathA[0] != '\0') {
			int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, videoPathA, -1, videoPath, MAX_PATH);
			if (n == 0)
				MultiByteToWideChar(CP_ACP, 0, videoPathA, -1, videoPath, MAX_PATH);
		}
		char loopValue[16] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyLoop", "TRUE", loopValue, 16, iniPathA);
		bool videoLoop = _stricmp(loopValue, "FALSE") != 0 && strcmp(loopValue, "0") != 0;
		char modeValue[16] = { 0 };
		GetPrivateProfileStringA("JTSettings", "VideoModifyMode", "video", modeValue, 16, iniPathA);
		bool useImage = _stricmp(modeValue, "image") == 0 || _stricmp(modeValue, "photo") == 0;
		g_VideoImagePath = imgPath;
		g_VideoPath = videoPath;
		g_VideoLoop = videoLoop;
		g_VideoMode = useImage ? L"image" : L"video";

		// 设置重载时先停止旧解码线程，再恢复静态图作为首帧/失败回退。
		VStopReplacementVideo();
		if (imgPath[0] != L'\0') VLoadReplacementImage(imgPath);
		else VUnloadReplacementImage();
		if (!useImage && videoPath[0] != L'\0')
			VStartReplacementVideo(videoPath, videoLoop);
	}
	if (g_HdcMem) { DeleteDC(g_HdcMem); g_HdcMem = NULL; }
	GetPrivateProfileString(L"JTSettings", L"AllowControl", L"FALSE", w, 32, mainIniPath);
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
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
				PostMessageW(hWndMsgCenter, WM_COMMAND, IDC_SW_STATUS_RB_FAIL_FLASH, 0);
			}
			else if (arr[1] == L"setmonitor" && arr.size() >= 3) {
				// 显式设置 allowMonitor，直接驱动 live 全局，不被 ini 默认值覆盖
				allowMonitor = (arr[2] == L"1" || arr[2] == L"TRUE" || arr[2] == L"true");
				whitePreviewOnly = !allowMonitor;
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideo" && arr.size() >= 3) {
				// 显式设置视频流保护，直接驱动 live 全局
				bool en = (arr[2] == L"1" || arr[2] == L"TRUE" || arr[2] == L"true");
				g_VideoProps.m_Width = GetSystemMetrics(SM_CXSCREEN);
				g_VideoProps.m_Height = GetSystemMetrics(SM_CYSCREEN);
				g_VideoProps.m_EnableModifyVideoStream = en;
				VSendMessageBack(L"hkb:succ", hWndMsgCenter);
			}
			else if (arr[1] == L"setvideomode" && arr.size() >= 3) {
				const bool useImage = arr[2] == L"image" || arr[2] == L"photo";
				g_VideoMode = useImage ? L"image" : L"video";
				VStopReplacementVideo();
				if (useImage) {
					if (!g_VideoImagePath.empty()) VLoadReplacementImage(g_VideoImagePath.c_str());
				}
				else if (!g_VideoPath.empty()) {
					VStartReplacementVideo(g_VideoPath.c_str(), g_VideoLoop);
				}
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
			jiYuWnds.push_back(hWnd);
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
			jiYuWnds.push_back(hWnd);
			//jiYuWndCanSize.push_back(hWnd);
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
	WNDPROC oldWndProc = (WNDPROC)GetWindowLong(hWnd, GWL_WNDPROC);
	if (oldWndProc != (WNDPROC)JiYuWndProc) {
		jiYuWndProc = (WNDPROC)oldWndProc;
		SetWindowLong(hWnd, GWL_WNDPROC, (LONG)JiYuWndProc);
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
	list<HWND>::iterator testiterator;
	for (testiterator = jiYuWnds.begin(); testiterator != jiYuWnds.end(); testiterator++)
	{
		if ((*testiterator) == hWnd)
			return true;
	}
	return false;
}
bool VIsInIllegalCanSizeWindows(HWND hWnd) {
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
	HWND receiveWindow = NULL;
	if (tlsCommandSender && IsWindow(tlsCommandSender))
		receiveWindow = tlsCommandSender;
	if (!receiveWindow)
		receiveWindow = FindWindow(NULL, L"JiYu Trainer Main Window");
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
			else if (hWndOutOfControlConformRs == IDC_RESTART)
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
	jiYuWnds.clear();
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
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));

		SetDlgItemText(hDlg, IDC_EDIT_OP_PATH, currOpCfPath);
		SetDlgItemText(hDlg, IDC_EDIT_OP_PARARM, currOpCfPararm);

		SetWindowLong(GetDlgItem(hDlg, IDNO), GWL_USERDATA, (LPARAM)30);
		SetTimer(hDlg, TIMER_COUNTDOWN_TICK, 1000, NULL);

		lResult = TRUE;
		break;
	}
	case WM_TIMER: {
		if (wParam == TIMER_COUNTDOWN_TICK) {
			int w = GetWindowLong(GetDlgItem(hDlg, IDNO), GWL_USERDATA);
			if (w > 0) {
				WCHAR str[18]; swprintf_s(str, L"拒绝 (%d)", w); 
				SetWindowLong(GetDlgItem(hDlg, IDNO), GWL_USERDATA, (LPARAM)(w - 1));
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
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICONWARN)));

		SetDlgItemText(hDlg, IDC_EDIT_OP_PATH, currOpCfPath);
		SetDlgItemText(hDlg, IDC_EDIT_OP_PARARM, currOpCfPararm);

		SetWindowLong(GetDlgItem(hDlg, IDC_RESTART), GWL_USERDATA, (LPARAM)7);
		SetTimer(hDlg, TIMER_COUNTDOWN_TICK, 1000, NULL);

		lResult = TRUE;
		break;
	}
	case WM_TIMER: {
		if (wParam == TIMER_COUNTDOWN_TICK) {
			int w = GetWindowLong(GetDlgItem(hDlg, IDC_RESTART), GWL_USERDATA);
			if (w > 0) {
				WCHAR str[18]; swprintf_s(str, L"重启主程序 (%d)", w);
				SetWindowLong(GetDlgItem(hDlg, IDC_RESTART), GWL_USERDATA, (LPARAM)(w - 1));
				SetDlgItemText(hDlg, IDC_RESTART, str);
			}
			else SendMessage(hDlg, WM_COMMAND, IDC_RESTART, NULL);
		}
		break;
	}
	case WM_COMMAND: {
		if (wParam == IDC_RESTART || wParam == IDNO || wParam == IDC_KILL) {
			
			hWndOutOfControlConformRs = wParam;
			lResult = wParam; 
			DestroyWindow(hDlg);
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

// 接管新帧缓冲的所有权，并原子替换 JPEG/H.264 两条路径共同读取的当前帧。
static bool VPublishReplacementFrame(BYTE* rgbData, BYTE* dibData,
	                                 int width, int height, int dibStride)
{
	if (!rgbData || !dibData || width <= 0 || height <= 0 || dibStride < width * 3) {
		delete[] rgbData;
		delete[] dibData;
		return false;
	}
	if (!g_ImgCsInit) { InitializeCriticalSection(&g_ImgCs); g_ImgCsInit = true; }

	EnterCriticalSection(&g_ImgCs);
	if (g_ImgValid) {
		if (g_ImgRefs == 0) {
			delete[] g_ImgData;
			delete[] g_ImgDibData;
		}
		else {
			if (g_ImgData) g_ImgPendingBuffers.push_back(g_ImgData);
			if (g_ImgDibData) g_ImgPendingBuffers.push_back(g_ImgDibData);
		}
	}
	g_ImgData = rgbData;
	g_ImgDibData = dibData;
	g_ImgW = width;
	g_ImgH = height;
	g_ImgDibStride = dibStride;
	g_ImgValid = true;
	LeaveCriticalSection(&g_ImgCs);
	return true;
}

void VUnloadReplacementImage()
{
	if (!g_ImgCsInit) return;
	EnterCriticalSection(&g_ImgCs);
	if (g_ImgValid) {
		if (g_ImgRefs == 0) {
			delete[] g_ImgData;
			delete[] g_ImgDibData;
		}
		else {
			if (g_ImgData) g_ImgPendingBuffers.push_back(g_ImgData);
			if (g_ImgDibData) g_ImgPendingBuffers.push_back(g_ImgDibData);
		}
		g_ImgValid = false;
		g_ImgData = NULL; g_ImgDibData = NULL;
		g_ImgW = 0; g_ImgH = 0; g_ImgDibStride = 0;
	}
	if (g_ImgRefs == 0 && !g_ImgPendingBuffers.empty()) {
		for (size_t i = 0; i < g_ImgPendingBuffers.size(); ++i) delete[] g_ImgPendingBuffers[i];
		g_ImgPendingBuffers.clear();
	}
	LeaveCriticalSection(&g_ImgCs);
}

bool VLoadReplacementImage(LPCWSTR path)
{
	if (!g_ImgCsInit) { InitializeCriticalSection(&g_ImgCs); g_ImgCsInit = true; }
	if (!path || path[0] == L'\0') { VUnloadReplacementImage(); return false; }
	BYTE* buf = NULL; int w = 0, h = 0;
	if (!GDIPlusDecodeToRGB24(path, buf, w, h)) {
		VDebugLog(L"VideoModifyImage decode failed path=%s", path);
		return false;
	}
	int dibStride = (w * 3 + 3) & ~3;
	BYTE* dibBuf = new (std::nothrow) BYTE[(size_t)dibStride * h];
	if (!dibBuf) {
		delete[] buf;
		VDebugLog(L"VideoModifyImage DIB allocation failed path=%s w=%d h=%d", path, w, h);
		return false;
	}
	ZeroMemory(dibBuf, (size_t)dibStride * h);
	for (int y = 0; y < h; ++y) {
		const BYTE* src = buf + (size_t)y * w * 3;
		BYTE* dst = dibBuf + (size_t)y * dibStride;
		for (int x = 0; x < w; ++x) {
			dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
			src += 3; dst += 3;
		}
	}
	if (!VPublishReplacementFrame(buf, dibBuf, w, h, dibStride))
		return false;
	VDebugLog(L"VideoModifyImage loaded path=%s w=%d h=%d dibStride=%d", path, w, h, dibStride);
	return true;
}

template <typename T>
static void VSafeRelease(T*& value)
{
	if (value) {
		value->Release();
		value = NULL;
	}
}

// Media Foundation 的 RGB32 样本通过 IMF2DBuffer::Lock2D 取得逻辑顶行与 pitch，
// 从而同时兼容 RGB DIB 的 bottom-up 和视频处理器输出的 top-down 布局。
static bool VConvertVideoSample(IMFSample* sample, int width, int height,
	                            BYTE*& rgbData, BYTE*& dibData, int& dibStride)
{
	rgbData = NULL;
	dibData = NULL;
	dibStride = 0;
	if (!sample || width <= 0 || height <= 0 || width > 16384 || height > 16384)
		return false;
	if ((size_t)width > SIZE_MAX / 3 ||
		(size_t)height > SIZE_MAX / ((size_t)width * 3))
		return false;

	IMFMediaBuffer* buffer = NULL;
	IMF2DBuffer* buffer2D = NULL;
	BYTE* scanline0 = NULL;
	BYTE* flatData = NULL;
	LONG pitch = 0;
	bool locked2D = false;
	bool bufferLocked = false;
	bool ok = false;

	DWORD bufferCount = 0;
	HRESULT hr = sample->GetBufferCount(&bufferCount);
	if (SUCCEEDED(hr) && bufferCount == 1)
		hr = sample->GetBufferByIndex(0, &buffer);
	else if (SUCCEEDED(hr))
		hr = sample->ConvertToContiguousBuffer(&buffer);
	if (SUCCEEDED(hr))
		hr = buffer->QueryInterface(IID_PPV_ARGS(&buffer2D));
	if (SUCCEEDED(hr) && buffer2D) {
		hr = buffer2D->Lock2D(&scanline0, &pitch);
		locked2D = SUCCEEDED(hr);
	}
	if (!locked2D && buffer) {
		DWORD maxLength = 0, currentLength = 0;
		hr = buffer->Lock(&flatData, &maxLength, &currentLength);
		bufferLocked = SUCCEEDED(hr);
		if (SUCCEEDED(hr) && currentLength >= (DWORD)((size_t)width * 4 * height)) {
			// 连续 RGB32 DIB 的常见布局为 bottom-up。
			scanline0 = flatData + (size_t)(height - 1) * width * 4;
			pitch = -(LONG)(width * 4);
		}
	}

	if (scanline0 && pitch != 0) {
		dibStride = (width * 3 + 3) & ~3;
		rgbData = new (std::nothrow) BYTE[(size_t)width * 3 * height];
		dibData = new (std::nothrow) BYTE[(size_t)dibStride * height];
		if (rgbData && dibData) {
			ZeroMemory(dibData, (size_t)dibStride * height);
			for (int y = 0; y < height; ++y) {
				// Lock2D returns the first scanline in memory. RGB32 commonly has a
				// negative pitch, so reverse the source row index and publish one
				// consistent top-down cache for both JPEG and GDI consumers.
				const BYTE* src = scanline0 + (ptrdiff_t)(height - 1 - y) * pitch;
				BYTE* rgb = rgbData + (size_t)y * width * 3;
				BYTE* dib = dibData + (size_t)y * dibStride;
				for (int x = 0; x < width; ++x) {
					// MFVideoFormat_RGB32 为 B,G,R,X。
					dib[0] = src[0]; dib[1] = src[1]; dib[2] = src[2];
					rgb[0] = src[2]; rgb[1] = src[1]; rgb[2] = src[0];
					src += 4; rgb += 3; dib += 3;
				}
			}
			ok = true;
		}
	}

	if (!ok) {
		delete[] rgbData;
		delete[] dibData;
		rgbData = NULL;
		dibData = NULL;
		dibStride = 0;
	}
	if (locked2D) buffer2D->Unlock2D();
	if (bufferLocked) buffer->Unlock();
	VSafeRelease(buffer2D);
	VSafeRelease(buffer);
	return ok;
}

static bool VGetVideoFrameSize(IMFSourceReader* reader, int& width, int& height)
{
	width = 0;
	height = 0;
	IMFMediaType* mediaType = NULL;
	UINT32 w = 0, h = 0;
	HRESULT hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mediaType);
	if (SUCCEEDED(hr))
		hr = MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &w, &h);
	VSafeRelease(mediaType);
	if (FAILED(hr) || w == 0 || h == 0 || w > 16384 || h > 16384)
		return false;
	width = (int)w;
	height = (int)h;
	return true;
}

static DWORD WINAPI VReplacementVideoThread(LPVOID parameter)
{
	ReplacementVideoContext* context = static_cast<ReplacementVideoContext*>(parameter);
	if (!context)
		return ERROR_INVALID_PARAMETER;

	HRESULT finalHr = S_OK;
	HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool comInitialized = SUCCEEDED(coHr);
	bool mfInitialized = false;
	IMFAttributes* attributes = NULL;
	IMFSourceReader* reader = NULL;
	IMFMediaType* outputType = NULL;
	LONG frameCount = 0;
	int width = 0, height = 0;
	LONGLONG firstTimestamp = -1;
	ULONGLONG playbackStart = 0;

	do {
		if (FAILED(coHr)) { finalHr = coHr; break; }
		finalHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
		if (FAILED(finalHr)) break;
		mfInitialized = true;

		finalHr = MFCreateAttributes(&attributes, 1);
		if (FAILED(finalHr)) break;
		attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
		finalHr = MFCreateSourceReaderFromURL(context->path.c_str(), attributes, &reader);
		if (FAILED(finalHr)) break;
		reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

		finalHr = MFCreateMediaType(&outputType);
		if (FAILED(finalHr)) break;
		outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		finalHr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outputType);
		if (FAILED(finalHr)) break;
		if (!VGetVideoFrameSize(reader, width, height)) {
			finalHr = MF_E_INVALIDMEDIATYPE;
			break;
		}
		VDebugLog(L"VideoModifyVideo opened path=%s w=%d h=%d loop=%d",
		          context->path.c_str(), width, height, context->loop ? 1 : 0);

		while (WaitForSingleObject(context->stopEvent, 0) == WAIT_TIMEOUT) {
			DWORD actualStream = 0, flags = 0;
			LONGLONG timestamp = 0;
			IMFSample* sample = NULL;
			finalHr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
			                             &actualStream, &flags, &timestamp, &sample);
			if (FAILED(finalHr)) {
				VSafeRelease(sample);
				break;
			}

			if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
				if (!VGetVideoFrameSize(reader, width, height)) {
					VSafeRelease(sample);
					finalHr = MF_E_INVALIDMEDIATYPE;
					break;
				}
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
				VSafeRelease(sample);
				if (!context->loop)
					break;
				PROPVARIANT position;
				PropVariantInit(&position);
				position.vt = VT_I8;
				position.hVal.QuadPart = 0;
				finalHr = reader->SetCurrentPosition(GUID_NULL, position);
				PropVariantClear(&position);
				if (FAILED(finalHr)) break;
				firstTimestamp = -1;
				playbackStart = 0;
				continue;
			}
			if (!sample)
				continue;

			if (firstTimestamp < 0) {
				firstTimestamp = timestamp;
				playbackStart = GetTickCount64();
			}
			LONGLONG relativeTime = timestamp - firstTimestamp;
			if (relativeTime < 0) relativeTime = 0;
			ULONGLONG dueTick = playbackStart + (ULONGLONG)(relativeTime / 10000);
			ULONGLONG now = GetTickCount64();
			if (dueTick > now) {
				ULONGLONG delay = dueTick - now;
				DWORD waitMs = delay > 0xFFFFFFFEULL ? 0xFFFFFFFEUL : (DWORD)delay;
				if (WaitForSingleObject(context->stopEvent, waitMs) != WAIT_TIMEOUT) {
					VSafeRelease(sample);
					break;
				}
			}

			BYTE* rgbData = NULL;
			BYTE* dibData = NULL;
			int dibStride = 0;
			if (VConvertVideoSample(sample, width, height, rgbData, dibData, dibStride) &&
				VPublishReplacementFrame(rgbData, dibData, width, height, dibStride)) {
				g_VideoFrameActive = true;
				// Desktop duplication is incremental. Mark the full output dirty for
				// every decoded video frame so a static desktop still advances video.
				for (int outputIndex = 0; outputIndex < 2; ++outputIndex)
					InterlockedExchange(&g_DispDXGIFullRefreshPending[outputIndex], 1);
				LONG index = InterlockedIncrement(&frameCount);
				if (index <= 3 || (index % 250 == 0))
					VDebugLog(L"VideoModifyVideo frame published index=%ld timestamp=%lld w=%d h=%d",
					          index, timestamp, width, height);
			}
			VSafeRelease(sample);
		}
	} while (false);

	VSafeRelease(outputType);
	VSafeRelease(reader);
	VSafeRelease(attributes);
	if (mfInitialized) MFShutdown();
	if (comInitialized) CoUninitialize();
	g_VideoFrameActive = false;
	VDebugLog(L"VideoModifyVideo thread end path=%s frames=%ld hr=0x%08X",
	          context->path.c_str(), frameCount, (DWORD)finalHr);
	delete context;
	return SUCCEEDED(finalHr) ? 0 : (DWORD)finalHr;
}

void VStopReplacementVideo()
{
	if (!g_VideoCsInit) return;
	EnterCriticalSection(&g_VideoCs);
	if (g_VideoThread) {
		if (g_VideoStopEvent) SetEvent(g_VideoStopEvent);
		WaitForSingleObject(g_VideoThread, INFINITE);
		CloseHandle(g_VideoThread);
		g_VideoThread = NULL;
	}
	if (g_VideoStopEvent) {
		CloseHandle(g_VideoStopEvent);
		g_VideoStopEvent = NULL;
	}
	g_VideoFrameActive = false;
	LeaveCriticalSection(&g_VideoCs);
}

bool VStartReplacementVideo(LPCWSTR path, bool loop)
{
	VStopReplacementVideo();
	if (!path || path[0] == L'\0' || !PathFileExistsW(path)) {
		VDebugLog(L"VideoModifyVideo file not found path=%s", path ? path : L"");
		return false;
	}
	if (!g_VideoCsInit) { InitializeCriticalSection(&g_VideoCs); g_VideoCsInit = true; }

	ReplacementVideoContext* context = new (std::nothrow) ReplacementVideoContext();
	if (!context) return false;
	context->path = path;
	context->loop = loop;
	context->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!context->stopEvent) {
		delete context;
		return false;
	}

	EnterCriticalSection(&g_VideoCs);
	g_VideoStopEvent = context->stopEvent;
	g_VideoThread = CreateThread(NULL, 0, VReplacementVideoThread, context, 0, NULL);
	if (!g_VideoThread) {
		CloseHandle(g_VideoStopEvent);
		g_VideoStopEvent = NULL;
		delete context;
		LeaveCriticalSection(&g_VideoCs);
		VDebugLog(L"VideoModifyVideo thread create failed error=%lu", GetLastError());
		return false;
	}
	LeaveCriticalSection(&g_VideoCs);
	VDebugLog(L"VideoModifyVideo thread started path=%s loop=%d", path, loop ? 1 : 0);
	return true;
}

// 线程安全读取替换图缓冲(引用计数防止在读期间被释放)
static bool VAcquireImage(BYTE*& data, BYTE*& dibData, int& w, int& h, int& dibStride)
{
	if (!g_ImgCsInit) return false;
	EnterCriticalSection(&g_ImgCs);
	if (g_ImgValid) {
		InterlockedIncrement(&g_ImgRefs);
		data = g_ImgData; dibData = g_ImgDibData;
		w = g_ImgW; h = g_ImgH; dibStride = g_ImgDibStride;
		LeaveCriticalSection(&g_ImgCs);
		return true;
	}
	LeaveCriticalSection(&g_ImgCs);
	return false;
}
static void VReleaseImage()
{
	if (!g_ImgCsInit) return;
	EnterCriticalSection(&g_ImgCs);
	LONG refs = InterlockedDecrement(&g_ImgRefs);
	if (refs == 0 && !g_ImgPendingBuffers.empty()) {
		for (size_t i = 0; i < g_ImgPendingBuffers.size(); ++i) delete[] g_ImgPendingBuffers[i];
		g_ImgPendingBuffers.clear();
	}
	LeaveCriticalSection(&g_ImgCs);
}

// JPEG 预览输入缓冲按 bottom-up 行序解释。将顶向下的替换图按反向源行写入，
// 避免编码后的缩略图上下颠倒；H.264 backing bitmap 使用独立的顶向下 DIB 路径。
static void OverlayImage(BYTE* dst, int dw, int dh, int dstride, const BYTE* src, int sw, int sh)
{
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
	for (int y = 0; y < dh; y++) {
		int sy = ((dh - 1 - y) * sh) / dh; if (sy >= sh) sy = sh - 1;
		const BYTE* srow = src + (size_t)sy * sw * 3;
		BYTE* drow = dst + (size_t)y * dstride;
		for (int x = 0; x < dw; x++) {
			int sx = (x * sw) / dw; if (sx >= sw) sx = sw - 1;
			const BYTE* s = srow + sx * 3;
			BYTE* d = drow + x * 3;
			d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
		}
	}
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
			// 不透明黑:B=G=R=0, A=0xFF。逐 DWORD 填充,覆盖 RGBA/BGRA 两种布局
			// (三色字节恒为 0,alpha 强制不透明),避免消费端按 alpha 合成时透出真实桌面。
			DWORD* px = (DWORD*)lpbyBits;
			SIZE_T total = (SIZE_T)w * (SIZE_T)h;
			for (SIZE_T i = 0; i < total; ++i) px[i] = 0xFF000000;
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
// 原因:DispcapHelper 常驻或由极域在 JiYuTrainer 注入 StudentMain 之前创建,或由其他进程创建,
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

void VInstallHooks(VirusMode mode) {
	VDebugLog(L"VInstallHooks begin mode=%d", mode);

	//Mhook_SetHook
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
	if (!g_HdcMemCsInit) { InitializeCriticalSection(&g_HdcMemCs); g_HdcMemCsInit = true; }

	if (hDwmApi) faDwmEnableComposition = (fnDwmEnableComposition)GetProcAddress(hDwmApi, "DwmEnableComposition");
	if (hFltLib) faFilterConnectCommunicationPort = (fnFilterConnectCommunicationPort)GetProcAddress(hFltLib, "FilterConnectCommunicationPort");

	//HMODULE hTDDesk2 = GetModuleHandle(L"libtddesk2.dll");
	//if (hTDDesk2) {
	//	faTDDeskCreateInstance = (fnTDDeskCreateInstance)GetProcAddress(hTDDesk2, "TDDeskCreateInstance");
	//}

	if (mode == VirusModeHook) {
		if (whitePreviewOnly || g_VideoProps.m_EnableModifyVideoStream)
		{
			VDebugLog(L"VInstallHooks minimal frame-replacement mode color=%s",
				whitePreviewOnly ? L"white" : L"black");
			VEnsureJPEGHook();
			if (captureHelperForwarding)
			{
				if (faCreateProcessW)
					hk22 = VSetHookLogged(L"CreateProcessW(capture-helper)", (PVOID*)&faCreateProcessW, (PVOID)hkCreateProcessW);
				if (faCreateProcessA)
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
		// 视频流"纯黑"模式接线:设捕获尺寸参考(g_HdcMem 保持 NULL → 触发纯黑)。
		// Hook_BitBlt 在捕获辅助进程(DispcapHelper)会对所有 BitBlt 刷黑,
		// 在学生端主进程(StudentMain)仅对与捕获尺寸匹配的 BitBlt 刷黑,以防破坏 UI。
		if (hk41 && g_VideoProps.m_EnableModifyVideoStream && !g_HdcMem) {
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
		if (captureHelperForwarding) {
			HANDLE hRI = CreateThread(NULL, 0, VRemoteInjectThread, NULL, 0, NULL);
			if (hRI) CloseHandle(hRI);
		}
		VDebugLog(L"VInstallHooks end mode=minimal hk28=%d hk27=%d hk26=%d hk33=%d hk23=%d hk41=%d", hk28, hk27, hk26, hk33, hk23, hk41);
		return;
		}

		//HMODULE hTDMaster = GetModuleHandle(L"libTDMaster.dll");
		//UnHookLocalInput = (fnUnHookLocalInput)GetProcAddress(hTDMaster, "UnHookLocalInput");
		//if (UnHookLocalInput) UnHookLocalInput();

		VOutPutStatus(L"[D] Install hook in mode VirusModeHook");

		hk1 = VSetHookLogged(L"SetWindowPos", (PVOID*)&raSetWindowPos, (PVOID)hkSetWindowPos);
		hk2 = VSetHookLogged(L"MoveWindow", (PVOID*)&raMoveWindow, (PVOID)hkMoveWindow);
		hk3 = VSetHookLogged(L"SetForegroundWindow", (PVOID*)&raSetForegroundWindow, (PVOID)hkSetForegroundWindow);
		hk4 = VSetHookLogged(L"BringWindowToTop", (PVOID*)&faBringWindowToTop, (PVOID)hkBringWindowToTop);
		hk5 = VSetHookLogged(L"DeviceIoControl", (PVOID*)&raDeviceIoControl, (PVOID)hkDeviceIoControl);
		hk6 = VSetHookLogged(L"CreateFileA", (PVOID*)&faCreateFileA, (PVOID)hkCreateFileA);
		hk7 = VSetHookLogged(L"CreateFileW", (PVOID*)&faCreateFileW, (PVOID)hkCreateFileW);
		hk8 = VSetHookLogged(L"SetWindowsHookExA", (PVOID*)&faSetWindowsHookExA, (PVOID)hkSetWindowsHookExA);
		hk9 = VSetHookLogged(L"DeferWindowPos", (PVOID*)&faDeferWindowPos, (PVOID)hkDeferWindowPos);
		hk10 = VSetHookLogged(L"SendInput", (PVOID*)&faSendInput, (PVOID)hkSendInput);
		hk11 = VSetHookLogged(L"mouse_event", (PVOID*)&famouse_event, (PVOID)hkmouse_event);

		//if(faTDDeskCreateInstance) 
		//	hk15 = Mhook_SetHook((PVOID*)&faTDDeskCreateInstance, hkTDDeskCreateInstance);

		hk16 = VSetHookLogged(L"SetWindowLongA", (PVOID*)&faSetWindowLongA, (PVOID)hkSetWindowLongA);
		hk17 = VSetHookLogged(L"SetWindowLongW", (PVOID*)&faSetWindowLongW, (PVOID)hkSetWindowLongW);
		hk18 = VSetHookLogged(L"ShowWindow", (PVOID*)&faShowWindow, (PVOID)hkShowWindow);
		hk19 = VSetHookLogged(L"ExitWindowsEx", (PVOID*)&faExitWindowsEx, (PVOID)hkExitWindowsEx);

		//hk20 = Mhook_SetHook((PVOID*)&faShellExecuteW, hkShellExecuteW);
		//if(faShellExecuteExW) hk21 = Mhook_SetHook((PVOID*)&faShellExecuteExW, hkShellExecuteExW);

		hk22 = VSetHookLogged(L"CreateProcessW", (PVOID*)&faCreateProcessW, (PVOID)hkCreateProcessW);

		if (faDwmEnableComposition) hk23 = VSetHookLogged(L"DwmEnableComposition", (PVOID*)&faDwmEnableComposition, (PVOID)hkDwmEnableComposition);

		hk24 = VSetHookLogged(L"WinExec", (PVOID*)&faWinExec, (PVOID)hkWinExec);
		hk25 = VSetHookLogged(L"CallNextHookEx", (PVOID*)&faCallNextHookEx, (PVOID)hkCallNextHookEx);
		hk26 = VSetHookLogged(L"GetWindowDC", (PVOID*)&faGetWindowDC, (PVOID)hkGetWindowDC);
		hk27 = VSetHookLogged(L"GetDesktopWindow", (PVOID*)&faGetDesktopWindow, (PVOID)hkGetDesktopWindow);

		VEnsureJPEGHook();
		hk29 = VSetHookLogged(L"UnhookWindowsHookEx", (PVOID*)&faUnhookWindowsHookEx, (PVOID)hkUnhookWindowsHookEx);

		hk30 = VSetHookLogged(L"CreateProcessA", (PVOID*)&faCreateProcessA, (PVOID)hkCreateProcessA);
		hk31 = VSetHookLogged(L"GetForegroundWindow", (PVOID*)&faGetForegroundWindow, (PVOID)hkGetForegroundWindow);

		hk33 = VSetHookLogged(L"CreateDCW", (PVOID*)&faCreateDCW, (PVOID)hkCreateDCW);
		hk34 = VSetHookLogged(L"EnableMenuItem", (PVOID*)&faEnableMenuItem, (PVOID)hkEnableMenuItem);

		hk35 = VSetHookLogged(L"SetClassLongA", (PVOID*)&faSetClassLongA, (PVOID)hkSetClassLongA);
		hk36 = VSetHookLogged(L"SetClassLongW", (PVOID*)&faSetClassLongW, (PVOID)hkSetClassLongW);

		hk37 = 0;
		VDebugLog(L"PostMessageW hook disabled to preserve StudentMain stream initialization");
		// Hooking SendMessageW also intercepts this DLL's dialog-control messages and can deadlock its UI thread.
		hk38 = 0;
		VDebugLog(L"SendMessageW hook disabled to keep status UI responsive");
		hk39 = VSetHookLogged(L"TerminateProcess", (PVOID*)&faTerminateProcess, (PVOID)hkTerminateProcess);

		if(faFilterConnectCommunicationPort) hk40 = VSetHookLogged(L"FilterConnectCommunicationPort", (PVOID*)&faFilterConnectCommunicationPort, (PVOID)hkFilterConnectCommunicationPort);

		hk41 = 0;
		VDebugLog(L"BitBlt hook disabled; video replacement uses EncodeToJPEGBuffer");

		VDebugLog(L"VInstallHooks end mode=hook hk28=%d", hk28);
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
void VUnInstallHooks() {
	g_Unloading = true;
	while (InterlockedCompareExchange(&g_DispDXGIInstallLock, 1, 0) != 0)
		Sleep(1);

	if (hk1) Mhook_Unhook((PVOID*)&raSetWindowPos);
	if (hk2) Mhook_Unhook((PVOID*)&raMoveWindow);
	if (hk3) Mhook_Unhook((PVOID*)&raSetForegroundWindow);
	if (hk4) Mhook_Unhook((PVOID*)&faBringWindowToTop);
	if (hk5) Mhook_Unhook((PVOID*)&raDeviceIoControl);
	if (hk6) Mhook_Unhook((PVOID*)&faCreateFileA);
	if (hk7) Mhook_Unhook((PVOID*)&faCreateFileW);
	if (hk8) Mhook_Unhook((PVOID*)&faSetWindowsHookExA);
	if (hk9) Mhook_Unhook((PVOID*)&faDeferWindowPos);
	if (hk10) Mhook_Unhook((PVOID*)&faSendInput);
	if (hk11) Mhook_Unhook((PVOID*)&famouse_event);
	//if (hk12) Mhook_Unhook((PVOID*)&faChangeDisplaySettingsW);
	//if (hk13) Mhook_Unhook((PVOID*)&faOpenDesktopA);
	//if (hk14) Mhook_Unhook((PVOID*)&faOpenInputDesktop);
	//if (hk15) Mhook_Unhook((PVOID*)&faTDDeskCreateInstance);
	if (hk16) Mhook_Unhook((PVOID*)&faSetWindowLongA);
	if (hk17) Mhook_Unhook((PVOID*)&faSetWindowLongW);
	if (hk18) Mhook_Unhook((PVOID*)&faShowWindow);

	if (hk19) Mhook_Unhook((PVOID*)&faExitWindowsEx);
	if (hk20) Mhook_Unhook((PVOID*)&faShellExecuteW);
	if (hk21) Mhook_Unhook((PVOID*)&faShellExecuteExW);
	if (hk22) Mhook_Unhook((PVOID*)&faCreateProcessW);
	if (hk23) Mhook_Unhook((PVOID*)&faDwmEnableComposition);
	if (hk24) Mhook_Unhook((PVOID*)&faWinExec);
	if (hk25) Mhook_Unhook((PVOID*)&faCallNextHookEx);
	if (hk26) Mhook_Unhook((PVOID*)&faGetWindowDC);
	if (hk27) Mhook_Unhook((PVOID*)&faGetDesktopWindow);
	// 三层 JPEG 钩卸载:先还原多模块 IAT 补丁,再 Mhook_Unhook 实现体/GPA 钩。
	if (hk28 || g_JpegIATHooked || g_JpegI422Hooked || g_JpegImplHooked || g_JpegI422ImplHooked || g_JpegGpaHooked) {
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
		if (g_JpegI422ImplHooked) { Mhook_Unhook((PVOID*)&faEncodeToJPEGBufferI422); g_JpegI422ImplHooked = false; }
		if (g_JpegImplHooked) { Mhook_Unhook((PVOID*)&faEncodeToJPEGBuffer); g_JpegImplHooked = false; }
		if (g_JpegGpaHooked) { Mhook_Unhook((PVOID*)&g_pfGetProcAddress); g_JpegGpaHooked = false; }
	if (hkLLW) { Mhook_Unhook((PVOID*)&g_pfLoadLibraryW); hkLLW = false; }
	if (hkLLX) { Mhook_Unhook((PVOID*)&g_pfLoadLibraryExW); hkLLX = false; }
		g_JpegIATHooked = false; g_JpegI422Hooked = false;
		hk28 = 0;
	}
	if (g_DispDXGIHooked) {
		Mhook_Unhook((PVOID*)&g_origDispDXGIBitBlt);
		g_DispDXGIHooked = false;
		g_origDispDXGIBitBlt = NULL;
	}
	if (g_DispDXGIGetScreenUpdateHooked) {
		Mhook_Unhook((PVOID*)&g_origDispDXGIGetScreenUpdate);
		g_DispDXGIGetScreenUpdateHooked = false;
		g_origDispDXGIGetScreenUpdate = NULL;
	}
	InterlockedExchange(&g_DispDXGIInstallLock, 0);
	InterlockedExchange(&g_DispDXGIRetryStarted, 0);
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

	if (hk30) Mhook_Unhook((PVOID*)&faCreateProcessA);
	if (hk31) Mhook_Unhook((PVOID*)&faGetForegroundWindow);

	if (hk33) Mhook_Unhook((PVOID*)&faCreateDCW);
	if (hk34) Mhook_Unhook((PVOID*)&faEnableMenuItem);
	if (hk35) Mhook_Unhook((PVOID*)&faSetClassLongA);
	if (hk36) Mhook_Unhook((PVOID*)&faSetClassLongW);
	if (hk37) Mhook_Unhook((PVOID*)&faPostMessageW);
	if (hk38) Mhook_Unhook((PVOID*)&faSendMessageW);
	if (hk39) Mhook_Unhook((PVOID*)&faTerminateProcess);
	if (hk40) Mhook_Unhook((PVOID*)&faFilterConnectCommunicationPort);
	if (hk41) { g_Unloading = true; Mhook_Unhook((PVOID*)&fpBitBlt); }
	if (g_HdcMemCsInit) {
		// g_Unloading 已置位,Hook_BitBlt 不会再进入替换路径,此处可安全清理。
		EnterCriticalSection(&g_HdcMemCs);
		if (g_HdcMem) { DeleteDC(g_HdcMem); g_HdcMem = NULL; }
		for (size_t i = 0; i < g_HdcMemPending.size(); i++) DeleteDC(g_HdcMemPending[i]);
		g_HdcMemPending.clear();
		LeaveCriticalSection(&g_HdcMemCs);
		DeleteCriticalSection(&g_HdcMemCs);
		g_HdcMemCsInit = false;
	}
	VStopReplacementVideo();
	VUnloadReplacementImage();
	if (g_VideoCsInit) { DeleteCriticalSection(&g_VideoCs); g_VideoCsInit = false; }
	if (g_ImgCsInit) { DeleteCriticalSection(&g_ImgCs); g_ImgCsInit = false; }

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


HHOOK hMouseHook;
HHOOK hKeyboardHook;

HOOKPROC lpMouseHookfn = NULL;
HOOKPROC lpKeyboardHookfn = NULL;

LRESULT WINAPI LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = faCallNextHookEx(hMouseHook, nCode, wParam, lParam);
	if (lpMouseHookfn)
		lpMouseHookfn(nCode, wParam, lParam);
	return result;
}
LRESULT WINAPI LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = faCallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	if (lpKeyboardHookfn)
		lpKeyboardHookfn(nCode, wParam, lParam);
	return result;
}

//Hook stubs

BOOL WINAPI hkSetForegroundWindow(HWND hWnd)
{
	return raSetForegroundWindow(hWnd);
}
BOOL WINAPI hkSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx, int cy, UINT uFlags)
{
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
					jiYuWndCanSize.remove(hWnd);
					raSetWindowPos(hWnd, hWndInsertAfter, x, y, cx, cy, uFlags | SWP_NOZORDER);
					SendMessage(hWnd, WM_SIZE, NULL, NULL);
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
	if (loaded)
	{
		if ((uFlags & SWP_NOSIZE) == 0)
		{
			if (x == 0 && y == 0 && cx == screenWidth && cy == screenHeight) {
				VSwitchLockState(hWnd, TRUE);
				if (fakeFull && VIsWindowGbOrHp(hWnd))
					return faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags);
				if (VIsInIllegalCanSizeWindows(hWnd)) {
					jiYuWndCanSize.remove(hWnd);
					HDWP rs = faDeferWindowPos(hWinPosInfo, hWnd, hWndInsertAfter, x, y, cx, cy, uFlags | SWP_NOZORDER);
					SendMessage(hWnd, WM_SIZE, NULL, NULL);
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
	if (loaded)
	{
		if (x == 0 && y == 0 && cx == screenWidth && cy == screenHeight) {
			VSwitchLockState(hWnd, TRUE);
			if (fakeFull && VIsWindowGbOrHp(hWnd))
				return raMoveWindow(hWnd, x, y, cx, cy, bRepaint);
			if (VIsInIllegalCanSizeWindows(hWnd)) {
				jiYuWndCanSize.remove(hWnd);
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
BOOL WINAPI hkDeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned,  LPOVERLAPPED lpOverlapped) {
	if (hDeviceTDKeybd) {
		if (hDevice == hDeviceTDKeybd) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (hDeviceTDProcHook) {
		if (hDevice == hDeviceTDProcHook) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (hDeviceTDNetFilter) {
		if (hDevice == hDeviceTDNetFilter) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	return raDeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
}
HANDLE WINAPI hkCreateFileA(LPCSTR lpFileName,  DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile ) {
	if (loaded)
	{
		if (StringHlp::StrEqualA(lpFileName, "\\\\.\\TDKeybd")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualA(lpFileName, "\\\\.\\TDProcHook")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualA(lpFileName, "\\\\.\\TDNetFilter")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
	}
	return faCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}
HANDLE WINAPI hkCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,  DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
	if (loaded)
	{
		if (StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDKeybd")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDProcHook")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDNetFilter")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
	}
	return faCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}
VOID WINAPI hkmouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData,  ULONG_PTR dwExtraInfo)
{
	if (allowControl) famouse_event(dwFlags, dx, dy, dwData, dwExtraInfo);
}
UINT WINAPI hkSendInput(UINT cInputs, LPINPUT pInputs, int cbSize)
{
	if (!allowControl && loaded)
		return cInputs;
	return faSendInput(cInputs, pInputs, cbSize);
}
BOOL WINAPI hkBringWindowToTop(HWND hWnd)
{
	if (loaded)
	{
		if (VIsInIllegalWindows(hWnd))
			return TRUE;
	}
	return faBringWindowToTop(hWnd);
}
BOOL WINAPI hkExitWindowsEx(UINT uFlags, DWORD dwReason) {
	const wchar_t* op = L"";
	if((uFlags & EWX_POWEROFF) == EWX_POWEROFF) op = L"关闭您的计算机";
	else if ((uFlags & EWX_REBOOT) == EWX_REBOOT) op = L"重启您的计算机";
	else if ((uFlags & EWX_LOGOFF) == EWX_LOGOFF) op = L"注销您的计算机";
	wchar_t str[37];
	swprintf_s(str, L"极域电子教室试图%s，是否允许极域继续操作？", op);
	if (MessageBox(NULL, str, L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDYES) 
		return faExitWindowsEx(uFlags, dwReason);
	SetLastError(ERROR_ACCESS_DENIED);
	return FALSE;
}
HINSTANCE WINAPI hkShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR lpDirectory, INT nShowCmd) {

	/*if (StrEqual(lpOperation, L"open") || StrEqual(lpOperation, L"runas")) {
		if (VGetExtension(wstring(lpFile)) == L".exe") {
			if (bandAllRunOp || !VShowOpConfirmDialog(lpFile, lpParameters))
				return hInst;
		}
	}*/
	return faShellExecuteW(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, nShowCmd);
}
BOOL WINAPI hkShellExecuteExW(SHELLEXECUTEINFOW *pExecInfo) {

	/*if (StrEqual(pExecInfo->lpFile, L"open") || StrEqual(pExecInfo->lpFile, L"runas")) {
		if (bandAllRunOp || VGetExtension(wstring(pExecInfo->lpFile)) == L".exe") {
			if (!VShowOpConfirmDialog(pExecInfo->lpFile, pExecInfo->lpParameters)) 
				return TRUE;
		}
	}*/
	return faShellExecuteExW(pExecInfo);

}
BOOL VInjectHookIntoProcess(HANDLE process)
{
	if (!process)
		return FALSE;

	WCHAR modulePath[MAX_PATH] = { 0 };
	DWORD pathLength = GetModuleFileNameW(hInst, modulePath, _countof(modulePath));
	if (pathLength == 0 || pathLength >= _countof(modulePath))
	{
		VDebugLog(L"capture-helper load skipped reason=module-path error=%lu", GetLastError());
		return FALSE;
	}

	const SIZE_T pathBytes = (static_cast<SIZE_T>(pathLength) + 1) * sizeof(WCHAR);
	LPVOID remotePath = VirtualAllocEx(process, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remotePath)
	{
		VDebugLog(L"capture-helper load failed stage=VirtualAllocEx error=%lu", GetLastError());
		return FALSE;
	}

	SIZE_T written = 0;
	if (!WriteProcessMemory(process, remotePath, modulePath, pathBytes, &written) || written != pathBytes)
	{
		VDebugLog(L"capture-helper load failed stage=WriteProcessMemory error=%lu written=%Iu expected=%Iu", GetLastError(), written, pathBytes);
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	LPTHREAD_START_ROUTINE loadLibrary = kernel32
		? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
		: NULL;
	if (!loadLibrary)
	{
		VDebugLog(L"capture-helper load failed stage=LoadLibraryW-address error=%lu", GetLastError());
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	HANDLE remoteThread = CreateRemoteThread(process, NULL, 0, loadLibrary, remotePath, 0, NULL);
	if (!remoteThread)
	{
		VDebugLog(L"capture-helper load failed stage=CreateRemoteThread error=%lu", GetLastError());
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	DWORD waitResult = WaitForSingleObject(remoteThread, 5000);
	DWORD remoteModule = 0;
	if (waitResult == WAIT_OBJECT_0)
		GetExitCodeThread(remoteThread, &remoteModule);
	BOOL result = waitResult == WAIT_OBJECT_0 && remoteModule != 0;
	VDebugLog(L"capture-helper load result=%d wait=%lu module=%p error=%lu", result, waitResult,
		reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(remoteModule)), result ? ERROR_SUCCESS : GetLastError());
	CloseHandle(remoteThread);
	VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
	return result;
}
BOOL WINAPI hkCreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
	const bool isCaptureHelper =
		(lpApplicationName && StrStrIW(lpApplicationName, L"DispcapHelper.exe")) ||
		(lpCommandLine && StrStrIW(lpCommandLine, L"DispcapHelper.exe"));
	if (isCaptureHelper)
	{
		const bool callerRequestedSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
		BOOL result = faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
			bInheritHandles, dwCreationFlags | CREATE_SUSPENDED, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
		DWORD error = GetLastError();
		BOOL moduleLoaded = FALSE;
		if (result && lpProcessInformation)
			moduleLoaded = VInjectHookIntoProcess(lpProcessInformation->hProcess);
		if (result && lpProcessInformation && !callerRequestedSuspended)
			ResumeThread(lpProcessInformation->hThread);
		LONG logIndex = InterlockedIncrement(&captureHelperProcessLogCount);
		VDebugLog(L"CreateProcessW capture-helper index=%ld result=%d moduleLoaded=%d error=%lu pid=%lu application=%s command=%s",
			logIndex, result, moduleLoaded, error, result && lpProcessInformation ? lpProcessInformation->dwProcessId : 0,
			lpApplicationName ? lpApplicationName : L"(null)", lpCommandLine ? lpCommandLine : L"(null)");
		SetLastError(error);
		return result;
	}

	bool canContinue = true;
	if(allowAllRunOp)
		return faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);

	LPCWSTR exePath = NULL;
	if (!StrEmepty(lpApplicationName) && _waccess_s(lpApplicationName, 0) == 0) exePath = lpApplicationName;
	else if (!StrEmepty(lpCommandLine) && _waccess_s(lpCommandLine, 0) == 0) exePath = lpCommandLine;
	else if(!StrEmepty(lpApplicationName)) exePath = lpApplicationName;
	else if (!StrEmepty(lpCommandLine)) exePath = lpCommandLine;

	LPCWSTR lowStr = StringHlp::StrLoW(exePath);
	if (StringHlp::StrContainsW(lowStr, L"shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	} 
	else if (StringHlp::StrContainsW(lowStr, L"jiyutrainer.exe", NULL) 
		|| StringHlp::StrContainsW(lowStr, L"explorer", NULL)
		|| StringHlp::StrContainsW(lowStr, L"sogouinput", NULL)
		|| StringHlp::StrContainsW(lowStr, L"ime", NULL)
		|| StringHlp::StrContainsW(lowStr, L"baidupinyin", NULL)
		|| VIsOpInWhiteList(lowStr)) canContinue = true;
	else if (StringHlp::StrContainsW(lowStr, L"tdchalk.exe", NULL))
		canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(lpApplicationName, lpCommandLine)) canContinue = false;

	if(canContinue) return faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	else return TRUE;
}
BOOL WINAPI hkCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
	const bool isCaptureHelper =
		(lpApplicationName && StrStrIA(lpApplicationName, "DispcapHelper.exe")) ||
		(lpCommandLine && StrStrIA(lpCommandLine, "DispcapHelper.exe"));
	if (isCaptureHelper)
	{
		const bool callerRequestedSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
		BOOL result = faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
			bInheritHandles, dwCreationFlags | CREATE_SUSPENDED, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
		DWORD error = GetLastError();
		BOOL moduleLoaded = FALSE;
		if (result && lpProcessInformation)
			moduleLoaded = VInjectHookIntoProcess(lpProcessInformation->hProcess);
		if (result && lpProcessInformation && !callerRequestedSuspended)
			ResumeThread(lpProcessInformation->hThread);
		LONG logIndex = InterlockedIncrement(&captureHelperProcessLogCount);
		VDebugLog(L"CreateProcessA capture-helper index=%ld result=%d moduleLoaded=%d error=%lu pid=%lu",
			logIndex, result, moduleLoaded, error, result && lpProcessInformation ? lpProcessInformation->dwProcessId : 0);
		SetLastError(error);
		return result;
	}

	bool canContinue = true;
	if (allowAllRunOp)
		return faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);

	LPCWSTR lpApplicationNameW = StringHlp::AnsiToUnicode(lpApplicationName);
	LPCWSTR lpCommandLineW = StringHlp::AnsiToUnicode(lpCommandLine);

	LPCSTR exePath = NULL;
	if (!StringHlp::StrEmeptyA(lpApplicationName) && _waccess_s(lpApplicationNameW, 0) == 0) exePath = lpApplicationName;
	else if (!StringHlp::StrEmeptyA(lpCommandLine) && _waccess_s(lpCommandLineW, 0) == 0) exePath = lpCommandLine;
	else if (!StringHlp::StrEmeptyA(lpApplicationName)) exePath = lpApplicationName;
	else if (!StringHlp::StrEmeptyA(lpCommandLine)) exePath = lpCommandLine;

	LPCSTR lowStr = StringHlp::StrLoA(exePath);
	LPCWSTR lowStrW = StringHlp::AnsiToUnicode(lowStr);

	if (StringHlp::StrContainsA(lowStr, "shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	}
	else if (StringHlp::StrContainsA(lowStr, "jiyutrainer.exe", NULL)
		|| StringHlp::StrContainsA(lowStr, "explorer", NULL)
		|| StringHlp::StrContainsA(lowStr, "sogouinput", NULL)
		|| StringHlp::StrContainsA(lowStr, "ime", NULL)
		|| StringHlp::StrContainsA(lowStr, "baidupinyin", NULL)
		|| VIsOpInWhiteList(lowStrW)) canContinue = true;
	else if (StringHlp::StrContainsA(lowStr, "tdchalk.exe", NULL))
		canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(lpApplicationNameW, lpCommandLineW)) canContinue = false;

	StringHlp::FreeStringPtr(lpApplicationNameW);
	StringHlp::FreeStringPtr(lpCommandLineW);
	StringHlp::FreeStringPtr(lowStrW);

	if (canContinue) return faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	else return TRUE;
}
UINT WINAPI hkWinExec(LPCSTR lpCmdLine, UINT uCmdShow) {
	bool canContinue = true;
	if (allowAllRunOp)
		return faWinExec(lpCmdLine, uCmdShow);

	LPCWSTR uniStr = StringHlp::AnsiToUnicode(lpCmdLine);
	LPCWSTR lowStr = StringHlp::StrLoW(uniStr);
	if (StringHlp::StrContainsW(lowStr, L"shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	}
	else if (StringHlp::StrContainsW(lowStr, L"jiyutrainer.exe", NULL)
		|| StringHlp::StrContainsW(lowStr, L"explorer", NULL)
		|| StringHlp::StrContainsW(lowStr, L"sogouinput", NULL)
		|| StringHlp::StrContainsW(lowStr, L"baidupinyin", NULL)
		|| StringHlp::StrContainsW(lowStr, L"ime", NULL)
		|| VIsOpInWhiteList(lowStr)) canContinue = true;
	else if (StringHlp::StrContainsW(lowStr, L"tdchalk.exe", NULL)) canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(uniStr, L"")) canContinue = false;

	delete uniStr;
	if (canContinue) return faWinExec(lpCmdLine, uCmdShow);
	else return 32;
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
LONG WINAPI hkSetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
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
	if (VIsWindowGbOrHp(hWnd)) 
	{
		if (nCmdShow == SW_SHOW || nCmdShow == SW_SHOWNORMAL  || nCmdShow == SW_MAXIMIZE || nCmdShow == SW_MAX || nCmdShow == SW_SHOWNA
			|| nCmdShow ==  SW_SHOWMAXIMIZED || nCmdShow == SW_SHOWNOACTIVATE || nCmdShow == SW_SHOWDEFAULT) {
			if(VIsWindowHp(hWnd))
				VSwitchLockState(hWnd, TRUE);
			if (VIsWindowGb(hWnd)) isGbFounded = true;
			VSendMessageBack(L"hkb:immck", hWndMsgCenter);
		}
		else if (nCmdShow == SW_HIDE) {
			if (VIsWindowGb(hWnd)) isGbFounded = false;
			VSwitchLockState(hWnd, FALSE);
		}
	}
	return faShowWindow(hWnd, nCmdShow);
}
HRESULT WINAPI hkDwmEnableComposition(UINT uCompositionAction)
{
	if (!allowMonitor && uCompositionAction == DWM_EC_DISABLECOMPOSITION)
		return S_OK;
	return faDwmEnableComposition(uCompositionAction);
}
HHOOK WINAPI hkSetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId)
{
	LONG logIndex = InterlockedIncrement(&inputHookLogCount);
	if (logIndex <= 20)
		VDebugLog(L"SetWindowsHookExA call index=%ld idHook=%d callback=%p module=%p thread=%lu loaded=%d",
			logIndex, idHook, lpfn, hmod, dwThreadId, loaded);
	if (loaded)
	{
		if (idHook == WH_CBT) {
			if (logIndex <= 20) VDebugLog(L"SetWindowsHookExA rejected reason=WH_CBT");
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		if (idHook == WH_MOUSE_LL || idHook == WH_MOUSE || idHook == WH_KEYBOARD_LL) {
			if (hmod == hLibTDMaster) {
				switch (idHook)
				{
				case WH_MOUSE_LL:
					lpMouseHookfn = lpfn;
					hMouseHook = faSetWindowsHookExA(idHook, LowLevelMouseProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_MOUSE_LL hooked 0x%08x", hMouseHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_MOUSE;
				case WH_MOUSE:
					lpMouseHookfn = lpfn;
					hMouseHook = faSetWindowsHookExA(idHook, LowLevelMouseProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_MOUSE hooked 0x%08x", hMouseHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_MOUSE;
				case WH_KEYBOARD_LL:
					lpKeyboardHookfn = lpfn;
					hKeyboardHook = faSetWindowsHookExA(idHook, LowLevelKeyboardProc, hmod, dwThreadId);
					VOutPutStatus(L"[K] hMouseHook WH_KEYBOARD_LL hooked 0x%08x", hKeyboardHook);
					VOutPutStatus(L"[K] lpfn : 0x%08x hmod :  0x%08x dwThreadId : %d", hMouseHook, hmod, dwThreadId);
					return (HHOOK)INVALID_HHOOK_KEYBOARD;
				}
			}
			if (logIndex <= 20) VDebugLog(L"SetWindowsHookExA rejected reason=input-hook-module-mismatch");
			return FALSE;
		}
	}
	return faSetWindowsHookExA(idHook, lpfn, hmod, dwThreadId);
}
LRESULT WINAPI hkCallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam) 
{
	if (hhk == (HHOOK)INVALID_HHOOK_MOUSE) return 0;
	if (hhk == (HHOOK)INVALID_HHOOK_KEYBOARD) return 0;
	return faCallNextHookEx(hhk, nCode, wParam, lParam);
}
BOOL WINAPI hkUnhookWindowsHookEx(HHOOK hhk)
{
	if (hhk == (HHOOK)INVALID_HHOOK_MOUSE) {
		BOOL rs = faUnhookWindowsHookEx(hMouseHook);
		hMouseHook = NULL;
		VOutPutStatus(L"[K] hMouseHook Unkooked !");
		return rs;
	}
	if (hhk == (HHOOK)INVALID_HHOOK_KEYBOARD) {
		BOOL rs = faUnhookWindowsHookEx(hKeyboardHook);
		hMouseHook = hKeyboardHook;
		VOutPutStatus(L"[K] hKeyboardHook Unkooked !");
		return rs;
	}
	return faUnhookWindowsHookEx(hhk);
}
BOOL WINAPI hkPostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (loaded && hWnd == mainWindow && VIsInIllegalWindowMessage(Msg) && ProhibitCloseWindow)
		return 0;
	if (loaded && VIsInIllegalWindowMessage(Msg)  && ProhibitCloseWindow) {
		DWORD pid;
		GetWindowThreadProcessId(hWnd, &pid);
		if(pid != currentPid)
			return 0;
	}
	return faPostMessageW(hWnd, Msg, wParam, lParam);
}
LRESULT WINAPI hkSendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (loaded && hWnd == mainWindow) return 0;
	if (loaded && VIsInIllegalWindowMessage(Msg) && ProhibitCloseWindow) {
		DWORD pid;
		GetWindowThreadProcessId(hWnd, &pid);
		if (pid != currentPid)
			return 0;
	}
	return faSendMessageW(hWnd, Msg,  wParam,  lParam);
}
BOOL WINAPI hkTerminateProcess(HANDLE hProcess, UINT uExitCode)
{
	if (loaded && ProhibitKillProcess  && hProcess != GetCurrentProcess()) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return faTerminateProcess(hProcess, uExitCode);
}
HRESULT WINAPI hkFilterConnectCommunicationPort(LPCWSTR lpPortName, DWORD dwOptions, LPCVOID lpContext, WORD wSizeOfContext, LPSECURITY_ATTRIBUTES lpSecurityAttributes, HANDLE * hPort)
{
	if (loaded && StrEqual(lpPortName, L"\\TDFileFilterPort"))
		return S_FALSE;
	return faFilterConnectCommunicationPort(lpPortName, dwOptions, lpContext, wSizeOfContext, lpSecurityAttributes,  hPort);
}
HWND WINAPI hkGetDesktopWindow(VOID)
{
	if (loaded)
		return allowMonitor ? desktopWindow : fakeDesktopWindow;
	return desktopWindow;
}
HDC WINAPI hkGetWindowDC(__in_opt HWND hWnd) {
	if (loaded && hWnd == desktopWindow && !allowMonitor)
		return faGetWindowDC(fakeDesktopWindow);
	return faGetWindowDC(hWnd);
}
int __cdecl hkEncodeToJPEGBuffer(const BYTE *pixels, int width, int height, int stride, BYTE *jpegOutput, DWORD *jpegLength, int quality, int reserved1, int reserved2)
{
	LONG callIndex = InterlockedIncrement(&jpegCallCount);
	bool traceCall = callIndex <= 3;
	const bool replaceFrame = !allowMonitor || g_VideoProps.m_EnableModifyVideoStream;
	const BYTE replacementValue = !allowMonitor ? 0xFF : 0x00;
	if (traceCall)
		VDebugLog(L"JPEG call enter index=%ld pixels=%p width=%d height=%d stride=%d output=%p lengthPtr=%p quality=%d allowMonitor=%d videoModify=%d",
			callIndex, pixels, width, height, stride, jpegOutput, jpegLength, quality, allowMonitor, g_VideoProps.m_EnableModifyVideoStream);
	if (replaceFrame)
	{
		if (!faEncodeToJPEGBuffer || !jpegOutput || !jpegLength ||
			width <= 0 || height <= 0 || stride <= 0 ||
			width > 16384 || height > 16384 ||
			static_cast<size_t>(stride) < static_cast<size_t>(width) * 3 ||
			static_cast<size_t>(height) > SIZE_MAX / static_cast<size_t>(stride))
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
		BYTE* imgData = NULL; BYTE* imgDibData = NULL;
		int imgW = 0, imgH = 0, imgDibStride = 0;
		if (g_VideoProps.m_EnableModifyVideoStream &&
			VAcquireImage(imgData, imgDibData, imgW, imgH, imgDibStride))
		{
			OverlayImage(const_cast<BYTE*>(pixels), width, height, stride, imgData, imgW, imgH);
			VReleaseImage();
			if (traceCall) VDebugLog(L"JPEG image overlay index=%ld imgW=%d imgH=%d", callIndex, imgW, imgH);
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
	LONG callIndex = InterlockedIncrement(&jpegCallCount);
	bool traceCall = callIndex <= 3;
	if (traceCall)
		VDebugLog(L"I422 call enter index=%ld pixels=%p width=%d height=%d stride=%d", callIndex, pixels, width, height, stride);

	const bool replaceFrame = !allowMonitor || g_VideoProps.m_EnableModifyVideoStream;
	if (!replaceFrame) {
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
		try { blackBuf.assign(frameSize, 0); }
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

HWND WINAPI hkGetForegroundWindow(VOID)
{
	return allowAllRunOp ? faGetForegroundWindow() : NULL;
}
HDC WINAPI hkCreateDCW(LPCWSTR pwszDriver, LPCWSTR pwszDevice, LPCWSTR pszPort, const DEVMODEW * pdm)
{
	if (!allowMonitor)
	{
		if (StrEqual(pwszDriver, L"DISPLAY"))
			return NULL;
	}
	return faCreateDCW(pwszDriver, pwszDevice, pszPort, pdm);
}
BOOL WINAPI hkEnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable)
{
	if(uIDEnableItem == SC_CLOSE)
		return 0;
	return faEnableMenuItem(hMenu, uIDEnableItem, uEnable);
}
DWORD WINAPI hkSetClassLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (loaded) {
		if (nIndex == GCL_STYLE && (dwNewLong & CS_NOCLOSE) == CS_NOCLOSE)
			return faSetClassLongA(hWnd, nIndex, dwNewLong ^ CS_NOCLOSE);
	}
	return faSetClassLongA(hWnd, nIndex, dwNewLong);
}
DWORD WINAPI hkSetClassLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
	if (loaded) {
		if(nIndex == GCL_STYLE && (dwNewLong & CS_NOCLOSE) == CS_NOCLOSE)
			return faSetClassLongW(hWnd, nIndex, dwNewLong ^ CS_NOCLOSE);
	}
	return faSetClassLongW(hWnd, nIndex, dwNewLong);
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
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);
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
		ShowWindow(fakeDesktopWindow, SW_SHOW);
		UpdateWindow(fakeDesktopWindow);
		ShowWindow(hDlg, SW_SHOW);
		UpdateWindow(hDlg);
		VDebugLog(L"MainWndProc initialization completed; window shown");
		return TRUE;
	}
	case WM_INITDIALOG: {
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);
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
			WNDPROC oldWndProc = (WNDPROC)GetWindowLong(jiYuGBDeskRdWnd, GWL_WNDPROC);
			if (oldWndProc != (WNDPROC)JiYuTDDeskWndProc) {
				jiYuTDDeskWndProc = (WNDPROC)oldWndProc;
				SetWindowLong(jiYuGBDeskRdWnd, GWL_WNDPROC, (LONG)JiYuTDDeskWndProc);
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
		else jiYuWndCanSize.push_back(hWnd);
	}
	else if (message == WM_DESTROY)
	{
		jiYuWnds.remove(hWnd);
		jiYuWndCanSize.remove(hWnd);
		VSwitchLockState(false);
		isGbFounded = false;
	}
	else if (message == WM_COMMAND) {
		switch (wParam)
		{
		case IDM_FULL:
		case IDM_TOPMOST: return SendMessage(hWnd, WM_SYSCOMMAND, wParam, NULL);
		case IDM_HELP_GB: {
			MessageBox(hWnd, L"这个右键菜单是 JiYuTrainer 为方便您控制广播全屏和窗口添加的。\n如果您需要打开极域的右键菜单，请在窗口中心右键。\n关于更多帮助，请查看 JiYuTrainer 帮助文档。 ", L"JiYuTrainer 提示", MB_ICONINFORMATION);
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
			if (MessageBox(hWnd, L"您真的要关闭广播窗口吗？", L"JiYuTrainer - 提示", MB_YESNO | MB_ICONEXCLAMATION) == IDYES)
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
	BYTE* rgbData = NULL;
	BYTE* dibData = NULL;
	int imageWidth = 0, imageHeight = 0, dibStride = 0;
	if (!VAcquireImage(rgbData, dibData, imageWidth, imageHeight, dibStride)) return FALSE;

	BOOL rendered = FALSE;
	if (dibData && imageWidth > 0 && imageHeight > 0 && dibStride >= imageWidth * 3 &&
		width > 0 && height > 0)
	{
		BITMAPINFO info;
		ZeroMemory(&info, sizeof(info));
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = imageWidth;
		info.bmiHeader.biHeight = -imageHeight; // 缓存按顶向下行序保存
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 24;
		info.bmiHeader.biCompression = BI_RGB;

		int oldStretchMode = SetStretchBltMode(hdc, COLORONCOLOR);
		int scanLines = StretchDIBits(hdc, x, y, width, height,
		                              0, 0, imageWidth, imageHeight,
		                              dibData, &info, DIB_RGB_COLORS, SRCCOPY);
		if (oldStretchMode != 0) SetStretchBltMode(hdc, oldStretchMode);
		rendered = scanLines != 0 && scanLines != GDI_ERROR;
	}

	VReleaseImage();
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
	if (!fpBitBlt)
		return FALSE;
	// 线程内重入保护:同一线程递归调用 BitBlt 直接透传,避免自嵌套死循环。
	if (bitBltHookActive)
		return fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

	// 快速路径:未启用视频流修改,或正在卸载 —— 纯透传,不做任何额外 GDI 调用,
	// 把对进程内其它 BitBlt(UI 绘制等)的影响降到最低,避免无谓开销与潜在卡死。
	// 当前仓库从未调用 VSetVideoStreamModify 提供替换帧,因此 g_HdcMem 恒为 NULL,
	// 走的就是这条快速路径:钩子存在但行为等同透传,绝不会卡死。
	if (g_Unloading || !g_VideoProps.m_EnableModifyVideoStream)
		return fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

	// 视频流修改:两种模式
	//  (a) 替换帧模式:有 g_HdcMem 且与捕获尺寸匹配 → 用替换帧 BitBlt(原逻辑)
	//  (b) 图片/纯黑模式:无替换帧(g_HdcMem==NULL) → 自定义图片铺满目标捕获位图，
	//      未配置或绘制失败时回退纯黑。
	//      防破坏学生端 UI:仅当捕获辅助进程(DispcapHelper,基本只做屏幕捕获)对所有
	//      BitBlt 刷黑;学生端主进程(StudentMain)只对与捕获尺寸匹配的 BitBlt 刷黑。
	HDC replacement = NULL;
	bool blackFill = false;
	if (g_HdcMemCsInit) {
		EnterCriticalSection(&g_HdcMemCs);
		if (g_HdcMem && cx == g_VideoProps.m_Width && cy == g_VideoProps.m_Height
			&& GetObjectType(hdcSrc) == OBJ_DC) {
			replacement = g_HdcMem;
			InterlockedIncrement(&g_HdcMemRefs);
		}
		else if (!g_HdcMem) {
			if (!captureHelperForwarding)
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
		LeaveCriticalSection(&g_HdcMemCs);
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
		// 用完了:递减引用计数;若归零且有待删除的旧 DC,在此安全删除
		// (引用计数归零意味着已没有任何线程正在使用该 DC)。
		EnterCriticalSection(&g_HdcMemCs);
		if (InterlockedDecrement(&g_HdcMemRefs) == 0 && !g_HdcMemPending.empty()) {
			for (size_t i = 0; i < g_HdcMemPending.size(); i++)
				DeleteDC(g_HdcMemPending[i]);
			g_HdcMemPending.clear();
		}
		LeaveCriticalSection(&g_HdcMemCs);
	}
	return result;
}

void VSetVideoStreamModify(int width, int height, HDC hdcMem, bool enable)
{
	g_VideoProps.m_Width = width;
	g_VideoProps.m_Height = height;
	g_VideoProps.m_EnableModifyVideoStream = enable;

	if (!g_HdcMemCsInit) return;
	EnterCriticalSection(&g_HdcMemCs);
	HDC old = g_HdcMem;
	// 替换或清空当前替换帧;enable=false 时直接置 NULL(停用替换)。
	g_HdcMem = (enable && hdcMem && hdcMem != old) ? hdcMem : NULL;
	LeaveCriticalSection(&g_HdcMemCs);

	if (old) {
		// 若此刻仍有捕获线程正引用 old,绝不能立即删:先挂起,等引用计数归零时由 Hook_BitBlt 删除。
		if (InterlockedCompareExchange(&g_HdcMemRefs, 0, 0) == 0) {
			DeleteDC(old);
		} else {
			EnterCriticalSection(&g_HdcMemCs);
			g_HdcMemPending.push_back(old);
			LeaveCriticalSection(&g_HdcMemCs);
		}
	}
}

void VSetPulseWindow(HWND hWnd)
{
	g_VideoProps.m_hPulseWnd = hWnd;
}
