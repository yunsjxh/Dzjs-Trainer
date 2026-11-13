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

} // namespace UpdaterManifest
