#pragma once
#include "stdafx.h"

// 主程序侧的更新入口。
//
// 实际的下载/校验/替换仍然全部由独立的 DzjsTrainerUpdater.exe 完成 ——
// 主程序自己不下载、不替换、也不等待父进程退出。
//
// 但主程序需要**自己探测新版本**并把结果展示给用户（版本号 / 更新说明 / 哈希），
// 所以这里额外暴露一个只读的清单查询接口。它是纯探测，不产生任何副作用。

// 清单内容。跨模块边界，所以用固定大小的字符缓冲而不是 STL 容器
// （本模块编译成独立静态库，vtable/STL 布局不一致会出问题 —— 见 .cpp 顶部说明）。
struct JUpdaterManifestInfo {
	wchar_t version[64];    // 例：1.0.4
	wchar_t notes[4096];    // 更新说明，可含换行
	wchar_t url[1024];      // 安装包地址（可能是相对路径）
	wchar_t sha256[128];    // 安装包 SHA-256，小写十六进制；可能为空
	// 失败原因（成功时为空串）。清单请求要过时效票据校验，失败可能是
	// "服务器没提供票据""签名不符""本机时间差太大"等，光一个 FALSE 分不清。
	wchar_t error[256];
};

UPEXPORT_CFUNC(BOOL) JUpdater_CheckInternet();

// 启动最早期调一次，在任何驱动加载 / 自保护建立之前：先把 Schannel 和
// Negotiate 的出站凭据各取一遍，让 secur32 的进程本地包列表提前初始化。
//
// 背景：主程序进程里 WinHttpSendRequest 一直返回 SEC_E_SECPKG_NOT_FOUND
// (0x80090305)，而同一份代码在独立进程里正常。包列表是 secur32 的进程本地
// 缓存、只初始化一次，所以这里的结果有两种解读：
//   返回 TRUE  —— 预热成功。若之后进程内 TLS 仍然失败，说明是启动期某个动作
//                 把已建好的状态又破坏了（需要看 DiagnoseSspi 的输出定位）。
//   返回 FALSE —— 进程一出生就没有可用的 Schannel，与后续动作无关。
UPEXPORT_CFUNC(BOOL) JUpdater_WarmupSspi();

// 分层自检：包枚举 / 每包凭据 / CNG 提供程序 / 模块加载状态，结果写日志。
// 每进程只跑一次；在进程内 TLS 失败时调用。
UPEXPORT_CFUNC(VOID) JUpdater_DiagnoseSspi();

// 探测远端清单。成功（拿到 version 与 url）返回 TRUE。
// 内部按 "随机子域 x2 -> 顶点域名" 依次尝试，JSON 清单不可用时回退旧的纯文本接口。
// 这是只读操作，不会下载或替换任何文件。
//
// 主程序直接调用它即可：内部已从 WinINet 换成 WinHTTP，GUI 进程里可用
// （WinINet 在这个进程里会返回 ERROR_INTERNET_CANNOT_CONNECT，那正是历史上
// 被迫引入 JUpdater_ProbeManifest 子进程、进而闪黑窗口的原因）。
UPEXPORT_CFUNC(BOOL) JUpdater_FetchManifest(JUpdaterManifestInfo* info);

// 旧路径：释放内嵌更新器、让它以 `--manifest <临时文件>` 代取清单。
// WinHTTP 版本可用后它只作为兜底（代理/防火墙下 WinHTTP 不通时）。
// 该模式下更新器已改成不建控制台，不会再闪黑窗口。
UPEXPORT_CFUNC(BOOL) JUpdater_ProbeManifest(JUpdaterManifestInfo* info);

// 版本比较：left < right 返回 -1，相等 0，大于 1。缺失分量按 0 处理。
UPEXPORT_CFUNC(int) JUpdater_CompareVersion(const wchar_t* left, const wchar_t* right);

// 释放内嵌的更新器并启动它。主程序确认用户要更新之后才调用。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchUpdater();

// 只让它替换：包已经由主程序下载并校验过，更新器会先等主程序退出，
// 然后把包换上去并重启。sha256Hex 可为空。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchApply(LPCWSTR packagePath, LPCWSTR sha256Hex);

// 借子进程下载安装包。主程序自己的进程里 WinHTTP 的 TLS 会失败
// （Schannel 返回 SEC_E_SECPKG_NOT_FOUND），同一个库在独立进程里正常，
// 所以网络抓取统一退到子进程做，主程序只负责校验与替换。
// notifyWindow 非空时，子进程每读完一块就 PostMessage(WM_JUP_DOWNLOAD_PROGRESS)
// 回来（total 在 wParam、done 在 lParam），界面靠它画真实进度条。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchDownload(LPCWSTR url, LPCWSTR destPath, LPCWSTR sha256Hex,
	DWORD timeoutMs, HWND notifyWindow);
