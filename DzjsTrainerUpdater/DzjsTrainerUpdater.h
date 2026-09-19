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
};

UPEXPORT_CFUNC(BOOL) JUpdater_CheckInternet();

// 探测远端清单。成功（拿到 version 与 url）返回 TRUE。
// 内部按 "随机子域 x2 -> 顶点域名" 依次尝试，JSON 清单不可用时回退旧的纯文本接口。
// 这是只读操作，不会下载或替换任何文件。
//
// 注意：**在主程序（GUI 进程）里调用这个函数会失败** —— 实测返回
// ERROR_INTERNET_CANNOT_CONNECT，同账号同权限的控制台进程却正常。
// 主程序请改用下面的 JUpdater_ProbeManifest。
UPEXPORT_CFUNC(BOOL) JUpdater_FetchManifest(JUpdaterManifestInfo* info);

// 释放内嵌更新器，让它以 `--manifest <临时文件>` 代取一次清单，再读回结果。
// 更新器是控制台进程，不受上面那个问题影响。这是主程序该用的探测入口。
UPEXPORT_CFUNC(BOOL) JUpdater_ProbeManifest(JUpdaterManifestInfo* info);

// 版本比较：left < right 返回 -1，相等 0，大于 1。缺失分量按 0 处理。
UPEXPORT_CFUNC(int) JUpdater_CompareVersion(const wchar_t* left, const wchar_t* right);

// 释放内嵌的更新器并启动它。主程序确认用户要更新之后才调用。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchUpdater();
