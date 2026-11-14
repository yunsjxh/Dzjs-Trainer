#include "stdafx.h"
#include "HookEngine.h"
#include "minhook/include/MinHook.h"

// MinHook 以源码形式内嵌（minhook/src，BSD-2-Clause，见 minhook/LICENSE.txt）。
// 不用预编译 .lib：第三方 lib 的工具集版本与本工程不一致会直接 C1047。

// 日志实现住在主文件（VDebugLog 内部会走被挂钩的 CreateFileW，必须按需避开递归）。
void VDebugLog(const wchar_t* str, ...);

namespace
{
	INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
	bool g_initOk = false;

	volatile LONG g_batchDepth = 0;     // >0 表示处于批量安装作用域
	volatile LONG g_hookCount = 0;      // 成功 CreateHook 的数量
	volatile LONG g_activeDetours = 0;  // 正在执行的 detour 数
	volatile LONG g_unloading = 0;

	BOOL CALLBACK InitOnceCallback(PINIT_ONCE, PVOID, PVOID*)
	{
		const MH_STATUS status = MH_Initialize();
		if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
			g_initOk = true;
		}
		else {
			VDebugLog(L"HookEngine: MH_Initialize 失败 status=%d (%S)",
				static_cast<int>(status), MH_StatusToString(status));
			g_initOk = false;
		}
		return TRUE;
	}

	// 批次结束时统一启用。返回启用的失败数（只为日志）。
	int CommitPending()
	{
		const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
		if (status != MH_OK) {
			VDebugLog(L"HookEngine: MH_EnableHook(ALL) 失败 status=%d (%S)",
				static_cast<int>(status), MH_StatusToString(status));
			return 1;
		}
		return 0;
	}
}

namespace HookEngine
{
	bool EnsureInitialized()
	{
		InitOnceExecuteOnce(&g_initOnce, InitOnceCallback, nullptr, nullptr);
		return g_initOk;
	}

	bool Unloading()
	{
		return InterlockedCompareExchange(&g_unloading, 0, 0) != 0;
	}

	void BeginUnload()
	{
		InterlockedExchange(&g_unloading, 1);
	}

	int Count()
	{
		return static_cast<int>(InterlockedCompareExchange(&g_hookCount, 0, 0));
	}

	bool Install(LPCWSTR name, PVOID* original, PVOID replacement)
	{
		if (!original || !replacement) {
			return false;
		}
		if (Unloading()) {
			VDebugLog(L"HookEngine: 拒绝安装 %s（已进入卸载流程）", name ? name : L"?");
			return false;
		}
		if (!EnsureInitialized()) {
			return false;
		}

		// 注意：*original 必须是**原始函数地址**。同一个变量被第二次 Install 时
		// 它已经是 trampoline —— 调用点（hkNN 守卫）负责保证不会发生。
		LPVOID target = *original;
		if (!target) {
			return false;
		}

		MH_STATUS status = MH_CreateHook(target, replacement, original);
		if (status == MH_ERROR_ALREADY_CREATED) {
			// 幂等：同一目标重复安装直接视为成功，不重复计数。
			return true;
		}
		if (status != MH_OK) {
			VDebugLog(L"HookEngine: MH_CreateHook(%s) 失败 status=%d (%S)",
				name ? name : L"?", static_cast<int>(status), MH_StatusToString(status));
			return false;
		}

		InterlockedIncrement(&g_hookCount);

		if (InterlockedCompareExchange(&g_batchDepth, 0, 0) > 0) {
			status = MH_QueueEnableHook(target);
			if (status != MH_OK) {
				VDebugLog(L"HookEngine: MH_QueueEnableHook(%s) 失败 status=%d (%S)",
					name ? name : L"?", static_cast<int>(status), MH_StatusToString(status));
				return false;
			}
			return true;
		}

		status = MH_EnableHook(target);
		if (status != MH_OK) {
			VDebugLog(L"HookEngine: MH_EnableHook(%s) 失败 status=%d (%S)",
				name ? name : L"?", static_cast<int>(status), MH_StatusToString(status));
			return false;
		}
		return true;
	}

	ScopedBatch::ScopedBatch()
	{
		InterlockedIncrement(&g_batchDepth);
	}

	ScopedBatch::~ScopedBatch()
	{
		if (InterlockedDecrement(&g_batchDepth) == 0) {
			CommitPending();
		}
	}

	void EnterDetour()
	{
		InterlockedIncrement(&g_activeDetours);
	}

	void LeaveDetour()
	{
		InterlockedDecrement(&g_activeDetours);
	}

	int DisableAll()
	{
		if (!g_initOk) {
			return 0;
		}
		const MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
		if (status != MH_OK) {
			VDebugLog(L"HookEngine: MH_DisableHook(ALL) 失败 status=%d (%S)",
				static_cast<int>(status), MH_StatusToString(status));
			return 1;
		}
		return 0;
	}

	bool WaitForDrain(DWORD timeoutMs)
	{
		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		for (;;) {
			const LONG active = InterlockedCompareExchange(&g_activeDetours, 0, 0);
			if (active == 0) {
				return true;
			}
			if (GetTickCount64() >= deadline) {
				VDebugLog(L"HookEngine: 等待 detour 排空超时，仍有 %ld 个在跑", active);
				return false;
			}
			Sleep(1);
		}
	}

	void Uninitialize()
	{
		if (!g_initOk) {
			return;
		}
		// 先移除再反初始化：MH_Uninitialize 会连带释放全部 trampoline。
		MH_RemoveHook(MH_ALL_HOOKS);
		const MH_STATUS status = MH_Uninitialize();
		if (status != MH_OK) {
			VDebugLog(L"HookEngine: MH_Uninitialize 失败 status=%d (%S)",
				static_cast<int>(status), MH_StatusToString(status));
		}
		g_initOk = false;
	}
}
