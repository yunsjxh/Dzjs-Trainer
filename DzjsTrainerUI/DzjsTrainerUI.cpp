// DzjsTrainerUI.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "MainWindow.h"
#include "BugReportWindow.h"
#include "../DzjsTrainer/AppPublic.h"
#include "../DzjsTrainer/DzjsTrainer.h"
#include "../DzjsTrainerUpdater/DzjsTrainerUpdater.h"
#include "DzjsTrainerUI.h"

MainWindow *currentMainWindow = nullptr;
extern JTApp * currentApp;

// Windows 会用位图拉伸"非 DPI 感知"进程的窗口，高分屏上整窗发虚。
// 在创建任何窗口之前声明 Per-Monitor V2（Win10 1703+），失败再退回旧接口。
static void EnableDpiAwareness()
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32) {
		using SetContextFn = BOOL(WINAPI*)(HANDLE);
		auto setContext = reinterpret_cast<SetContextFn>(
			GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
		if (setContext && setContext(reinterpret_cast<HANDLE>(-4))) return; // PER_MONITOR_AWARE_V2
	}
	HMODULE shcore = LoadLibraryW(L"shcore.dll");
	if (shcore) {
		using SetAwarenessFn = HRESULT(WINAPI*)(int);
		auto setAwareness = reinterpret_cast<SetAwarenessFn>(
			GetProcAddress(shcore, "SetProcessDpiAwareness"));
		if (setAwareness && SUCCEEDED(setAwareness(2))) { FreeLibrary(shcore); return; }
		FreeLibrary(shcore);
	}
	SetProcessDPIAware();
}

int DzjsTrainerUIRunMain()
{
	EnableDpiAwareness();
	currentMainWindow = new MainWindow();
	int result = currentMainWindow->RunLoop();
	delete currentMainWindow;
	return result;
}
int DzjsTrainerUIRunUpdate()
{
	// 注意：这条路径**直接启动更新器，不经过主程序的探测与确认弹窗** ——
	// 用户会在没看到版本号 / 更新说明 / 哈希的情况下被拉去更新。
	// 目前它是死代码：AppStartTypeUpdater 从未被赋值（App.cpp:939 只比较不赋值）。
	// 将来若要启用，请改走 MainWindow::CheckForUpdate，而不是直接启动更新器。
	return JUpdater_LaunchUpdater() ? 0 : -1;
}
int DzjsTrainerUIRunConfig()
{
	return DzjsTrainerUIRunMain();
}
int DzjsTrainerUIRunBugReport()
{
	ShowBugReportWindow();
	return 0;
}

UIEXPORT_CFUNC(int) DzjsTrainerUICommonEntry(int i)
{
	SetUnhandledExceptionFilter(AppUnhandledExceptionFilter);
	switch (i)
	{
	case 0: return DzjsTrainerUIRunMain();
	case 1: return DzjsTrainerUIRunUpdate();
	case 2: return DzjsTrainerUIRunConfig();
	case 3: return DzjsTrainerUIRunBugReport();
	case 4: currentApp->RunOperation(AppOperation3);  return 0;
	default: return 0;
	}
}
