#pragma once

//
// HookEngine.h —— 用户态 inline hook 引擎（MinHook 封装）。
//
// 设计参考 D:\用户态杀软（R3ShieldCore）的 guard 骨架，取其三条：
//   1. 统一入口 + 批量启用：全部 CreateHook 成功后再一次性 EnableHook，
//      避免"装到一半开始生效"的半套状态；
//   2. 卸载三拍：DisableHook → 等在飞 detour 归零 → Uninitialize；
//   3. 失败方向安全：卸载开始后一律拒绝新装。
//
// 与原 mhook 的差异（调用点零改动）：
//   VSetHookLogged(name, &fn, detour) 的语义完全保留 —— 入参 fn 是原始函数地址，
//   返回时被改写为 trampoline（MinHook 与 mhook 在这一点上一致）。
//
#include <windows.h>

namespace HookEngine
{
	// 幂等；首次调用内部 MH_Initialize。失败后不再重试（失败方向安全）。
	bool EnsureInitialized();

	//
	// 安装一个 inline detour。
	//
	//   name        仅用于日志
	//   original    入：目标函数地址；出：trampoline
	//   replacement detour 地址
	//
	// 处于 ScopedBatch 作用域内 → CreateHook + QueueEnableHook，批次结束统一启用；
	// 否则立即启用（等价于原 Mhook_SetHook 的即时生效）。
	//
	bool Install(LPCWSTR name, PVOID* original, PVOID replacement);

	//
	// 批量作用域。VInstallHooks 有多个提前 return，用 RAII 保证每条路径都会 flush。
	//
	struct ScopedBatch
	{
		ScopedBatch();
		~ScopedBatch();
		ScopedBatch(const ScopedBatch&) = delete;
		ScopedBatch& operator=(const ScopedBatch&) = delete;
	};

	//
	// detour 在飞计数。
	//
	// 热路径 detour（BitBlt / LibDeskMonitor / DispDXGI / JPEG 编码）进入时 +1、
	// 离开时 -1。卸载时先 MH_DisableHook（不再有新进入），再等计数归零，
	// 之后才允许 MH_Uninitialize —— 否则仍在 trampoline 里的线程会踩到已释放的内存。
	//
	void EnterDetour();
	void LeaveDetour();

	struct DetourScope
	{
		DetourScope() { EnterDetour(); }
		~DetourScope() { LeaveDetour(); }
		DetourScope(const DetourScope&) = delete;
		DetourScope& operator=(const DetourScope&) = delete;
	};

	int Count();                 // 已成功创建的钩子数
	bool Unloading();            // 是否已进入卸载流程
	void BeginUnload();          // 置位：之后 Install 一律拒绝

	int DisableAll();            // MH_DisableHook(MH_ALL_HOOKS)，返回失败数
	bool WaitForDrain(DWORD timeoutMs);   // 等在飞 detour 归零
	void Uninitialize();         // MH_Uninitialize（释放 trampoline）
}
