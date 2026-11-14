#pragma once
// 更新清单的获取与版本比较。
//
// 这段逻辑原先只在独立的更新器 EXE（UpdaterMain.cpp）里，主程序拿不到，
// 所以主程序无法自己探测新版本。现在抽成独立模块，编译进主程序链接的
// 静态库（DzjsTrainerUpdater.vcxproj），主程序即可直接调用。
//
// 更新器 EXE 仍然保留它自己那份（不改动已经能工作的代码）。

#include <string>

namespace UpdaterManifest {

struct Info {
	std::wstring version;   // 清单里的版本号，例如 1.0.4
	std::wstring notes;     // 更新说明（可含换行）
	std::wstring url;       // 安装包地址（可能是相对路径）
	std::wstring sha256;    // 安装包 SHA-256（小写十六进制，可能为空）
	// 失败原因（成功时为空）。清单请求现在要通过时效票据校验，
	// 失败可能是"服务器没给票据""签名不符""本机时间差太多"等完全不同的情况，
	// 只回一个 false 会让用户和排查者都无从下手。
	std::wstring error;
};

// 拉取清单。成功返回 true 且 info.version / info.url 非空。
// 依次尝试：随机子域 x2 -> 顶点域名。每个端点都要先通过时效票据校验。
// 失败时 info.error 里是最后一次的原因，可以直接展示给用户。
bool Fetch(Info& info);

// 顶点域名下的服务器根，例如 https://host/update-server/
// 清单里给相对路径时，内置更新用它补成绝对地址。
std::wstring ApexBaseUrl();

// 版本比较：left < right 返回 -1，相等 0，left > right 返回 1。
// 缺失的分量按 0 处理（1.0.5 与 1.0.5.0 视为相同）。
int CompareVersions(const std::wstring& left, const std::wstring& right);

// 启动最早期调一次：先把 Schannel/Negotiate 的出站凭据取一遍，让 secur32 的
// 进程本地包列表在那之前就初始化好。主程序进程里的 WinHTTP TLS 一直报
// SEC_E_SECPKG_NOT_FOUND，而同一份代码在独立进程里正常 —— 如果问题出在
// "首次枚举被启动期的某个动作搞坏"，那提前做掉就能修好；如果这里就失败，
// 说明进程一出生就没有可用的 SSPI，与后续动作无关。返回值即 Schannel 是否可用。
bool WarmupSspi();

// 分层自检：包枚举 / 每个包的凭据 / CNG 提供程序 / 模块加载状态。
// 每进程只跑一次，写在进程内 TLS 失败之后调用。
void DiagnoseSspi();

} // namespace UpdaterManifest
