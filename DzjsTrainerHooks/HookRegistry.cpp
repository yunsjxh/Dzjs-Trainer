// ============================================================================
// HookRegistry.cpp —— 全量钩子集的有序注册表
//
// 原来 VInstallHooks 里是 40 多行零散的 `if (VHookAllowed()) hkN = ...`，
// 顺序只存在于源码的书写次序里，改一行就可能悄悄改变 HooksLimit=N 的语义。
// 这里把整套钩子收成一张**按 seq 排序的数据表**：
//   * 每一项显式带域标签、原始指针、detour、安装标记；
//   * InstallFullSet() 只做一遍线性扫描，行为与原实现逐项等价；
//   * CountByDomain() 让四个 guard 模块能报告自己装了几个。
//
// 注意：表**必须保持 hkN 升序**。HooksLimit 是排查「哪个钩子把极域卡死」的
// 二分开关，VHookAllowed() 每次调用自增序号，只有 useLimit=true 的项会消耗
// 一次计数；Action / SkipWithLog 不消耗，与原实现一致。
// ============================================================================
#include "HooksCommon.h"
#include "GuardWindow.h"
#include "GuardProcess.h"
#include "GuardInput.h"
#include "GuardDisplay.h"

namespace {

#define HOOK_ITEM(seq, domain, text, orig, detour, flag) \
	{ seq, HookDomain::domain, L##text, (PVOID*)&(orig), (PVOID)(detour), &(flag), true, HookSpecKind::Install, nullptr }
#define HOOK_IF_ORIGINAL(seq, domain, text, orig, detour, flag) \
	{ seq, HookDomain::domain, L##text, (PVOID*)&(orig), (PVOID)(detour), &(flag), false, HookSpecKind::InstallIfOriginal, nullptr }
#define HOOK_SKIP(seq, domain, flag, text) \
	{ seq, HookDomain::domain, L##text, nullptr, nullptr, &(flag), false, HookSpecKind::SkipWithLog, nullptr }
#define HOOK_ACTION(seq, domain, fn) \
	{ seq, HookDomain::domain, nullptr, nullptr, nullptr, nullptr, false, HookSpecKind::Action, &(fn) }

const HookSpec kFullSet[] = {
	HOOK_ITEM( 1, Window,  "SetWindowPos",              raSetWindowPos,              hkSetWindowPos,              hk1),
	HOOK_ITEM( 2, Window,  "MoveWindow",                raMoveWindow,                hkMoveWindow,                hk2),
	HOOK_ITEM( 3, Window,  "SetForegroundWindow",       raSetForegroundWindow,       hkSetForegroundWindow,       hk3),
	HOOK_ITEM( 4, Window,  "BringWindowToTop",          faBringWindowToTop,          hkBringWindowToTop,          hk4),
	HOOK_ITEM( 5, Process, "DeviceIoControl",           raDeviceIoControl,           hkDeviceIoControl,           hk5),
	HOOK_ITEM( 6, Process, "CreateFileA",               faCreateFileA,               hkCreateFileA,               hk6),
	HOOK_ITEM( 7, Process, "CreateFileW",               faCreateFileW,               hkCreateFileW,               hk7),
	HOOK_ITEM( 8, Input,   "SetWindowsHookExA",         faSetWindowsHookExA,         hkSetWindowsHookExA,         hk8),
	HOOK_ITEM( 9, Window,  "DeferWindowPos",            faDeferWindowPos,            hkDeferWindowPos,            hk9),
	HOOK_ITEM(10, Input,   "SendInput",                 faSendInput,                 hkSendInput,                 hk10),
	HOOK_ITEM(11, Input,   "mouse_event",               famouse_event,               hkmouse_event,               hk11),
	// hk12..hk15 是历史保留位，原实现从未安装，也不消耗 HooksLimit 序号。
	HOOK_ITEM(16, Window,  "SetWindowLongA",            faSetWindowLongA,            hkSetWindowLongA,            hk16),
	HOOK_ITEM(17, Window,  "SetWindowLongW",            faSetWindowLongW,            hkSetWindowLongW,            hk17),
	HOOK_ITEM(18, Window,  "ShowWindow",                faShowWindow,                hkShowWindow,                hk18),
	HOOK_ITEM(19, Process, "ExitWindowsEx",             faExitWindowsEx,             hkExitWindowsEx,             hk19),
	// hk20 / hk21 (ShellExecute*) 在原实现里被注释掉，保持不安装。
	HOOK_ITEM(22, Process, "CreateProcessW",            faCreateProcessW,            hkCreateProcessW,            hk22),
	// DwmEnableComposition 只在模块里真的导出时才装，且原实现不消耗 HooksLimit 序号。
	HOOK_IF_ORIGINAL(23, Display, "DwmEnableComposition", faDwmEnableComposition, hkDwmEnableComposition, hk23),
	HOOK_ITEM(24, Process, "WinExec",                    faWinExec,                   hkWinExec,                   hk24),
	HOOK_ITEM(25, Input,   "CallNextHookEx",            faCallNextHookEx,            hkCallNextHookEx,            hk25),
	HOOK_ITEM(26, Display, "GetWindowDC",               faGetWindowDC,               hkGetWindowDC,               hk26),
	HOOK_ITEM(27, Display, "GetDesktopWindow",          faGetDesktopWindow,          hkGetDesktopWindow,          hk27),
	// JPEG 三层钩插在 hk27 与 hk29 之间；它自己管理 hk28，不消耗 HooksLimit 序号。
	HOOK_ACTION(28, Display, VEnsureJPEGHook),
	HOOK_ITEM(29, Input,   "UnhookWindowsHookEx",       faUnhookWindowsHookEx,       hkUnhookWindowsHookEx,       hk29),
	HOOK_ITEM(30, Process, "CreateProcessA",            faCreateProcessA,            hkCreateProcessA,            hk30),
	HOOK_ITEM(31, Window,  "GetForegroundWindow",       faGetForegroundWindow,       hkGetForegroundWindow,       hk31),
	// hk32 历史保留位。
	HOOK_ITEM(33, Display, "CreateDCW",                 faCreateDCW,                 hkCreateDCW,                 hk33),
	HOOK_ITEM(34, Window,  "EnableMenuItem",           faEnableMenuItem,            hkEnableMenuItem,            hk34),
	HOOK_ITEM(35, Window,  "SetClassLongA",             faSetClassLongA,             hkSetClassLongA,             hk35),
	HOOK_ITEM(36, Window,  "SetClassLongW",             faSetClassLongW,             hkSetClassLongW,             hk36),
	// 这两个消息钩会拦住 DLL 自己的对话框消息、把 UI 线程卡死，原实现显式禁用。
	HOOK_SKIP(37, Process, hk37, "PostMessageW hook disabled to preserve StudentMain stream initialization"),
	HOOK_SKIP(38, Process, hk38, "SendMessageW hook disabled to keep status UI responsive"),
	HOOK_ITEM(39, Process, "TerminateProcess",          faTerminateProcess,          hkTerminateProcess,          hk39),
	HOOK_IF_ORIGINAL(40, Process, "FilterConnectCommunicationPort", faFilterConnectCommunicationPort, hkFilterConnectCommunicationPort, hk40),
	// BitBlt 全量钩会误伤 UI 绘制，帧替换改走 EncodeToJPEGBuffer；minimal 模式才按需装。
	HOOK_SKIP(41, Display, hk41, "BitBlt hook disabled; video replacement uses EncodeToJPEGBuffer"),
	// ---- hk42..hk45：从 R3ShieldCore 移植的四个缺口 ----------------------------
	// 截屏不只走 BitBlt（hk41 还是禁用的）。GetDC(NULL) 之后直接 GetDIBits 读像素、
	// StretchBlt 缩放拷贝、PrintWindow 按窗口抓，三条路都能绕过去。
	// 前置判据见 GuardWindow.cpp 的 IsScreenCaptureInto / IsScreenDibSource：
	// 源 DC 得是桌面（窗口 DC 或 CreateDCW(L"DISPLAY") 的显示 DC），且目的端是内存 DC；
	// 内存 DC 全部放行，所以不会误伤极域自己的界面绘制。
	HOOK_ITEM(42, Display, "PrintWindow",               faPrintWindow,               hkPrintWindow,               hk42),
	HOOK_ITEM(43, Display, "GetDIBits",                 faGetDIBits,                 hkGetDIBits,                 hk43),
	HOOK_ITEM(44, Display, "StretchBlt",                faStretchBlt,                hkStretchBlt,                hk44),
	// 原来只钩了 A 版，实际调用方绝大多数走 W 版。
	HOOK_ITEM(45, Input,   "SetWindowsHookExW",         faSetWindowsHookExW,         hkSetWindowsHookExW,         hk45),
};

const size_t kFullSetCount = sizeof(kFullSet) / sizeof(kFullSet[0]);

} // namespace

namespace HookRegistry {

// [CustomInject] Mode=minimal 时，通用宿主跳过极域专属钩子：
//   * 整个 Display 域（JPEG / DXGI / DC 捕获），普通进程里没有这些模块；
//   * Process 域里会干扰普通程序正常行为的驱动通信、远程运行白名单、关机/结束进程拦截。
// 只在 g_customHost 模式下生效，极域进程的 34 项表与 HooksLimit 语义完全不变。
bool CustomHostSkip(const HookSpec& s)
{
	if (s.domain == HookDomain::Display) return true;
	if (s.domain != HookDomain::Process) return false;
	switch (s.seq) {
	case 5:  case 6:  case 7:   // DeviceIoControl / CreateFileA/W（极域驱动通信）
	case 19:                    // ExitWindowsEx（阻止远程关机）
	case 22: case 24: case 30:  // CreateProcessW / WinExec / CreateProcessA 白名单
	case 39: case 40:           // TerminateProcess / FilterConnectCommunicationPort
		return true;
	default:
		return false;
	}
}

void InstallFullSet()
{
	for (size_t i = 0; i < kFullSetCount; ++i)
	{
		const HookSpec& s = kFullSet[i];
		if (g_customHookMinimal && CustomHostSkip(s)) continue;
		switch (s.kind)
		{
		case HookSpecKind::Action:
			if (s.action) s.action();
			break;

		case HookSpecKind::SkipWithLog:
			if (s.flag) *s.flag = 0;
			if (s.name) VDebugLog(L"%s", s.name);
			break;

		case HookSpecKind::InstallIfOriginal:
			// 模块未导出该 API 时静默跳过，且**不**消耗 HooksLimit 序号（原实现如此）。
			if (!s.original || !*s.original) break;
			// fall through
		case HookSpecKind::Install:
			if (s.useLimit && !VHookAllowed()) break;
			if (s.flag) *s.flag = VSetHookLogged(s.name, s.original, s.detour);
			break;
		}
	}
}

int CountByDomain(HookDomain domain)
{
	int count = 0;
	for (size_t i = 0; i < kFullSetCount; ++i)
	{
		const HookSpec& s = kFullSet[i];
		if (s.domain == domain && s.flag && *s.flag) ++count;
	}
	return count;
}

int TotalInstalled()
{
	int count = 0;
	for (size_t i = 0; i < kFullSetCount; ++i)
		if (kFullSet[i].flag && *kFullSet[i].flag) ++count;
	return count;
}

} // namespace HookRegistry

namespace GuardWindow  { int HookCount() { return HookRegistry::CountByDomain(Domain); } }
namespace GuardProcess { int HookCount() { return HookRegistry::CountByDomain(Domain); } }
namespace GuardInput   { int HookCount() { return HookRegistry::CountByDomain(Domain); } }
namespace GuardDisplay { int HookCount() { return HookRegistry::CountByDomain(Domain); } }
