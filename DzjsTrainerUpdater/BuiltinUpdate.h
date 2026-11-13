#pragma once
// 内置更新：主程序自己完成下载、校验、替换，不再依赖独立的更新器进程。
//
// 设计取舍：
//   · HTTP 用 WinHTTP，不用 WinINet —— WinINet 在本程序的 GUI 进程里实测会
//     直接 ERROR_INTERNET_CANNOT_CONNECT（见 DzjsTrainerUpdater.h 的说明），
//     WinHTTP 是另一套栈，不受那个问题影响。
//   · 替换运行中的 exe 只能"改名让位"：把自己重命名成 .old，再把新包搬过来，
//     然后把新 exe 以 --after-update 启动，本进程退出。新进程会等旧实例把
//     单实例互斥体放开再继续。
//   · 任何一步失败都不留半成品：拿不到包就删，搬不过去就把 .old 改回来。

#include "stdafx.h"

typedef void(__stdcall* JUpdaterProgressFn)(unsigned long long done, unsigned long long total, void* context);

// 把清单里的相对地址补成绝对地址（清单只给 1.0.5/DzjsTrainer.exe 这种相对路径时用）。
UPEXPORT_CFUNC(BOOL) JUpdater_ResolveUrl(LPCWSTR manifestUrl, LPWSTR outUrl, size_t outChars);

// 下载到 destPath，progress 可以为空。失败时不留残缺文件。
//
// 下载前会先完成一次时效票据校验（走取清单那条路），验不过直接返回失败，
// 不会向远端索取安装包。调用方拿到 FALSE 时应当回退到独立更新器进程。
UPEXPORT_CFUNC(BOOL) JUpdater_DownloadPackage(LPCWSTR url, LPCWSTR destPath, JUpdaterProgressFn progress, void* context);

// 文件 SHA-256，小写十六进制。用于和清单里的值比对。
UPEXPORT_CFUNC(BOOL) JUpdater_FileSha256(LPCWSTR path, LPWSTR outHex, size_t outChars);

// 在本进程里把当前 exe 换成 packagePath 指向的新版本，成功后由调用方立刻退出。
//
// 原理：Windows 的映像节以 FILE_SHARE_DELETE 打开，所以对**正在运行**的 exe
// 调 MoveFileEx 改名是允许的，改名后已映射的映像照常执行；腾出原名后新文件
// 可以占回原路径。改名后的 .old 在旧进程退出前删不掉（仍映射着），留给下次
// 启动清理。任何一步失败都会回滚，不会留下没有 exe 的目录。
//
// sha256Hex 必填：没有它无法确认包的来源，一律拒绝替换。失败原因写进 errorText。
UPEXPORT_CFUNC(BOOL) JUpdater_ApplyPackageInPlace(LPCWSTR packagePath, LPCWSTR sha256Hex,
	LPWSTR errorText, size_t errorChars);

// 启动时清理上一个版本留下的 <exe>.old。
UPEXPORT_CFUNC(void) JUpdater_CleanupOldImages();
