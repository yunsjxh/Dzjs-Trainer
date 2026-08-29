#pragma once

#include <Windows.h>

namespace config {

// 双进程守护中固定、已知的进程名。
inline constexpr const wchar_t* kKnownGuardProcessName = L"jfglzsn.exe";

// 后台关键进程名。结束前必须清除 ProcessBreakOnTermination。
inline constexpr const wchar_t* kCriticalProcessName = L"zmserv.exe";

// 关联 Windows 服务名。最终阶段会停止并删除该服务。
inline constexpr const wchar_t* kServiceName = L"zmserv";

// 结束固定守护进程后，等待它被随机守护进程重新拉起的最长时间。
inline constexpr DWORD kRespawnTimeoutMs = 60'000;

// 轮询进程快照的间隔。
inline constexpr DWORD kPollIntervalMs = 200;

// 单个进程终止后等待退出的最长时间。
inline constexpr DWORD kTerminateWaitMs = 5'000;

// 停止服务时等待 SERVICE_STOPPED 的最长时间。
inline constexpr DWORD kServiceStopTimeoutMs = 10'000;

// 安全校验：随机守护进程必须是 5 位小写字母 + ".exe"。
// 如安全部门确认父进程名存在大小写或其他变体，可改为 false。
inline constexpr bool kRequireRandomGuardianNamePattern = true;

// TerminateProcess 使用的退出码。
inline constexpr UINT kTerminateExitCode = 0xD2A5;

}  // namespace config
