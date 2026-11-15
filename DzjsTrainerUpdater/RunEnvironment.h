#pragma once

// 运行环境自述。关于页和启动日志共用同一份，避免两边说法对不上。
//
// Summary 是给人看的一行：Windows 11 26100.2314 · x64 · 管理员+UIAccess
// Detail 在 Summary 后面追加进程位数和会话类型，给日志用。
//
// 系统版本不是装饰：抓屏排除的「透过」只有 Win10 2004（build 19041）以后成立，
// 更早的系统会把它当黑块处理，见 WindowCaptureProtection.h。
const wchar_t* JTRunEnvSummary();
const wchar_t* JTRunEnvDetail();
