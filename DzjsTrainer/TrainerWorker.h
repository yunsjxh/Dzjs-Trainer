#pragma once
#include "stdafx.h"
#include <string>
#include <list>
#include <map>
#include <set>
#include "SysHlp.h"

enum TrainerWorkerOp {
	TrainerWorkerOpVirusBoom,
	TrainerWorkerOpVirusQuit,
	TrainerWorkerOp1,
	TrainerWorkerOpForceUnLoadVirus,
	TrainerWorkerOp2,
	TrainerWorkerOp3,
	TrainerWorkerOp4,
	TrainerWorkerOp5,
};

class TrainerWorkerCallback
{
public:
	
	enum TrainerStatus {
		TrainerStatusNotFound,
		TrainerStatusNotRunning,
		TrainerStatusUnknowProblem,
		TrainerStatusControllFailed,
		TrainerStatusControlled,
		TrainerStatusControlledAndUnLocked,
		TrainerStatusStopped
	};

	virtual void OnUpdateStudentMainInfo(bool running, LPCWSTR fullPath, DWORD pid, bool byuser) {}
	virtual void OnUpdateState(TrainerStatus status, LPCWSTR textMain, LPCWSTR textMore) {}
	// 其他电子教室宿主（自动适配注入的那些进程）有增减时回调。
	// count 是当前受控的宿主进程数，summary 形如「红蜘蛛 ×1、联想传奇 ×2」。
	virtual void OnUpdateEduHosts(int count, LPCWSTR summary) {}
	virtual void OnResolveBlackScreenWindow() {}
	virtual void OnSimpleMessageCallback(LPCWSTR text) {}
	virtual void OnBeforeSendStartConf() {}
	virtual void OnAllowGbTop() {}
	virtual void OnShowHelp() {}
	virtual void OnSwitchLockStatus(bool locked) {}
	virtual HWND GetMainHWND() { return NULL; }
};
class TrainerWorker
{
public:
	TrainerWorker() {}
	virtual ~TrainerWorker() {}

	virtual void Init() {}
	virtual void UpdateScreenSize() {}
	virtual void InitSettings() {}

	virtual void Start() {}
	virtual void Stop() {}
	virtual bool Running() { return false; }
	virtual DWORD GetStudentMainPid() { return 0; }
	virtual void* RunOperation(TrainerWorkerOp op) { return nullptr; }
	virtual bool SwitchFakeFull() { return false; }
	virtual bool FindProcess(LPCWSTR processName, DWORD*outPid) { return false; }
	virtual bool KillProcess(DWORD pid, bool force){ return false; }

	virtual bool Kill(bool autoWork = false) { return false;  }
	virtual bool Rerun(bool autoWork = false) { return false; }

	virtual void SetUpdateInfoCallback(TrainerWorkerCallback *callback) {}
	virtual void HandleMessageFromVirus(LPCWSTR buf) {}
	virtual void SendMessageToVirus(LPCWSTR buf) {}
	virtual bool AppointStudentMainLocation(LPCWSTR fullPath) { return false; }

	// 「进程注入」：target 可以是十进制 PID，也可以是进程名（如 notepad.exe）。
	// status 返回可直接显示给用户的中文结果；实现负责位数判断、黑名单与卸载清理。
	virtual bool InjectCustomProcess(LPCWSTR target, std::wstring* status) { return false; }
	virtual bool UninjectCustomProcess(LPCWSTR target, std::wstring* status) { return false; }
	virtual std::wstring GetCustomInjectedList() { return L""; }
	// 进程选择器用 PID 查「已注入」标记。返回 bool 而不是容器，避免跨 DLL 传 STL。
	virtual bool IsCustomInjected(DWORD pid) { return false; }

	// 「自动接管其他电子教室」（EduProducts.h 里登记的产品）。关掉只是不再
	// 自动注入新进程，已经注进去的宿主保持不动，等它们退出或被手动卸载。
	virtual void SetEduCompatAutoInject(bool enabled) {}
	virtual bool GetEduCompatAutoInject() { return true; }
	virtual int GetEduHostCount() { return 0; }
	virtual std::wstring GetEduHostSummary() { return L""; }
	// 让已注入的电子教室宿主重读 ini。界面上改了显示相关开关（信息流保护、
	// 允许监看）之后调用，否则要等宿主重启或者下一次保存设置才生效。
	virtual void SyncCustomHosts() {}
};

#ifdef JTEXPORT

struct IncorrectStudentMainFilterData {
	DWORD pid;
	bool checked;
};

class TrainerWorkerInternal : public TrainerWorker
{
public:
	TrainerWorkerInternal();
	~TrainerWorkerInternal();

	void Init();
	void InitSettings();

	void UpdateScreenSize();

	void Start();
	void Stop();
	void StopInternal();
	bool Running() { return _Running; }
	void* RunOperation(TrainerWorkerOp op);

	void SetUpdateInfoCallback(TrainerWorkerCallback *callback);
	void HandleMessageFromVirus(LPCWSTR buf);
	void SendMessageToVirus(LPCWSTR buf);
	bool AppointStudentMainLocation(LPCWSTR fullPath);
	DWORD GetStudentMainPid() { return _StudentMainPid; }

	bool InjectCustomProcess(LPCWSTR target, std::wstring* status) override;
	bool UninjectCustomProcess(LPCWSTR target, std::wstring* status) override;
	std::wstring GetCustomInjectedList() override;
	bool IsCustomInjected(DWORD pid) override;
	void SetEduCompatAutoInject(bool enabled) override;
	bool GetEduCompatAutoInject() override { return _EduCompatEnabled; }
	int GetEduHostCount() override;
	std::wstring GetEduHostSummary() override;
	void SyncCustomHosts() override;

	// 扫描 EduProducts.h 里登记的学生端进程，命中就按通用宿主注入。
	// 一台机器可以同时装几套电子教室，一轮扫描会把命中的目标全部注进去。
	// 只在 _EduCompatEnabled 打开时做事，失败的目标会被压住一段时间不重试。
	void RunEduCompatInject();
	// 受控宿主有增减时通知界面。调用点只有注入成功、卸载、清理死进程三处。
	void NotifyEduHosts();

	bool Kill(bool autoWork = false);
	bool KillStAuto();
	bool Rerun(bool autoWork = false);
private:

	TrainerWorkerCallback *_Callback = nullptr;
	bool _Running = false;
	HDESK hDesktop = NULL;

	bool _VirusInstalled = false;
	DWORD _StudentMainPid = 0;
	DWORD _MasterHelperPid = 0;
	std::wstring _StudentMainPath;
	bool _StudentMainFileLocated = false;
	bool _StudentMainFileLocatedByProcess = false;
	bool _StudentMainControlled = false;
	bool _StudentMainRunningLock = false;
	std::wstring _StatusTextMain;
	std::wstring _StatusTextMore;
	std::wstring _TopDomainPassword;

	std::list<IncorrectStudentMainFilterData> incorrectStudentMainPids;

	// 用户通过「进程注入」显式指定的目标；退出时统一卸载，不自动扩散到子进程。
	struct CustomInjectedProcess {
		DWORD pid = 0;
		std::wstring name;
		bool is64 = false;
		// 命中 EduProducts.h 的产品名（如「红蜘蛛」）。手动注入的普通进程为空，
		// 界面据此把自动接管的目标和用户自己选的分开显示。
		std::wstring product;
	};
	std::list<CustomInjectedProcess> _CustomInjected;

	// 电子教室自动适配（见 EduProducts.h）。扫描本身有 10 秒间隔，失败目标
	// 再压 10 分钟，避免「2 秒一次的重扫」把注入失败刷满日志。
	bool _EduCompatEnabled = true;
	ULONGLONG _LastEduCompatScan = 0;
	std::map<DWORD, ULONGLONG> _EduInjectBlocked;
	// 用户在「进程注入」里手动卸载过的目标：同一个进程不再自动注入回去，
	// 进程退出后自动解禁（PID 会被复用，不能用它做长期标记）。
	std::set<DWORD> _EduManualSkip;
	// 64 位注入助手的等待上限。手动注入时用户知道自己在等，给足 40 秒；
	// 自动扫描是后台行为，卡住的是界面线程，所以压到几秒就放弃、下一轮再试。
	DWORD _Injector64TimeoutMs = 40000;
	bool ResolveCustomTarget(LPCWSTR target, DWORD* outPid, std::wstring* outName, std::wstring* status);
	bool IsSelfCustomTarget(DWORD pid, LPCWSTR name, std::wstring* status);
	bool DetectTargetBitness(DWORD pid, bool* outIs64, std::wstring* status);
	bool RunInjector64(DWORD pid, LPCWSTR dllPath, bool unload, std::wstring* status);
	void UnloadAllCustomInjected();

	HHOOK hMsgBoxHook;
	HWND hWndMain;

	bool setAllowGbTop = false;
	bool setAutoIncludeFullWindow = false;
	int setCkInterval = 3100;
	int screenWidth, screenHeight;
	bool gbFullManual = false;

	int _ControlHandshakeAttempts = 0;

	bool _FakeFull = false;
	bool _LastResoveBroadcastWindow = false, _LastResoveBlackScreenWindow = false;
	bool _FirstBlackScreenWindow = false;
	int _LastResolveWindowCount = 0;
	bool _FakeBroadcastFull = false, _FakeBlackScreenFull = false;
	HWND _CurrentBroadcastWnd, _CurrentBlackScreenWnd;

	bool RunCk();
	void RunResetPid();

	/* Locate and found*/

	bool FlushProcess();
	void ClearProcess();
	bool FindProcess(LPCWSTR processName, DWORD*outPid);
	bool KillProcess(DWORD pid, bool force);

	bool ReadTopDomanPassword(BOOL forceKnock);
	bool LocateStudentMainLocation();
	bool LocateStudentMain(DWORD *outFirstPid);
	bool LocateMasterHelper(DWORD *outFirstPid);
	bool CheckPidFilterd(DWORD pid);
	void AddFilterdPid(DWORD pid);
	void ClearFilterdPid();

	/* Update ui*/

	void UpdateStudentMainInfo(bool byuser);
	void UpdateState();

	/*Virus*/

	bool InstallVirus();
	bool InstallVirusForMaster();
	bool InjectDll(DWORD pid, LPCWSTR dllPath);
	bool UnInjectDll(DWORD pid, LPCWSTR moduleName);
	bool UnLoadAllVirus();
	bool UnLoadNetFilterDriver();
	bool UnLoadJiYuProtectDriver();

	/*Window resolver*/

	bool SwitchFakeFull();
	void FakeFull(bool fk);
	void ManualFull(bool fk);
	void ManualTop(bool fk);
	bool ChecIsJIYuWindow(HWND hWnd, LPDWORD outPid, LPDWORD outTid);
	bool CheckIsTargetWindow(LPWSTR text, HWND hWnd);
	bool CheckWindowTextIsGb(const wchar_t* text);
	void FixWindow(HWND hWnd, LPWSTR text);

	static TrainerWorkerInternal * currentTrainerWorker;

	static BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam);
	static VOID CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD);
	static VOID CALLBACK MsgBoxCallback(LPHELPINFO lpHelpInfo);
	static LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam);
};

#endif

bool UnDecryptJiyuKnock(BYTE * Data, DWORD cbData, WCHAR *ss);
