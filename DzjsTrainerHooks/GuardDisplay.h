#pragma once
// ============================================================================
// GuardDisplay —— 显示/抓屏域的模块边界
//
// 与其它三个 guard 不同，本域的 detour **刻意留在 DzjsTrainerHooks.cpp**：
// JPEG 三层钩、DispFilter DXGI、LibDeskMonitor IAT、BitBlt 帧替换共用一组
// 运行期状态（IAT 补丁表、重试线程、g_surface / g_frames），物理拆开只会把
// 这些状态重新变成跨 TU 的 extern 泥潭。像素运算与帧状态已经按同一范式抽成
// ReplaceCodec.h（纯函数）与 ReplaceState.h（对象）。
//
// 注册表里标为 Display 的四项（hk23/26/27/33，假桌面与 DC 拦截）实现体在
// GuardWindow.cpp；它们逻辑上属于显示域，物理上跟随窗口 DC 那一组。
// ============================================================================
#include "HooksCommon.h"

namespace GuardDisplay {
	constexpr HookDomain Domain = HookDomain::Display;
	int HookCount();   // 已安装的显示域钩子数，由 HookRegistry.cpp 按域统计
}
