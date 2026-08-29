// JiYuTrainerUI.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "MainWindow.h"
#include "UpdaterWindow.h"
#include "BugReportWindow.h"
#include "../JiYuTrainer/AppPublic.h"
#include "../JiYuTrainer/JiYuTrainer.h"
#include "JiYuTrainerUI.h"

MainWindow *currentMainWindow = nullptr;
extern JTApp * currentApp;

int JiYuTrainerUIRunMain()
{
	currentMainWindow = new MainWindow();
	int result = currentMainWindow->RunLoop();
	delete currentMainWindow;
	return result;
}
int JiYuTrainerUIRunUpdate()
{
	UpdaterWindow updaterWindow(NULL);
	return updaterWindow.RunLoop();
}
int JiYuTrainerUIRunConfig()
{
	return JiYuTrainerUIRunMain();
}
int JiYuTrainerUIRunBugReport()
{
	ShowBugReportWindow();
	return 0;
}

UIEXPORT_CFUNC(int) JiYuTrainerUICommonEntry(int i)
{
	SetUnhandledExceptionFilter(AppUnhandledExceptionFilter);
	switch (i)
	{
	case 0: return JiYuTrainerUIRunMain();
	case 1: return JiYuTrainerUIRunUpdate();
	case 2: return JiYuTrainerUIRunConfig();
	case 3: return JiYuTrainerUIRunBugReport();
	case 4: currentApp->RunOperation(AppOperation3);  return 0;
	default: return 0;
	}
}
