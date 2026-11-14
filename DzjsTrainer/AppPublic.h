#pragma once

#include "Logger.h"
#include "SettingHlp.h"
#include "TrainerWorker.h" 
#include "JyUdpAttack.h" 

#define CURRENT_VERSION "1.0.5"

// 进程启动最早期（wWinMain 里、任何驱动加载与权限提升之前）那一次 SSPI 预热的
// 返回值，0xFFFFFFFF 表示没跑过。那行日志写在日志文件打开之前，落不了盘，
// 所以结果留在这里，等更新探测线程连同它自己的复测一起记下来。
extern unsigned g_sspiEarlySchannel;
extern unsigned g_sspiEarlyNegotiate;

#define FAST_STR_BINDER(str, fstr, size, ...) WCHAR str[size]; swprintf_s(str, fstr, __VA_ARGS__)

#define PART_INI -1
#define PART_MAIN 0
#define PART_LOG 1
#define PART_HOOKER 3
#define PART_DRIVER 4
#define PART_AV_DRIVER 5
// 「进程注入」：64 位目标进程要用 x64 版 hooks DLL + 64 位注入助手，
// 两者都作为 BIN 资源内嵌，运行时释放到主程序同目录。
#define PART_HOOKER64 6
#define PART_INJECTOR64 7



enum EXTRACT_RES {
	ExtractUnknow,
	ExtractCreateFileError,
	ExtractWriteFileError,
	ExtractReadResError,
	ExtractSuccess
};
enum AppOperation {
	AppOperation1,
	AppOperation2,
	AppOperation3,
	AppOperationKShutdown,
	AppOperationKReboot,
	AppOperationUnLoadDriver,
	AppOperationForceLoadDriver,
};

class JTApp
{
public:

	/*
		ж��
	*/
	virtual void UnInstall() {}

	/*
		�ͷ�ģ����Դ���ļ�
		[resModule] ��Դ����ģ��
		[resId] ��Դid
		[resType] ��Դ����
		[extractTo] �ļ�·��
	*/
	virtual EXTRACT_RES InstallResFile(HINSTANCE resModule, LPWSTR resId, LPCWSTR resType, LPCWSTR extractTo) { return EXTRACT_RES::ExtractUnknow; }

	//��������в����Ƿ����ĳ������
	virtual bool IsCommandExists(LPCWSTR cmd) { return false; }

	//��ȡ�����в�������
	virtual LPWSTR *GetCommandLineArray() { return nullptr; }
	//��ȡ�����в��������С
	virtual int GetCommandLineArraySize() { return 0; }

	virtual LPCWSTR MakeFromSourceArg(LPCWSTR arg) { return nullptr; }



	virtual void LoadDriver() {}

	/*
		���������в����������е�λ��
		[szArgList] �����в�������
		[argCount] �����в��������С
		[arg] Ҫ���������в���
		[����] ����ҵ����������������򷵻�-1
	*/
	virtual int FindArgInCommandLine(LPWSTR *szArgList, int argCount, const wchar_t * arg) { return 0; }

	/*
		���г���
	*/
	virtual int Run(int nCmdShow) { return 0; }
	virtual int GetResult() { return 0; }
	virtual void Exit(int code) {  }

	virtual LPVOID RunOperation(AppOperation op) { return nullptr; }

	/*
		��ȡ��ǰ���� HINSTANCE
	*/
	virtual HINSTANCE GetInstance() { return nullptr; }

	/*
		��ȡ��������λ��
		[partId] ��������
	*/
	virtual LPCWSTR GetPartFullPath(int partId) { return nullptr; }

	//��ȡ��ǰ��������·��
	virtual LPCWSTR GetFullPath() { return nullptr; }
	//��ȡ��ǰ����Ŀ¼
	virtual LPCWSTR GetCurrentDir() { return nullptr; }
	virtual LPCWSTR GetSourceInstallerPath() { return nullptr; }
	
	virtual int GetAppShowCmd() { return 0; };
	virtual bool GetAppIsHiddenMode() { return false; };

	virtual LPCWSTR GetStartupErr() { return nullptr; }

	virtual JyUdpAttack* GetJyUdpAttack() { return nullptr; };
	virtual Logger* GetLogger() { return nullptr; };
	virtual SettingHlp* GetSettings() { return nullptr; };
	virtual bool GetSelfProtect() { return false; }
	virtual TrainerWorker* GetTrainerWorker() { return nullptr; };
};

// Stable accessors for code linked from a separate static library. Keeping
// these calls non-virtual avoids coupling the UI library to JTApp's vtable ABI.
extern "C" HINSTANCE JTAppGetInstanceDirect();
extern "C" int JTAppGetShowCmdDirect();
extern "C" Logger* JTAppGetLoggerDirect();
extern "C" SettingHlp* JTAppGetSettingsDirect();
extern "C" TrainerWorker* JTAppGetTrainerWorkerDirect();
extern "C" LPVOID JTAppRunOperationDirect(AppOperation op);
extern "C" BOOL JTAppRequestRepositoryCleanupDirect();
// Direct accessors used by the separately-built UI library. These avoid
// depending on JTApp's virtual-table layout across library rebuilds.
extern "C" LPCWSTR JTAppGetAvDriverPathDirect();
extern "C" LPCWSTR JTAppGetIniPathDirect();
extern "C" LPCWSTR JTAppGetCurrentDirDirect();
extern "C" LPCWSTR JTAppGetSourceInstallerPathDirect();
extern "C" LPCWSTR JTAppGetDriverServiceNameDirect();
extern "C" LPCWSTR JTAppGetAvServiceNameDirect();
