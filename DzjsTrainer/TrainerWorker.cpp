#include "stdafx.h"
#include "TrainerWorker.h"
#include "DllInjector/DllInjector.h"
#include "DzjsTrainer.h"
#include "AppPublic.h"
#include "NtHlp.h"
#include "PathHelper.h"
#include "StringHlp.h"
#include "MsgCenter.h"
#include "StringSplit.h"
#include "DriverLoader.h"
#include "KernelUtils.h"
#include "SysHlp.h"
#include "SettingHlp.h"
#include "RegHlp.h"
#include "EduProducts.h"
#include <TlHelp32.h>

extern JTApp *currentApp;
extern LoggerInternal * currentLogger;

TrainerWorkerInternal * TrainerWorkerInternal::currentTrainerWorker = nullptr;

extern NtQuerySystemInformationFun NtQuerySystemInformation;

using namespace std;

#define TIMER_RESET_PID 40115
#define TIMER_CK 40116

PSYSTEM_PROCESSES current_system_process = NULL;

TrainerWorkerInternal::TrainerWorkerInternal()
{
	currentTrainerWorker = this;

	//For message box center
	hMsgBoxHook = SetWindowsHookEx(
		WH_CBT,        // Type of hook 
		CBTProc,        // Hook procedure (see below)
		NULL,         // Module handle. Must be NULL (see docs)
		GetCurrentThreadId()  // Only install for THIS thread!!!
	);
}
TrainerWorkerInternal::~TrainerWorkerInternal()
{
	if (hDesktop) {
		CloseDesktop(hDesktop);
		hDesktop = NULL;
	}

	UnhookWindowsHookEx(hMsgBoxHook);

	StopInternal();
	ClearProcess();

	currentTrainerWorker = nullptr;
}

void TrainerWorkerInternal::Init()
{
	hDesktop = OpenDesktop(L"Default", 0, FALSE, DESKTOP_ENUMERATE);
	UpdateScreenSize();

	if (LocateStudentMainLocation()) currentLogger->Log(L"已定位极域电子教室位置： %s", _StudentMainPath.c_str());
	else currentLogger->LogWarn(L"无法定位极域电子教室位置");

	UpdateState();
	UpdateStudentMainInfo(false);

	InitSettings();
}
void TrainerWorkerInternal::InitSettings()
{
	SettingHlp*settings = currentApp->GetSettings();
	setAutoIncludeFullWindow = settings->GetSettingBool(L"AutoIncludeFullWindow");
	setCkInterval = currentApp->GetSettings()->GetSettingInt(L"CKInterval", 3100);
	setAllowGbTop = settings->GetSettingBool(L"AllowGbTop", false);
	if (setCkInterval < 1000 || setCkInterval > 10000) setCkInterval = 3000;

	if (_StudentMainControlled) {
		SendMessageToVirus(L"hk:reset");
		// 视频流保护仅由当前 GUI 会话下发，重新初始化时必须保持关闭，
		// 不能从历史 INI 恢复。
		bool allowMonitor = settings->GetSettingBool(L"AllowMonitor", true);
		FAST_STR_BINDER(monStr, L"hk:setmonitor:%d", 32, allowMonitor ? 1 : 0);
		SendMessageToVirus(monStr);
		SendMessageToVirus(L"hk:setvideo:0");
		SendMessageToVirus(L"hk:setvideomode:image");
	}

	// 第三方电子教室宿主没有极域的握手机制，上面那几条 hk: 命令到不了它们，
	// 它们在注入时只读过一次 ini。补一条 reset：对端会重读 ini、按新的
	// AllowMonitor / EnableVideoModify 补装钩子，界面上刚改的开关才真的生效。
	SyncCustomHosts();
}
void TrainerWorkerInternal::UpdateScreenSize()
{
	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void TrainerWorkerInternal::Start()
{
	if (!_Running) 
	{
		_Running = true;

		//Settings
		
		SetTimer(hWndMain, TIMER_CK, setCkInterval, TimerProc);
		SetTimer(hWndMain, TIMER_RESET_PID, 2000, TimerProc);

		// 其他电子教室产品的自动适配，默认开。自动往第三方进程里装钩子毕竟
		// 是个有副作用的事，留一个 ini 开关给不想用的人关掉。
		if (currentApp && currentApp->GetSettings())
			_EduCompatEnabled = currentApp->GetSettings()->GetSettingBool(L"EduCompatAutoInject", true);
		currentLogger->LogInfo(L"EduCompat autoInject=%d products=%d", _EduCompatEnabled ? 1 : 0, kEduProductCount);

		UpdateState();

	}
}
void TrainerWorkerInternal::Stop()
{
	if (_Running) {
		StopInternal();
		UpdateState();
	}
}
void TrainerWorkerInternal::StopInternal() {
	if (_Running) {
		_Running = false;
		KillTimer(hWndMain, TIMER_CK);
		KillTimer(hWndMain, TIMER_RESET_PID);
	}
	// 退出时把用户显式注入的进程全部卸载，避免残留钩子。
	UnloadAllCustomInjected();
}

void TrainerWorkerInternal::SetUpdateInfoCallback(TrainerWorkerCallback *callback)
{
	if (callback) {
		_Callback = callback;
		hWndMain = callback->GetMainHWND();
	}
}

void TrainerWorkerInternal::HandleMessageFromVirus(LPCWSTR buf)
{
	wstring act(buf);
	vector<wstring> arr;
	SplitString(act, arr, L":");
	if (arr.size() >= 2)
	{
		if (arr[0] == L"hkb")
		{
			if (arr[1] == L"succ") {
				bool firstHandshake = !_StudentMainControlled;
				_StudentMainControlled = true;
				_ControlHandshakeAttempts = 0;
				currentLogger->LogInfo(L"Receive ctl success message first=%d", firstHandshake);
				if (!firstHandshake)
					return;
				if (_Callback) _Callback->OnBeforeSendStartConf();
				UpdateState();

				//HS 
				FAST_STR_BINDER(str, L"hs:%ld", 32, (ULONG_PTR)hWndMain);
				SendMessageToVirus(str);
			}
			else if (arr[1] == L"immck") {
				RunCk();
				currentLogger->LogInfo(L"Receive  immck message ");
			}
			else if (arr[1] == L"jyk") {
				if (arr.size() >= 3) {
					_StudentMainRunningLock = arr[2] == L"1";
					UpdateState();
				}
			}
			else if (arr[1] == L"algbtop") {
				currentLogger->LogInfo(L"Receive allow gbTop message ");
				_Callback->OnAllowGbTop();
			}
			else if (arr[1] == L"gbmfull") {
				gbFullManual = true;
				ManualFull(gbFullManual);
			}
			else if (arr[1] == L"gbmnofull") {
				if (_FakeFull) FakeFull(false);
				gbFullManual = false;
				ManualFull(gbFullManual);
			}
			else if (arr[1] == L"gbuntop") ManualTop(false);
			else if (arr[1] == L"gbtop") ManualTop(true);
			else if (arr[1] == L"showhelp" && _Callback) _Callback->OnShowHelp();
			// 注入的宿主在 API 层挡下了一次关机。带上的第三个字段是拦在哪个
			// 函数上，只进日志；界面统一显示一句「已拦截关机请求」。
			else if (arr[1] == L"shutdown") {
				LPCWSTR api = arr.size() >= 3 ? arr[2].c_str() : L"unknown";
				currentLogger->LogWarn(L"Blocked a shutdown request in the host process api=%s", api);
				if (_Callback) _Callback->OnShutdownBlocked(api);
			}
			// 注入宿主里的一次全屏/置顶询问：active = 询问已就绪，其余是答复。
			// 宿主进程自己的钩子日志落在它的安装目录里，用户看不到，所以把结论
			// 记进主日志 —— 排查「明明该弹框却没弹」时全靠这一行。
			else if (arr[1] == L"eduop" && arr.size() >= 3) {
				if (arr[2] == L"active")
					currentLogger->LogInfo(L"Window op prompt ready in edu host");
				else
					currentLogger->LogInfo(L"Window op prompt result=%s hwnd=%s",
						arr[2].c_str(), arr.size() >= 4 ? arr[3].c_str() : L"?");
			}
			else if (arr[1] == L"wtf") {
				//插入了错误的目标，现在重置状态
				DWORD pid = _wtol(arr[2].c_str());
				AddFilterdPid(pid);

				if (_StudentMainFileLocatedByProcess)
					_StudentMainFileLocated = false;
				_StudentMainPid = 0;

				currentLogger->Log(L"插入了奇怪的 StudentMain.exe");

				UpdateState();
				UpdateStudentMainInfo(false);
			}
		}
		else if (arr[0] == L"wcd")
		{
			//wwcd
			int wcdc = _wtoi(arr[1].c_str());
			if (wcdc % 20 == 0)
				currentLogger->LogInfo(L"Receive watch dog message %d ", wcdc);
		}
		else if (arr[0] == L"vback") {
			wstring strBuff = arr[1];
			if (arr.size() > 2) {
				for (UINT i = 2; i < arr.size(); i++)
					strBuff += L":" + arr[i];
			}
			currentLogger->Log(L"Receive virus echo : %s", strBuff.c_str());
		}
	}
}
void TrainerWorkerInternal::SendMessageToVirus(LPCWSTR buf)
{
	MsgCenterSendResult result = MsgCenterSendToVirus(buf, hWndMain);
	if (!result.delivered)
		currentLogger->LogInfo(L"Control message send failed command=%s windowFound=%d error=%lu elapsedMs=%llu",
			buf, result.windowFound, result.error, result.elapsedMs);
}

bool TrainerWorkerInternal::Kill(bool autoWork)
{
	if (_StudentMainPid <= 4) {
		currentLogger->LogError(L"未找到极域主进程");
		return false;
	}
	// 这里原来会弹一个「您是否希望使用病毒进行爆破？」的确认框，回答「是」只是
	// 往注入的 DLL 发一条 ss2:0 让它别再拦广播，进程还在。卡片上写的是「结束」，
	// 那就该真的结束，不再多问一句。

	// The advanced selector applies to the termination path.
	std::wstring killMode = currentApp->GetSettings()->GetSettingStr(L"KillProcess", L"NtTerminateProcess");
	if (killMode != L"NtTerminateProcess") {
		const bool killed = KillProcess(_StudentMainPid, autoWork);
		if (killed) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
		}
		return killed;
	}

	HANDLE hProcess;
	NTSTATUS status = MOpenProcessNt(_StudentMainPid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			return true;
		}
		else {
			currentLogger->LogError(L"打开进程错误：0x%08X，请手动结束", status);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		_StudentMainPid = 0;
		_StudentMainControlled = false;
		UpdateState();
		UpdateStudentMainInfo(!autoWork);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) goto FORCEKILL;
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			currentLogger->LogError(L"结束进程错误：0x%08X，请手动结束", status);
			if (!autoWork)
				MessageBox(hWndMain, L"无法结束极域电子教室，您需要使用其他工具手动结束", L"DzjsTrainer - 错误", MB_ICONERROR);;
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
	}

FORCEKILL:
	if (XDriverLoaded() && MessageBox(hWndMain, L"普通无法结束极域，是否调用驱动结束极域？\n（驱动可能不稳定，请慎用。您也可以使用 PCHunter 等安全软件进行强杀）", L"DzjsTrainer - 提示", MB_ICONEXCLAMATION | MB_YESNO | MB_TOPMOST) == IDYES)
	{
		if (KForceKill(_StudentMainPid, &status)) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
		else if(!autoWork) MessageBox(hWndMain, L"驱动也无法结束，请使用 PCHunter 结束它吧！", L"错误", MB_ICONEXCLAMATION);
	}
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::KillStAuto()
{
	RunCk();
	RunResetPid();

	return Kill(true);
}
bool TrainerWorkerInternal::Rerun(bool autoWork)
{
	if (!_StudentMainFileLocated) {
		currentLogger->LogWarn(L"未找到极域电子教室");
		if (!autoWork && _Callback)
			_Callback->OnSimpleMessageCallback(L"<h5>我们无法在此计算机上找到极域电子教室，您需要手动启动</h5>");
		return false;
	}
	return  SysHlp::RunApplication(_StudentMainPath.c_str(), NULL);
}
void* TrainerWorkerInternal::RunOperation(TrainerWorkerOp op) 
{
	switch (op)
	{
	case TrainerWorkerOpVirusBoom: {
		MsgCenterSendToVirus(L"ss:0", hWndMain);
		return nullptr;
	}
	case TrainerWorkerOpVirusQuit: {
		MsgCenterSendToVirus((LPWSTR)L"hk:ckend", hWndMain);
		return nullptr;
	}
	case TrainerWorkerOp1: {
		WCHAR s[300]; swprintf_s(s, L"hk:path:%s", currentApp->GetFullPath());
		MsgCenterSendToVirus(s, hWndMain);
		swprintf_s(s, L"hk:inipath:%s", currentApp->GetPartFullPath(PART_INI));
		MsgCenterSendToVirus(s, hWndMain);
		break;
	}
	case TrainerWorkerOpForceUnLoadVirus: {
		UnLoadAllVirus();
		break;
	}
	case TrainerWorkerOp2: {
		if (ReadTopDomanPassword(false))
			return (LPVOID)_TopDomainPassword.c_str();
		return nullptr;
	}
	case TrainerWorkerOp3: {
		if (ReadTopDomanPassword(true)) 
			return (LPVOID)_TopDomainPassword.c_str();
		return nullptr;
	}
	case TrainerWorkerOp4: {
		return (void*)UnLoadJiYuProtectDriver();
	}
	case TrainerWorkerOp5: {
		return (void*) UnLoadNetFilterDriver();
	}
	}
	return nullptr;
}

bool TrainerWorkerInternal::RunCk()
{
	_LastResolveWindowCount = 0;
	_LastResoveBroadcastWindow = false;
	_LastResoveBlackScreenWindow = false;
	_FirstBlackScreenWindow = false;

	EnumDesktopWindows(hDesktop, EnumWindowsProc, (LPARAM)this);

	MsgCenterSendHWNDS(hWndMain);

	return _LastResolveWindowCount > 0;
}
void TrainerWorkerInternal::RunResetPid()
{
	FlushProcess();

	//Find jiyu main process
	DWORD newPid = 0;
	if (LocateStudentMain(&newPid)) { //找到极域

		if (_StudentMainPid != newPid)
		{
			_StudentMainPid = newPid;
			_VirusInstalled = false;
			_StudentMainControlled = false;
			_ControlHandshakeAttempts = 0;

			if (InstallVirus()) {
				_VirusInstalled = true;

				currentLogger->Log(L"向 StudentMain.exe [%d] 注入DLL成功", newPid);
			}
			else  currentLogger->LogError(L"向 StudentMain.exe [%d] 注入DLL失败", newPid);

			currentLogger->Log(L"已锁定 StudentMain.exe [%d]", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}
		if (_VirusInstalled && !_StudentMainControlled && _ControlHandshakeAttempts < 60) {
			++_ControlHandshakeAttempts;
			MsgCenterSendResult result = MsgCenterSendToVirus(L"hk:ckstat", hWndMain);
			currentLogger->LogInfo(L"Control handshake attempt=%d windowFound=%d delivered=%d error=%lu elapsedMs=%llu",
				_ControlHandshakeAttempts, result.windowFound, result.delivered, result.error, result.elapsedMs);
		}
	}
	else { //没有找到

		if (_StudentMainPid != 0)
		{
			_StudentMainPid = 0;
			_VirusInstalled = false;
			_StudentMainControlled = false;
			_ControlHandshakeAttempts = 0;

			currentLogger->Log(L"极域主进程 StudentMain.exe 已退出", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}

	}

	// MasterHelper injection is controlled by the advanced settings. The old
	// implementation was left commented out, so both UI switches were inert.
	SettingHlp* settings = currentApp->GetSettings();
	const bool injectMaster = settings->GetSettingBool(L"InjectMasterHelper", false);
	const bool injectHelper64 = settings->GetSettingBool(L"InjectProcHelper64", false) && SysHlp::Is64BitOS();
	if (injectMaster || injectHelper64) {
		newPid = 0;
		if (LocateMasterHelper(&newPid)) {
			if (_MasterHelperPid != newPid) {
				_MasterHelperPid = newPid;
				if (InstallVirusForMaster()) currentLogger->Log(L"向 MasterHelper.exe [%d] 注入 DLL 成功", newPid);
				else currentLogger->LogError(L"向 MasterHelper.exe [%d] 注入 DLL 失败", newPid);
			}
		}
		else {
			_MasterHelperPid = 0;
		}
	}
	else if (_MasterHelperPid > 4) {
		UnInjectDll(_MasterHelperPid, L"DzjsTrainerHooks.dll");
		_MasterHelperPid = 0;
	}

	// 结束之后先看住，再谈自动注入：被拉起来的目标不能一边杀一边又注回去。
	StepEduKillWatch();
	RunEduCompatInject();
}

bool TrainerWorkerInternal::FlushProcess()
{
	ClearProcess();

	DWORD requiredSize = 0;
	NTSTATUS status = NtQuerySystemInformation(
		SystemProcessInformation, NULL, 0, &requiredSize);
	if (status != STATUS_INFO_LENGTH_MISMATCH || requiredSize == 0) {
		currentLogger->LogError2(
			L"NtQuerySystemInformation size query failed ! 0x%08X", status);
		return false;
	}

	DWORD bufferSize = requiredSize;
	for (int attempt = 0; attempt < 6; ++attempt) {
		// The process list can grow between the size query and data query.
		DWORD headroom = bufferSize / 4;
		if (headroom < 64 * 1024) headroom = 64 * 1024;
		if (bufferSize > MAXDWORD - headroom) {
			currentLogger->LogError2(L"System process buffer size overflow");
			return false;
		}
		bufferSize += headroom;

		PSYSTEM_PROCESSES buffer =
			(PSYSTEM_PROCESSES)malloc(bufferSize);
		if (!buffer) {
			currentLogger->LogError2(
				L"Failed to allocate system process buffer (%lu bytes)", bufferSize);
			return false;
		}

		DWORD returnedSize = 0;
		status = NtQuerySystemInformation(
			SystemProcessInformation, buffer, bufferSize, &returnedSize);
		if (NT_SUCCESS(status)) {
			current_system_process = buffer;
			return true;
		}

		free(buffer);
		if (status != STATUS_INFO_LENGTH_MISMATCH) {
			currentLogger->LogError2(
				L"NtQuerySystemInformation failed ! 0x%08X", status);
			return false;
		}

		if (returnedSize > bufferSize) bufferSize = returnedSize;
	}

	currentLogger->LogError2(
		L"NtQuerySystemInformation failed after retries ! 0x%08X", status);
	return false;
}
void TrainerWorkerInternal::ClearProcess()
{
	if (current_system_process) {
		free(current_system_process);
		current_system_process = NULL;
	}
}
bool TrainerWorkerInternal::FindProcess(LPCWSTR processName, DWORD * outPid)
{
	return false;
}
bool TrainerWorkerInternal::KillProcess(DWORD pid, bool force)
{
	const std::wstring mode = currentApp->GetSettings()->GetSettingStr(L"KillProcess", L"NtTerminateProcess");
	if (mode == L"TerminateProcess") {
		HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) {
			currentLogger->LogError2(L"TerminateProcess 打开进程 [%d] 失败", pid);
			return false;
		}
		const BOOL terminated = ::TerminateProcess(process, 0);
		const DWORD error = terminated ? ERROR_SUCCESS : GetLastError();
		CloseHandle(process);
		if (terminated) currentLogger->Log(L"TerminateProcess 结束进程 [%d] 成功", pid);
		else currentLogger->LogError2(L"TerminateProcess 结束进程 [%d] 失败", pid);
		return terminated != FALSE;
	}
	if (mode == L"KernelMode") {
		if (!XDriverLoaded()) {
			currentLogger->LogWarn(L"Kernel Mode 结束失败：驱动未加载");
			return false;
		}
		NTSTATUS status = STATUS_UNSUCCESSFUL;
		if (KForceKill(pid, &status)) {
			currentLogger->Log(L"Kernel Mode 结束进程 [%d] 成功", pid);
			return true;
		}
		currentLogger->LogError2(L"Kernel Mode 结束进程 [%d] 失败：0x%08X", pid, status);
		return false;
	}

	HANDLE hProcess;
	// 这个函数也用来结束别的进程（例如注入的电子教室宿主），不能拿 _StudentMainPid
	// 顶替入参 —— 那样点「结束」会去杀极域，而目标进程安然无恙。
	NTSTATUS status = MOpenProcessNt(pid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"找不到进程 [%d] ", pid);
			return true;
		}
		else {
			currentLogger->LogError2(L"打开进程 [%d] 错误：0x%08X，请手动结束", pid);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		currentLogger->Log(L"进程 [%d] 结束成功", pid);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) {
			if (force) goto FORCEKILL;
			else currentLogger->LogError2(L"结束进程 [%d] 错误：拒绝访问。可尝试使用驱动结束", pid);
			CloseHandle(hProcess);
		}
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"结束进程 [%d] 错误：0x%08X，请手动结束", pid);
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"找不到进程 [%d] ", pid);
			CloseHandle(hProcess);
			return true;
		}
	}
FORCEKILL:
	if (XDriverLoaded())
	{
		if (KForceKill(pid, &status)) {
			currentLogger->Log(L"进程 [%d] 强制结束成功", pid);
			CloseHandle(hProcess);
			return true;
		}
		else {
			currentLogger->LogError2(L"驱动强制结束进程 [%d] 错误：0x%08X", pid);
		}
	}
	else currentLogger->Log(L"驱动未加载，无法强制结束进程");
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::ReadTopDomanPassword(BOOL forceKnock)
{
	_TopDomainPassword.clear();

	HKEY hKey;
	LRESULT lastError;

	if (forceKnock) goto READ_EX;
	//普通注册表读取，适用于4.0版本

	WCHAR Data[32];
	if (MRegReadKeyString64And32(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\TopDomain\\e-Learning Class Standard\\1.00",
		L"SOFTWARE\\Wow6432Node\\TopDomain\\e-Learning Class Standard\\1.00", L"UninstallPasswd", Data, 32)) {
		
		if (StrEqual(Data, L"Passwd[123456]")) goto READ_EX; //6.0以后读取不了了，都显示Passwd[123456]，用新的方法读取
		else {
			_TopDomainPassword = Data;
			_TopDomainPassword = _TopDomainPassword.substr(6, _TopDomainPassword.size() - 6);
			return true;
		}

	}
	else currentLogger->LogWarn2(L"MRegReadKeyString64And32 Failed : %s (%d)", PRINT_LAST_ERROR_STR);

	//HKEY_LOCAL_MACHINE\SOFTWARE\TopDomain\e-Learning Class\Student Knock1
READ_EX:

	lastError = RegOpenKeyEx(HKEY_LOCAL_MACHINE, SysHlp::Is64BitOS() ?
		L"SOFTWARE\\Wow6432Node\\TopDomain\\e-Learning Class\\Student" : 
		L"SOFTWARE\\TopDomain\\e-Learning Class\\Student", 0, KEY_WOW64_64KEY | KEY_READ, &hKey);
	if (lastError == ERROR_SUCCESS) {

		DWORD dwType = REG_BINARY;
		BYTE Data[120];
		DWORD cbData = 120;
		lastError = RegQueryValueEx(hKey, L"Knock1", 0, &dwType, (LPBYTE)Data, &cbData);
		if (lastError == ERROR_SUCCESS) {

			RegCloseKey(hKey);

			WCHAR ss[34] = { 0 };
			if (UnDecryptJiyuKnock(Data, cbData, ss)) {
				_TopDomainPassword = ss;
				return true;
			}
			else {
				currentLogger->LogWarn2(L"UnDecryptJiyuKnock failed !");
				return false;
			}
		}
		else currentLogger->LogWarn2(L"RegQueryValueEx Failed : %d", lastError);
		RegCloseKey(hKey);
	}
	else currentLogger->LogWarn2(L"RegOpenKeyEx Failed : %d", lastError);
	return false;
}
bool TrainerWorkerInternal::AppointStudentMainLocation(LPCWSTR fullPath) {
	if (Path::GetFileName(fullPath) != L"StudentMain.exe")	return false;
	if (Path::Exists(fullPath)) 
	{
		_StudentMainPath = fullPath;
		_StudentMainFileLocated = true;
		_StudentMainFileLocatedByProcess = false;

		currentApp->GetSettings()->SetSettingStr(L"StudentMainPath", fullPath);
		currentLogger->Log(L"成功手动定位极域电子教室位置： %s", fullPath);

		UpdateStudentMainInfo(false);
		return true;
	}
	return false;
}
bool TrainerWorkerInternal::LocateStudentMainLocation()
{
	//注册表查找 极域 路径
	WCHAR Data[MAX_PATH];
	if (MRegReadKeyString64And32(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\e-Learning Class V6.0",
		L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\e-Learning Class V6.0", 
		L"DisplayIcon", Data, MAX_PATH)) {

		if (Path::Exists(Data)) {
			_StudentMainPath = Data;
			_StudentMainFileLocated = true;
			return true;
		}
		else currentLogger->Log(L"读取注册表 [DisplayIcon] 获得了一个无效的极域电子教室路径 : %s", Data);
	}
	else currentLogger->LogWarn2(L"MRegReadKeyString64And32 Failed : %s (%d)", PRINT_LAST_ERROR_STR);

	//读取用户指定的路径
	wstring appointStudentMainPath  = currentApp->GetSettings()->GetSettingStr(L"StudentMainPath", L"", MAX_PATH);
	if (!StrEmepty(appointStudentMainPath.c_str()) && Path::Exists(appointStudentMainPath)) {
		wstring fileName = Path::GetFileName(appointStudentMainPath);
		if (StrEqual(fileName.c_str(), L"StudentMain.exe")) {
			_StudentMainPath = appointStudentMainPath;
			_StudentMainFileLocated = true;
			return true;
		}
	}

	//直接尝试查找
	LPCWSTR mabeInHere[6] = {
		L"c:\\Program Files\\Mythware\\极域课堂管理系统软件V6.0 2016 豪华版\\StudentMain.exe",
		L"C:\\Program Files\\Mythware\\e-Learning Class\\StudentMain.exe",
		L"C:\\Program Files (x86)\\Mythware\\极域课堂管理系统软件V6.0 2016 豪华版\\StudentMain.exe",
		L"C:\\Program Files (x86)\\Mythware\\e - Learning Class\\StudentMain.exe",
		L"C:\\e-Learning Class\\StudentMain.exe",
		L"c:\\极域课堂管理系统软件V6.0 2016 豪华版\\StudentMain.exe",
	};
	for (int i = 0; i < 6; i++) {
		if (Path::Exists(mabeInHere[i])) {
			_StudentMainPath = mabeInHere[i];
			_StudentMainFileLocated = true;
			_StudentMainFileLocatedByProcess = false;
			return true;
		}
	}

	return false;
}
bool TrainerWorkerInternal::LocateStudentMain(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"StudentMain.exe"))
			{
				if (CheckPidFilterd((DWORD)p->ProcessId)) continue;

				HANDLE hProcess;
				if (NT_SUCCESS(MOpenProcessNt((DWORD)p->ProcessId, &hProcess))) {
					WCHAR buffer[MAX_PATH];
					if (MGetProcessFullPathEx(hProcess, buffer)) {
						//检查exe相同目录下是否存在 LibTDMaster.dll 不存在则排除
						PathRemoveFileSpec(buffer);
						wcscat_s(buffer, L"\\LibTDMaster.dll");
						if (!PathFileExists(buffer)) {
							AddFilterdPid((DWORD)p->ProcessId);
							currentLogger->Log(L"奇怪的 StudentMain.exe [%d]", (DWORD)p->ProcessId);
							continue;
						}

						//Exe 确定位置
						if (!_StudentMainFileLocated) {
							currentLogger->Log(L"通过进程 StudentMain.exe [%d] 定位到位置： %s", (DWORD)p->ProcessId, _StudentMainPath);
							_StudentMainPath = buffer;
							_StudentMainFileLocated = true;
							_StudentMainFileLocatedByProcess = true;
						}
					}
					CloseHandle(hProcess);
				}

				if (outFirstPid) *outFirstPid = (DWORD)p->ProcessId;

				ClearFilterdPid();
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
		ClearFilterdPid();
	}
	return false;
}
bool TrainerWorkerInternal::LocateMasterHelper(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"MasterHelper.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}
bool TrainerWorkerInternal::CheckPidFilterd(DWORD pid)
{
	for (auto it = incorrectStudentMainPids.begin(); it != incorrectStudentMainPids.end(); it++) {
		if ((*it).pid == pid)
			return true;
	}
	return false;
}
void TrainerWorkerInternal::AddFilterdPid(DWORD pid)
{
	IncorrectStudentMainFilterData data;
	data.checked = true;
	data.pid = pid;
	incorrectStudentMainPids.push_back(data);
}
void TrainerWorkerInternal::ClearFilterdPid()
{
	if (current_system_process)
	{
		for (auto it = incorrectStudentMainPids.begin(); it != incorrectStudentMainPids.end(); it++) {
			auto pid = (*it).pid;
			bool founded = false;
			bool done = false;
			for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
				if ((DWORD)p->ProcessId == pid)
				{
					founded = true;
					break;
				}
				done = p->NextEntryOffset == 0;
			}
			if (!founded) 
				it = incorrectStudentMainPids.erase(it);
		}
	}
}


void TrainerWorkerInternal::UpdateStudentMainInfo(bool byUser)
{
	if (_Callback)
		_Callback->OnUpdateStudentMainInfo(_StudentMainPid > 4, _StudentMainPath.c_str(), _StudentMainPid, byUser);
}
void TrainerWorkerInternal::UpdateState()
{
	if (_Callback) 
	{
		TrainerWorkerCallback::TrainerStatus status;
		// 其他电子教室（伽卡他卡 / 红蜘蛛 / 联想传奇）只在这张表里有记录。
		// 标题原来一律写「极域电子教室 ……」：只装别家教室的机器会一直显示
		// 「极域未运行」，而钩子其实早就装进宿主进程了。这里两边合起来报。
		const int eduHosts = GetEduHostCount();
		const std::wstring eduSummary = eduHosts > 0 ? GetEduHostSummary() : std::wstring();
		_StatusTextMore.clear();

		if (_StudentMainPid > 4) {
			if (_StudentMainControlled) {
				_StatusTextMain = L"已控制极域电子教室";

				if (!_Running) {
					_StatusTextMain += L" 但控制器未启动";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_StudentMainRunningLock)
				{
					_StatusTextMore = L"已为您解锁极域电子教室";
					if (eduHosts > 0) _StatusTextMore += L"，同时接管了 " + eduSummary;
					_StatusTextMore += L"<br/>您可以放心继续您的工作";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlledAndUnLocked;
				}
				else {
					if (eduHosts > 0) _StatusTextMore = L"同时接管了 " + eduSummary + L"。";
					_StatusTextMore += L"您可以放心继续您的工作";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlled;
				}
			}
			else {
				_StatusTextMain = L"无法控制极域电子教室";
				if (!_Running) {
					_StatusTextMain = L"控制器未启动";
					_StatusTextMore = L"您已手动停止控制器<br / >当前不会对极域做任何操作";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_VirusInstalled) {
					_StatusTextMore = L"毒已插入极域，但未正常运行<br / ><span style=\"color:#f41702\">您可能需要重新启动极域</span>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusUnknowProblem;
				}
				else {
					_StatusTextMore = L"向极域电子教室插入病毒失败<br / >错误详情请查看 <a id=\"link_log\">日志</a>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControllFailed;
				}
				// 极域没拿下、别家教室却接管成功是常见组合（极域有驱动自保），
				// 这句话得说出来，否则用户只看到「无法控制」就以为工具废了。
				if (eduHosts > 0)
					_StatusTextMore = L"其他电子教室已接管 " + eduSummary + L"。<br/>" + _StatusTextMore;
			}
		}
		else if (eduHosts > 0) {
			// 极域没装也没跑，但其他教室的学生端已经在钩子里。这属于正常受控，
			// 不该再报「极域电子教室未运行」——防护确实在工作，只是对象换了。
			// 标题直呼产品名；同时来了几款才笼统说「多个」，完整清单在概览第二张卡。
			std::wstring single;
			bool mixed = false;
			for (const auto& item : _CustomInjected) {
				if (item.product.empty()) continue;
				if (single.empty()) single = item.product;
				else if (item.product != single) { mixed = true; break; }
			}
			if (!_Running) {
				_StatusTextMain = L"控制器未启动";
				_StatusTextMore = L"已接管 " + eduSummary + L"<br / >您已手动停止控制器，当前不会对电子教室做任何操作";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
			}
			else if (mixed || single.empty()) {
				_StatusTextMain = L"已接管多个电子教室";
				_StatusTextMore = eduSummary + L"。您可以放心继续您的工作";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlled;
			}
			else {
				_StatusTextMain = L"已接管" + single;
				_StatusTextMore = L"您可以放心继续您的工作";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlled;
			}
		}
		else {
			// 极域不在，别家教室也还没接管。但「没接管」不等于「没在跑」：
			// 学生端刚被守护拉起来、还没轮到扫描，或者自动接管被关掉，进程
			// 就在那儿。标题一派「未运行」，用户会以为点了启动没反应。
			const std::wstring pending = _Running ? GetEduPendingSummary() : std::wstring();
			if (!pending.empty()) {
				const bool single = pending.find(L'、') == std::wstring::npos;
				_StatusTextMain = single ? (L"检测到" + pending + L"在运行")
					: L"检测到多个电子教室在运行";
				_StatusTextMore = _EduCompatEnabled
					? (single ? L"正在接管它，稍等片刻"
						: L"正在接管 " + pending + L"，稍等片刻")
					: L"自动接管已关闭，点开 <b>电子教室操作</b> 可以结束它";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotRunning;
			}
			else {
				_StatusTextMain = L"极域电子教室未运行";
				if (!_Running) {
					_StatusTextMain = L"极域电子教室未运行 并且控制器未启动";
					_StatusTextMore = L"您已手动停止控制器<br / >当前不会检测极域的运行";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_StudentMainFileLocated) {
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotRunning;
					// 「下方按钮」已经改名叫「电子教室操作」（那格现在既能结束也能
					// 启动），文案得跟着走，否则用户会去找一个不存在的按钮。
					_StatusTextMore = L"已在此计算机上找到极域电子教室<br/>点开下方的 <b>电子教室操作</b> 可以启动它";
				}
				else {
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotFound;
					_StatusTextMore = L"未在此计算机上找到极域电子教室";
				}
			}
		}

		_Callback->OnUpdateState(status, _StatusTextMain.c_str(), _StatusTextMore.c_str());
	}
}

bool TrainerWorkerInternal::InstallVirus()
{
	return InjectDll(_StudentMainPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InstallVirusForMaster()
{
	return InjectDll(_MasterHelperPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InjectDll(DWORD pid, LPCWSTR dllPath)
{
	HANDLE hRemoteProcess;
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hRemoteProcess);
	if (!NT_SUCCESS(ntStatus)) {
		currentLogger->LogError2(L"注入病毒失败！打开进程失败：0x%08X", ntStatus);
		return FALSE;
	}
	const dll_injector::Result result = dll_injector::InjectDll(hRemoteProcess, dllPath);
	CloseHandle(hRemoteProcess);
	currentLogger->LogInfo(L"Injection result pid=%lu path=%s success=%d error=%lu remoteModule=0x%08X",
		pid, dllPath, result.success, result.error, result.remote_exit_code);
	if (!result) {
		currentLogger->LogError2(L"注入 DLL 失败！Win32 错误：%d", result.error);
	}
	return result.success;
}
bool TrainerWorkerInternal::UnInjectDll(DWORD pid, LPCWSTR moduleName)
{
	HANDLE hProcess;
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hProcess);
	if (!NT_SUCCESS(ntStatus)) {
		currentLogger->LogError2(L"卸载病毒失败！打开进程失败：0x%08X", ntStatus);
		return FALSE;
	}
	const dll_injector::Result result =
		dll_injector::UnloadDll(hProcess, pid, moduleName);
	CloseHandle(hProcess);
	if (!result) {
		currentLogger->LogError2(L"卸载 DLL 失败！Win32 错误：%d", result.error);
	}
	return result.success;
}
// ===================== 自定义进程注入 =====================
// 目标由用户在 GUI / 命令行显式指定，不自动扩散到子进程。
// 32 位目标复用现有 InjectDll；64 位目标交给内嵌的 64 位助手完成。
// 目标进程已经退出时不用卸载：模块跟着进程一起没了，硬去 OpenProcess 只会
// 拿到 STATUS_INVALID_CID（0xC000000B），退出日志凭空多两条失败记录。
static bool IsProcessGone(DWORD pid)
{
	HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
	const DWORD wait = WaitForSingleObject(process, 0);
	CloseHandle(process);
	return wait == WAIT_OBJECT_0;
}

static void SetCustomInjectStatus(std::wstring* status, const std::wstring& text)
{
	if (status) *status = text;
}

// 32 位目标：FreeLibrary 之前先远程调用 hooks DLL 导出的 VUnloadAll()。
// DllMain(DLL_PROCESS_DETACH) 不清理钩子，直接卸载会留下悬空 detour，
// 目标进程下一次调用被钩 API 就跳到已释放内存。64 位侧由助手做同样的两步。
static void RemoteInvokeVUnloadAll(DWORD pid, LPCWSTR dllPath, LPCWSTR moduleName)
{
	if (!dllPath || !*dllPath) return;
	// 当数据文件映射，不执行 DllMain；导出表仍可读，函数地址-基址即 RVA。
	const HMODULE local = LoadLibraryExW(dllPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!local) return;
	const FARPROC proc = GetProcAddress(local, "VUnloadAll");
	const DWORD rva = proc ? static_cast<DWORD>(
		reinterpret_cast<const BYTE*>(proc) - reinterpret_cast<const BYTE*>(local)) : 0;
	FreeLibrary(local);
	if (!rva) return;

	HMODULE remoteBase = nullptr;
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if (snapshot != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Module32FirstW(snapshot, &entry)) {
			do {
				if (_wcsicmp(entry.szModule, moduleName) == 0) { remoteBase = entry.hModule; break; }
			} while (Module32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
	}
	if (!remoteBase) return;
	HANDLE process = nullptr;
	if (!NT_SUCCESS(MOpenProcessNt(pid, &process)) || !process) return;
	const auto unloadFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		reinterpret_cast<BYTE*>(remoteBase) + rva);
	const HANDLE thread = CreateRemoteThread(process, nullptr, 0, unloadFn, nullptr, 0, nullptr);
	if (thread) { WaitForSingleObject(thread, 15000); CloseHandle(thread); }
	CloseHandle(process);
}

static std::wstring TrimCustomTarget(const std::wstring& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'"')) ++b;
	while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'"')) --e;
	return s.substr(b, e - b);
}
static bool IsDecimalCustomTarget(const std::wstring& s)
{
	if (s.empty()) return false;
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] < L'0' || s[i] > L'9') return false;
	return true;
}

bool TrainerWorkerInternal::ResolveCustomTarget(LPCWSTR target, DWORD* outPid,
	std::wstring* outName, std::wstring* status)
{
	const std::wstring text = TrimCustomTarget(target ? target : L"");
	if (text.empty()) { SetCustomInjectStatus(status, L"请输入 PID 或进程名"); return false; }
	const bool numeric = IsDecimalCustomTarget(text);
	const DWORD wantedPid = numeric ? static_cast<DWORD>(_wtol(text.c_str())) : 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		DWORD matches = 0, foundPid = 0;
		std::wstring foundName;
		if (Process32FirstW(snapshot, &entry)) {
			do {
				const std::wstring exe(entry.szExeFile);
				bool hit = numeric ? (entry.th32ProcessID == wantedPid)
					: (_wcsicmp(exe.c_str(), text.c_str()) == 0);
				if (!hit && !numeric) {
					const size_t dot = exe.find_last_of(L'.');
					const std::wstring base = dot == std::wstring::npos ? exe : exe.substr(0, dot);
					hit = _wcsicmp(base.c_str(), text.c_str()) == 0;
				}
				if (hit) {
					++matches;
					if (!foundPid) { foundPid = entry.th32ProcessID; foundName = exe; }
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		if (foundPid) {
			*outPid = foundPid;
			*outName = foundName;
			if (matches > 1)
				currentLogger->LogWarn(L"自定义注入：进程名 %s 匹配到 %lu 个实例，使用 PID %lu",
					text.c_str(), matches, foundPid);
			return true;
		}
	}
	// 快照枚举不到受保护进程时，显式给出的 PID 仍允许尝试，让注入器给出真实错误码。
	if (numeric) {
		*outPid = wantedPid;
		*outName = L"PID " + text;
		return true;
	}
	SetCustomInjectStatus(status, L"找不到进程：" + text);
	return false;
}

// 只挡"注入自己"这一种：训练器把 hooks 装进自己的进程，会把自己的文件/注册表
// 调用一起钩住，界面和日志当场就废。系统关键进程不再在这里拦 —— 目标可能来自
// 人工添加，是否值得冒险由用户决定；真打不开或注入失败时注入器会给出真实错误码。
bool TrainerWorkerInternal::IsSelfCustomTarget(DWORD pid, LPCWSTR name, std::wstring* status)
{
	if (pid == GetCurrentProcessId()) {
		SetCustomInjectStatus(status, L"不能注入训练器自身");
		return false;
	}
	return true;
}

bool TrainerWorkerInternal::DetectTargetBitness(DWORD pid, bool* outIs64, std::wstring* status)
{
	*outIs64 = false;
	if (!SysHlp::Is64BitOS()) return true;   // 32 位系统上所有进程都是 32 位

	HANDLE process = nullptr;
	const NTSTATUS ntStatus = MOpenProcessNt(pid, &process);
	if (!NT_SUCCESS(ntStatus) || !process) {
		WCHAR message[128] = {};
		swprintf_s(message, L"打开目标进程失败 (NTSTATUS 0x%08X)", static_cast<unsigned>(ntStatus));
		SetCustomInjectStatus(status, message);
		return false;
	}
	typedef BOOL(WINAPI *IsWow64Process2Fn)(HANDLE, USHORT*, USHORT*);
	const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	const auto isWow64Process2 = kernel32
		? reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(kernel32, "IsWow64Process2")) : nullptr;
	if (isWow64Process2) {
		USHORT machine = 0, native = 0;
		if (isWow64Process2(process, &machine, &native)) {
			*outIs64 = machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64
				|| (machine == IMAGE_FILE_MACHINE_UNKNOWN && native != IMAGE_FILE_MACHINE_I386);
			CloseHandle(process);
			return true;
		}
	}
	BOOL wow64 = FALSE;
	if (IsWow64Process(process, &wow64)) {
		*outIs64 = !wow64;   // 本机已是 64 位系统；非 WOW64 即原生 64 位
		CloseHandle(process);
		return true;
	}
	CloseHandle(process);
	SetCustomInjectStatus(status, L"无法判断目标进程位数，已取消注入");
	return false;
}

bool TrainerWorkerInternal::RunInjector64(DWORD pid, LPCWSTR dllPath, bool unload, std::wstring* status)
{
	const LPCWSTR injector = currentApp ? currentApp->GetPartFullPath(PART_INJECTOR64) : nullptr;
	if (!injector || GetFileAttributesW(injector) == INVALID_FILE_ATTRIBUTES) {
		SetCustomInjectStatus(status, L"64 位注入助手不存在，请使用包含新资源的主程序");
		return false;
	}
	std::wstring command = L"\"" + std::wstring(injector) + L"\" --pid " + std::to_wstring(pid)
		+ L" --dll \"" + std::wstring(dllPath) + L"\"" + (unload ? L" --unload" : L"");
	std::vector<wchar_t> writable(command.begin(), command.end());
	writable.push_back(L'\0');

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION processInfo = {};
	if (!CreateProcessW(injector, writable.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo)) {
		WCHAR message[128] = {};
		swprintf_s(message, L"启动 64 位注入助手失败，错误码 %lu", GetLastError());
		SetCustomInjectStatus(status, message);
		return false;
	}
	CloseHandle(processInfo.hThread);
	WaitForSingleObject(processInfo.hProcess, _Injector64TimeoutMs);
	DWORD exitCode = STILL_ACTIVE;
	GetExitCodeProcess(processInfo.hProcess, &exitCode);
	CloseHandle(processInfo.hProcess);
	if (exitCode != 0) {
		WCHAR message[160] = {};
		swprintf_s(message, L"64 位%s失败，错误码 %lu", unload ? L"卸载" : L"注入", exitCode);
		SetCustomInjectStatus(status, message);
		currentLogger->LogError(L"Injector64 failed pid=%lu unload=%d exit=%lu", pid, unload, exitCode);
		return false;
	}
	return true;
}

// 定义在文件后半段（卸载之后），注入时要先用它查路径判断产品归属。
static bool QueryProcessImagePath(DWORD pid, std::wstring* outPath);

bool TrainerWorkerInternal::InjectCustomProcess(LPCWSTR target, std::wstring* status)
{
	DWORD pid = 0;
	std::wstring name;
	if (!ResolveCustomTarget(target, &pid, &name, status)) return false;
	if (!IsSelfCustomTarget(pid, name.c_str(), status)) return false;
	for (const auto& injected : _CustomInjected) {
		if (injected.pid == pid) {
			SetCustomInjectStatus(status, L"该进程已注入，无需重复操作：" + name);
			return true;
		}
	}
	bool is64 = false;
	if (!DetectTargetBitness(pid, &is64, status)) return false;
	const LPCWSTR dll = currentApp
		? currentApp->GetPartFullPath(is64 ? PART_HOOKER64 : PART_HOOKER) : nullptr;
	if (!dll || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
		SetCustomInjectStatus(status, is64 ? L"x64 hooks DLL 不存在，请使用包含新资源的主程序"
			: L"hooks DLL 不存在");
		return false;
	}
	const bool ok = is64 ? RunInjector64(pid, dll, false, status) : InjectDll(pid, dll);
	if (!ok) {
		if (status && status->empty()) SetCustomInjectStatus(status, L"注入失败，详见运行日志");
		currentLogger->LogError(L"自定义注入失败 target=%s pid=%lu is64=%d", target ? target : L"", pid, is64);
		return false;
	}
	CustomInjectedProcess record;
	record.pid = pid;
	record.name = name;
	record.is64 = is64;
	// 手动注入的目标也过一遍产品表：用户自己点了「红蜘蛛」，界面上照样要能看出
	// 它属于哪套电子教室。路径查不到就当普通进程处理，不影响注入结果。
	std::wstring imagePath;
	if (QueryProcessImagePath(pid, &imagePath)) {
		record.path = imagePath;
		const wchar_t* product = EduCompatMatch(name.c_str(), imagePath.c_str());
		if (product) {
			record.product = product;
			// 结束之后还得靠它把学生端拉回来，记一份不跟着注入记录走的。
			_EduLaunchPaths[product] = imagePath;
		}
	}
	_CustomInjected.push_back(record);
	if (!record.product.empty()) NotifyEduHosts();
	WCHAR message[192] = {};
	swprintf_s(message, L"已注入 %s [PID %lu，%s]", name.c_str(), pid, is64 ? L"64 位" : L"32 位");
	SetCustomInjectStatus(status, message);
	currentLogger->LogInfo(L"自定义注入成功 pid=%lu name=%s is64=%d dll=%s", pid, name.c_str(), is64, dll);
	return true;
}

bool TrainerWorkerInternal::UninjectCustomProcess(LPCWSTR target, std::wstring* status)
{
	DWORD pid = 0;
	std::wstring name;
	if (!ResolveCustomTarget(target, &pid, &name, status)) return false;
	auto it = _CustomInjected.begin();
	for (; it != _CustomInjected.end(); ++it) if (it->pid == pid) break;
	bool is64 = false;
	std::wstring recordName = name;
	std::wstring recordProduct;
	if (it != _CustomInjected.end()) {
		is64 = it->is64;
		recordName = it->name;
		recordProduct = it->product;
	}
	else {
		// 命令行 -uninject 由另一个进程发起，本进程的已注入列表是空的；
		// 直接按目标位数卸载，目标若已退出由注入器/助手返回明确错误。
		if (!DetectTargetBitness(pid, &is64, status)) return false;
	}
	bool ok = false;
	if (is64) {
		const LPCWSTR dll = currentApp ? currentApp->GetPartFullPath(PART_HOOKER64) : nullptr;
		ok = dll && RunInjector64(pid, dll, true, status);
	}
	else {
		RemoteInvokeVUnloadAll(pid,
			currentApp ? currentApp->GetPartFullPath(PART_HOOKER) : L"",
			L"DzjsTrainerHooks.dll");
		ok = UnInjectDll(pid, L"DzjsTrainerHooks.dll");
		if (!ok) SetCustomInjectStatus(status, L"卸载失败，详见运行日志");
	}
	if (!ok) return false;
	if (it != _CustomInjected.end()) _CustomInjected.erase(it);
	if (!recordProduct.empty()) NotifyEduHosts();
	// 手动卸载是有意为之：记一笔，自动适配别在 10 秒后又把它注回去。
	// 进程退出后这条记录会被清掉，下次启动照常接管。
	_EduManualSkip.insert(pid);
	WCHAR message[192] = {};
	swprintf_s(message, L"已卸载 %s [PID %lu]", recordName.c_str(), pid);
	SetCustomInjectStatus(status, message);
	currentLogger->LogInfo(L"自定义注入已卸载 pid=%lu name=%s is64=%d", pid, recordName.c_str(), is64);
	return true;
}

// 取进程完整路径。先走 Nt 打开（受保护的进程也能拿到），失败再退普通权限。
static bool QueryProcessImagePath(DWORD pid, std::wstring* outPath)
{
	if (!outPath) return false;
	HANDLE process = nullptr;
	if (!NT_SUCCESS(MOpenProcessNt(pid, &process)) || !process)
		process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return false;

	WCHAR buffer[MAX_PATH * 2] = {};
	DWORD size = _countof(buffer);
	const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
	CloseHandle(process);
	if (!ok || size == 0) return false;
	*outPath = buffer;
	return true;
}

// 扫描其他电子教室的学生端进程，命中产品表就按通用宿主注入。
//
// 节奏：扫描本身 10 秒一次（RunResetPid 是 2 秒一次的定时器，直接跟着它跑太密）。
// 一轮里把命中的目标全部注进去 —— 一台机器同时装两套电子教室是常态，每轮只注
// 一个会让第二套晚 10 秒才受控，这中间它对教师端是完全透明的。
// 单轮上限 6 个：64 位目标要起注入助手并等它返回，一轮全注会占住界面线程，
// 6 个是「够用且基本感觉不到卡顿」的折中。
void TrainerWorkerInternal::RunEduCompatInject()
{
	const ULONGLONG now = GetTickCount64();
	const ULONGLONG interval = now < _EduStartWatchUntil ? 2000 : 10000;
	if (now - _LastEduCompatScan < interval) return;
	_LastEduCompatScan = now;

	// 已注入记录里的死 PID 必须清掉：PID 会被复用，留着会让新起来的进程被
	// 当成「已注入」而永远拿不到钩子。关掉自动注入时也要清 —— 界面上的列表
	// 和进程选择器的「已注入」标记都读这份记录。
	bool droppedHost = false;
	for (auto it = _CustomInjected.begin(); it != _CustomInjected.end(); )
	{
		if (IsProcessGone(it->pid)) {
			if (!it->product.empty()) droppedHost = true;
			it = _CustomInjected.erase(it);
		}
		else ++it;
	}
	if (droppedHost) NotifyEduHosts();

	// 「有教室在跑、但还没接管」也要让标题知道：自动接管关掉时不会走到下面
	// 的注入流程，没有别的事件会来刷新状态，界面就一直是「未运行」。
	const std::wstring pending = GetEduPendingSummary();
	if (pending != _EduPendingSummary) {
		_EduPendingSummary = pending;
		NotifyEduHosts();
	}

	if (!_EduCompatEnabled) return;

	// 这一轮跑在界面线程上：注入助手等不到就得放弃，下一轮再来。40 秒那个上限
	// 是给手动注入用的（用户知道自己在等），放在这里会把窗口钉住几十秒。
	const DWORD savedInjectorTimeout = _Injector64TimeoutMs;
	_Injector64TimeoutMs = 8000;

	// 失败记录压 10 分钟。PID 会被系统复用，进程没了就立刻放开。
	for (auto it = _EduInjectBlocked.begin(); it != _EduInjectBlocked.end(); )
	{
		if (now - it->second > 10 * 60 * 1000 || IsProcessGone(it->first)) it = _EduInjectBlocked.erase(it);
		else ++it;
	}
	// 手动卸载过的目标：同一个进程不再自动注入回去，进程退出后自动解禁。
	for (auto it = _EduManualSkip.begin(); it != _EduManualSkip.end(); )
	{
		if (IsProcessGone(*it)) it = _EduManualSkip.erase(it);
		else ++it;
	}

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		_Injector64TimeoutMs = savedInjectorTimeout;
		return;
	}

	const int kMaxPerRound = 6;
	int injected = 0;
	int failed = 0;
	int slowUsed = 0;
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (injected + failed >= kMaxPerRound) break;

			const DWORD pid = entry.th32ProcessID;
			if (pid <= 4) continue;
			// 名字先过一遍：不是候选就不查路径，一百多个进程绝大部分在这里筛掉。
			if (!EduCompatNameCandidate(entry.szExeFile)) continue;
			if (_EduInjectBlocked.find(pid) != _EduInjectBlocked.end()) continue;
			if (_EduManualSkip.find(pid) != _EduManualSkip.end()) continue;

			// 产品表要求路径特征，只看进程名会误伤同名程序。
			std::wstring path;
			if (!QueryProcessImagePath(pid, &path)) continue;
			const wchar_t* product = EduCompatMatch(entry.szExeFile, path.c_str());
			if (!product) continue;

			// 这款刚被「结束」过，正在盯梢：又冒出来的进程是要收掉的，不是要
			// 接管的。注进去等于自己跟自己的清理动作对着干。
			if (_EduKillWatchActive &&
				(_EduKillWatchProduct.empty() || _wcsicmp(_EduKillWatchProduct.c_str(), product) == 0))
				continue;

			// 64 位目标要走注入助手，一轮只放一个：这是整个扫描里唯一可能等上
			// 几秒的步骤，几个叠在一起就会明显卡界面。32 位注入是几十毫秒的事，
			// 同一轮注几个都无所谓。
			bool is64 = false;
			std::wstring bitness;
			if (!DetectTargetBitness(pid, &is64, &bitness)) {
				// 连进程都打不开，注入必然也失败：记一次、压 10 分钟，
				// 免得一个被保护的目标把日志刷满。
				++failed;
				_EduInjectBlocked[pid] = now;
				currentLogger->LogError(L"[%s] 无法判断 %s [PID %lu] 的位数：%s",
					product, entry.szExeFile, pid, bitness.c_str());
				continue;
			}
			if (is64 && slowUsed >= 1) continue;

			bool alreadyInjected = false;
			for (auto it = _CustomInjected.begin(); it != _CustomInjected.end(); ) {
				// 名字对不上说明这个 PID 已经被别的进程复用了，旧记录作废。
				if (it->pid == pid && _wcsicmp(it->name.c_str(), entry.szExeFile) != 0)
					it = _CustomInjected.erase(it);
				else {
					if (it->pid == pid) alreadyInjected = true;
					++it;
				}
			}
			if (alreadyInjected) continue;

			std::wstring status;
			const std::wstring target = std::to_wstring(pid);
			if (InjectCustomProcess(target.c_str(), &status)) {
				++injected;
				if (is64) ++slowUsed;
				currentLogger->Log(L"[%s] 已自动注入 %s [PID %lu]", product, entry.szExeFile, pid);
			}
			else {
				++failed;
				_EduInjectBlocked[pid] = now;
				currentLogger->LogError(L"[%s] 自动注入 %s [PID %lu] 失败：%s",
					product, entry.szExeFile, pid, status.c_str());
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	_Injector64TimeoutMs = savedInjectorTimeout;

	// 只在这一轮真的动过手时才补汇总：两套以上产品同时受控时，光看单条
	// 「已自动注入」拼不出全局状态；没事发生的轮次保持安静，别刷日志。
	const int handled = injected + failed;
	if (handled > 0 && (handled > 1 || _CustomInjected.size() > 1))
		currentLogger->Log(L"[电子教室] 本轮注入 %d 个（失败 %d 个），当前共管控 %d 个宿主",
			injected, failed, (int)_CustomInjected.size());
}

std::wstring TrainerWorkerInternal::GetCustomInjectedList()
{
	if (_CustomInjected.empty()) return L"（暂无自定义注入进程）";
	std::wstring text;
	for (const auto& item : _CustomInjected) {
		WCHAR line[224] = {};
		// 自动接管的宿主带上产品名，用户才知道这行不是自己点的；
		// 手动注入的目标保持原样，免得「· 手动」这种标签刷满一列。
		if (item.product.empty()) {
			swprintf_s(line, L"PID %-8lu  %-32s  %s", item.pid, item.name.c_str(),
				item.is64 ? L"64 位" : L"32 位");
		}
		else {
			swprintf_s(line, L"PID %-8lu  %-32s  %s  · %s", item.pid, item.name.c_str(),
				item.is64 ? L"64 位" : L"32 位", item.product.c_str());
		}
		text += line;
		text += L"\n";
	}
	return text;
}

int TrainerWorkerInternal::GetEduHostCount()
{
	int count = 0;
	for (const auto& item : _CustomInjected)
		if (!item.product.empty()) ++count;
	return count;
}

// 「红蜘蛛 ×2、联想传奇 ×1」。按产品聚合，同类多开（学生端有时会起来两个
// 进程）不会把同一句话重复几遍。
std::wstring TrainerWorkerInternal::GetEduHostSummary()
{
	struct Group { std::wstring name; int count = 0; std::list<std::wstring> processes; };
	std::list<Group> groups;
	for (const auto& item : _CustomInjected) {
		if (item.product.empty()) continue;
		auto it = groups.begin();
		for (; it != groups.end(); ++it)
			if (it->name == item.product) break;
		if (it == groups.end()) {
			Group group;
			group.name = item.product;
			group.count = 1;
			group.processes.push_back(item.name);
			groups.push_back(group);
		}
		else {
			++it->count;
			bool seen = false;
			for (const auto& existing : it->processes)
				if (_wcsicmp(existing.c_str(), item.name.c_str()) == 0) { seen = true; break; }
			if (!seen) it->processes.push_back(item.name);
		}
	}
	std::wstring text;
	for (const auto& group : groups) {
		if (!text.empty()) text += L"、";
		// 一个进程时不带数字：「红蜘蛛 ×1」里的 1 没有任何信息量。
		if (group.count <= 1) {
			text += group.name;
			continue;
		}
		WCHAR part[96] = {};
		swprintf_s(part, L"%s ×%d", group.name.c_str(), group.count);
		text += part;
		// 同一款教室挂多个进程是常态（联想传奇是 PsGhost 守护 + Student 学生端），
		// 只写 ×2 看不出挂的是哪两个，把进程名带上。
		if (group.processes.size() > 1) {
			std::wstring names;
			for (const auto& process : group.processes) {
				std::wstring plain = process;
				if (plain.size() > 4 && _wcsicmp(plain.c_str() + plain.size() - 4, L".exe") == 0)
					plain.resize(plain.size() - 4);
				if (!names.empty()) names += L"、";
				names += plain;
			}
			text += L"（" + names + L"）";
		}
	}
	return text;
}

// ControlService 只是把 STOP 请求投出去，紧接着去杀进程会和服务的恢复动作抢先
// 一步，看起来就是「杀掉了又回来了」。等到 STOPPED 再往下走。
static bool WaitServiceStopped(SC_HANDLE service, DWORD timeoutMs)
{
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		SERVICE_STATUS status{};
		if (!QueryServiceStatus(service, &status)) return false;
		if (status.dwCurrentState == SERVICE_STOPPED) return true;
		if (GetTickCount64() >= deadline) return false;
		Sleep(100);
	}
}

// 整目录清扫的上限。真往一个目录塞几十个 exe 的教室不多见，超了多半是目录
// 判断出了问题（比如学生端装在 Program Files 根下），宁可停手。
constexpr int kSweepProcessLimit = 32;

// 映像在 %SystemRoot% 下的算系统进程：父链走到这里就停，不能再往上杀。
static bool IsSystemProcessImage(const std::wstring& imagePath)
{
	if (imagePath.empty()) return true;  // 取不到路径的多半是受保护进程，不碰
	WCHAR windows[MAX_PATH] = {};
	const DWORD length = GetWindowsDirectoryW(windows, _countof(windows));
	if (length == 0 || length >= _countof(windows)) return false;
	std::wstring systemRoot = windows;
	while (!systemRoot.empty() && (systemRoot.back() == L'\\' || systemRoot.back() == L'/'))
		systemRoot.pop_back();
	if (imagePath.size() <= systemRoot.size()) return false;
	if (_wcsnicmp(imagePath.c_str(), systemRoot.c_str(), systemRoot.size()) != 0) return false;
	const wchar_t next = imagePath[systemRoot.size()];
	return next == L'\\' || next == L'/';
}

static bool IsUnderDirectory(const std::wstring& path, const std::wstring& directory)
{
	if (directory.empty() || path.size() <= directory.size()) return false;
	if (_wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0) return false;
	const wchar_t next = path[directory.size()];
	return next == L'\\' || next == L'/';
}

static void AppendUniqueName(std::vector<std::wstring>* list, const wchar_t* name)
{
	if (list == nullptr || name == nullptr || name[0] == L'\0') return;
	for (const auto& item : *list)
		if (_wcsicmp(item.c_str(), name) == 0) return;
	list->push_back(name);
}

// 整目录清理只在这种目录上做：不能是盘根，也不能是 Program Files / Windows
// 这类公共目录本身 —— 学生端真装在 Program Files 根上的话，一清就是半个系统。
static bool IsSafeSweepDirectory(const std::wstring& raw)
{
	std::wstring directory = raw;
	while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
		directory.pop_back();
	if (directory.size() <= 3) return false;

	static const wchar_t* const kForbiddenVars[] = {
		L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432",
		L"ProgramData", L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA", L"TEMP",
	};
	for (const wchar_t* variable : kForbiddenVars) {
		WCHAR value[MAX_PATH] = {};
		const DWORD length = GetEnvironmentVariableW(variable, value, _countof(value));
		if (length == 0 || length >= _countof(value)) continue;
		std::wstring candidate = value;
		while (!candidate.empty() && (candidate.back() == L'\\' || candidate.back() == L'/'))
			candidate.pop_back();
		if (!candidate.empty() && _wcsicmp(directory.c_str(), candidate.c_str()) == 0) return false;
	}
	return true;
}

// 进程是某个服务的宿主时，直接杀会让 SCM 按恢复策略把它再拉起来。整目录
// 清扫会碰上好几个这样的进程，所以先把「pid → 服务名」整表拉一次，别每杀
// 一个都枚举一遍服务（这条路径是在界面线程上跑的）。
static void BuildServicePidMap(std::map<DWORD, std::wstring>* out)
{
	if (out == nullptr) return;
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
		SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (manager == nullptr) return;

	std::vector<BYTE> buffer(16 * 1024);
	DWORD needed = 0, count = 0, resume = 0;
	for (;;) {
		const BOOL enumerated = EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
			SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()),
			&needed, &count, &resume, nullptr);
		if (!enumerated) {
			const DWORD error = GetLastError();
			if (error == ERROR_MORE_DATA) {
				buffer.resize(needed > buffer.size() ? needed : buffer.size() * 2);
				resume = 0;
				continue;
			}
			break;
		}
		auto* entries = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buffer.data());
		for (DWORD i = 0; i < count; ++i) {
			const DWORD pid = entries[i].ServiceStatusProcess.dwProcessId;
			if (pid == 0 || entries[i].lpServiceName == nullptr) continue;
			out->emplace(pid, entries[i].lpServiceName);
		}
		if (resume == 0) break;
	}
	CloseServiceHandle(manager);
}

// 停一个服务并等它停下；本来就没在跑也算成功。
static bool StopServiceEntry(SC_HANDLE manager, const wchar_t* name)
{
	if (manager == nullptr || name == nullptr) return false;
	SC_HANDLE service = OpenServiceW(manager, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (service == nullptr) return false;
	SERVICE_STATUS status{};
	bool stopped = false;
	if (ControlService(service, SERVICE_CONTROL_STOP, &status))
		stopped = WaitServiceStopped(service, 3000);
	else if (GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
		stopped = true;
	CloseServiceHandle(service);
	return stopped;
}

// 按「服务名」或「服务二进制路径里有这个 exe 名」把服务挑出来。
// 服务名给 nullptr 时只走路径匹配：红蜘蛛的 checkrs 在各版本里服务名不统一，
// 但可执行文件名一直是 checkrs.exe。
//
// 停止和启动都要这一套查法，抄两遍迟早会改漏一边。
static int FindServicesByImage(SC_HANDLE manager, const wchar_t* exeName,
	const wchar_t* serviceName, std::vector<std::wstring>* out)
{
	const bool byName = (serviceName != nullptr && serviceName[0] != L'\0');
	const bool byPath = (exeName != nullptr && exeName[0] != L'\0');
	if (out == nullptr || (!byName && !byPath)) return 0;

	int found = 0;
	std::vector<BYTE> buffer(16 * 1024);
	DWORD needed = 0, count = 0, resume = 0;
	for (;;) {
		const BOOL enumerated = EnumServicesStatusW(manager, SERVICE_WIN32, SERVICE_STATE_ALL,
			reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data()),
			static_cast<DWORD>(buffer.size()), &needed, &count, &resume);
		if (!enumerated) {
			const DWORD error = GetLastError();
			if (error == ERROR_MORE_DATA) {
				buffer.resize(needed > buffer.size() ? needed : buffer.size() * 2);
				resume = 0;  // 从头再枚举一遍，免得跳过已经返回过的条目
				continue;
			}
			currentLogger->LogWarn(L"枚举服务失败 error=%lu", error);
			break;
		}

		auto* entries = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data());
		for (DWORD i = 0; i < count; ++i) {
			const wchar_t* name = entries[i].lpServiceName;
			if (name == nullptr) continue;

			bool match = byName && _wcsicmp(name, serviceName) == 0;
			if (!match && byPath) {
				SC_HANDLE query = OpenServiceW(manager, name, SERVICE_QUERY_CONFIG);
				if (query != nullptr) {
					DWORD configSize = 0;
					QueryServiceConfigW(query, nullptr, 0, &configSize);
					if (configSize > 0) {
						std::vector<BYTE> config(configSize);
						if (QueryServiceConfigW(query,
							reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(config.data()), configSize, &configSize)) {
							const auto* serviceConfig =
								reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(config.data());
							match = serviceConfig->lpBinaryPathName != nullptr &&
								EduPathContainsNoCase(serviceConfig->lpBinaryPathName, exeName);
						}
					}
					CloseServiceHandle(query);
				}
			}
			if (!match) continue;
			out->push_back(name);
			++found;
		}
		if (resume == 0) break;
	}
	return found;
}

int TrainerWorkerInternal::StopServicesByImage(const wchar_t* exeName, const wchar_t* serviceName)
{
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
		SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (manager == nullptr) {
		currentLogger->LogWarn(L"停止守护服务：打开服务管理器失败 error=%lu", GetLastError());
		return 0;
	}

	std::vector<std::wstring> names;
	FindServicesByImage(manager, exeName, serviceName, &names);

	int stopped = 0;
	for (const auto& name : names) {
		SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (service == nullptr) {
			currentLogger->LogWarn(L"停止守护服务 %s：打开失败 error=%lu", name.c_str(), GetLastError());
			continue;
		}
		SERVICE_STATUS status{};
		if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
			if (WaitServiceStopped(service, 5000)) {
				currentLogger->LogInfo(L"已停止守护服务 %s", name.c_str());
				++stopped;
			}
			else {
				currentLogger->LogWarn(L"守护服务 %s 5 秒内没有停下来", name.c_str());
			}
		}
		else {
			const DWORD error = GetLastError();
			if (error == ERROR_SERVICE_NOT_ACTIVE) {
				// 本来就没在跑，不是失败。
			}
			else if (error == ERROR_SERVICE_CANNOT_ACCEPT_CTRL &&
				status.dwCurrentState == SERVICE_STOP_PENDING) {
				if (WaitServiceStopped(service, 5000)) ++stopped;
			}
			else {
				currentLogger->LogWarn(L"停止守护服务 %s 失败 error=%lu", name.c_str(), error);
			}
		}
		CloseServiceHandle(service);
	}
	CloseServiceHandle(manager);
	return stopped;
}

// 停的逆操作。服务起来了它自己会把学生端拉起来，而且用的命令行参数、工作目录
// 都是它自己那套，比我们拿着一个 exe 路径瞎拉要靠谱。
int TrainerWorkerInternal::StartServicesByImage(const wchar_t* exeName, const wchar_t* serviceName)
{
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
		SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
	if (manager == nullptr) {
		currentLogger->LogWarn(L"启动守护服务：打开服务管理器失败 error=%lu", GetLastError());
		return 0;
	}

	std::vector<std::wstring> names;
	FindServicesByImage(manager, exeName, serviceName, &names);

	int started = 0;
	for (const auto& name : names) {
		SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
		if (service == nullptr) {
			currentLogger->LogWarn(L"启动守护服务 %s：打开失败 error=%lu", name.c_str(), GetLastError());
			continue;
		}
		if (StartServiceW(service, 0, nullptr)) {
			currentLogger->LogInfo(L"已启动守护服务 %s", name.c_str());
			++started;
		}
		else {
			const DWORD error = GetLastError();
			if (error == ERROR_SERVICE_ALREADY_RUNNING) {
				++started;
			}
			else {
				// 服务被禁用或者被安全软件拦住时只有这条线索，写清楚错误码。
				currentLogger->LogWarn(L"启动守护服务 %s 失败 error=%lu", name.c_str(), error);
			}
		}
		CloseServiceHandle(service);
	}
	CloseServiceHandle(manager);
	return started;
}

// 按进程名收集 PID。只比名字，调用方自己决定路径要不要对。
static void CollectProcessIdsByName(const wchar_t* exeName, std::vector<DWORD>* out)
{
	if (out == nullptr || exeName == nullptr || exeName[0] == L'\0') return;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return;

	const DWORD self = GetCurrentProcessId();
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		if (entry.th32ProcessID <= 4 || entry.th32ProcessID == self) continue;
		if (_wcsicmp(entry.szExeFile, exeName) != 0) continue;
		out->push_back(entry.th32ProcessID);
	}
	CloseHandle(snapshot);
}

// TerminateProcess 返回成功到进程真的消失之间有一小段空档（对象还没完全回收），
// 紧接着查 IsProcessGone 会得到「还活着」，日志就会把成功写成失败。
static bool WaitProcessGone(DWORD pid, DWORD timeoutMs)
{
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	for (;;) {
		if (IsProcessGone(pid)) return true;
		if (GetTickCount64() >= deadline) return false;
		Sleep(50);
	}
}

// 按产品表找当前在跑的学生端进程。
//
// 判据和扫描注入用的是同一个 EduCompatMatch：名字不够独特的（REDAgent.exe、
// Student.exe）要么对上路径特征，要么对上同目录的伴生文件，不然会把不相干的
// 程序算成「电子教室在跑」，菜单里就多出一条结束项。
static void CollectProductHostPids(const EduProduct& product, std::vector<DWORD>* out)
{
	if (out == nullptr) return;
	for (int r = 0; r < product.ruleCount; ++r) {
		std::vector<DWORD> pids;
		CollectProcessIdsByName(product.rules[r].exe, &pids);
		for (DWORD pid : pids) {
			std::wstring path;
			if (!QueryProcessImagePath(pid, &path)) continue;
			const wchar_t* matched = EduCompatMatch(product.rules[r].exe, path.c_str());
			if (matched == nullptr || _wcsicmp(matched, product.name) != 0) continue;
			bool seen = false;
			for (DWORD existing : *out) if (existing == pid) { seen = true; break; }
			if (!seen) out->push_back(pid);
		}
	}
}

static const EduProduct* FindEduProduct(LPCWSTR name)
{
	if (name == nullptr || name[0] == L'\0') return nullptr;
	for (int i = 0; i < kEduProductCount; ++i)
		if (_wcsicmp(kEduProducts[i].name, name) == 0) return &kEduProducts[i];
	return nullptr;
}

// 这款教室的学生端现在有没有进程在跑（按产品表查，不看注入记录：
// 刚起来的进程要等下一轮扫描才会被接管）。
static bool AnyProductHostRunning(const EduProduct& product)
{
	std::vector<DWORD> pids;
	CollectProductHostPids(product, &pids);
	return !pids.empty();
}

// 结束一款教室之前先处理它的守护。
//
// 服务停了它的进程也就跟着没了。停不掉的那种（绿色安装、服务被删掉，或者服务
// 停在「标记删除」状态，ControlService 返回 1052）就剩下进程本身。怎么收它看
// 产品表里的 isHost：
//
//   isHost=true  它本身就是学生端（PsGhost），要的是「结束」—— 杀掉。
//   isHost=false 纯守护（checkrs、Smonitor），挂起比杀更有效。服务进程被
//                TerminateProcess 掉，SCM 会当成一次异常退出，按恢复策略把它
//                重新拉起来 —— 红蜘蛛就是这样，杀了 checkrs 几秒后它又起来，
//                再把 REDAgent 拉起来，用户看到的是「杀了还会重启」。挂起不
//                触发恢复动作。
int TrainerWorkerInternal::StopEduGuards(const EduProduct& product)
{
	int stopped = 0;
	std::map<DWORD, std::wstring> serviceByPid;
	bool serviceMapReady = false;

	// 先把已经不存在的守护 PID 从挂起表里剔掉：PID 会被复用，留着会让新进程
	// 被当成「已经挂起过」而跳过。
	for (auto it = _EduSuspendedGuards.begin(); it != _EduSuspendedGuards.end(); ) {
		if (IsProcessGone(*it)) it = _EduSuspendedGuards.erase(it);
		else ++it;
	}

	for (int i = 0; i < product.guardCount; ++i) {
		const EduGuardRule& guard = product.guards[i];
		stopped += StopServicesByImage(guard.exe, guard.service);

		// 服务停下来了它的进程也就跟着没了；还留着的还得按 isHost 收一遍。
		const bool alsoHost = guard.isHost;
		std::vector<DWORD> pids;
		CollectProcessIdsByName(guard.exe, &pids);
		if (pids.empty()) continue;
		if (!serviceMapReady) {
			BuildServicePidMap(&serviceByPid);
			serviceMapReady = true;
		}

		for (DWORD pid : pids) {
			if (!alsoHost) {
				// 挂起是计数式的：同一个进程挂第二次，之后一次 NtResumeProcess
				// 救不回来。「结束」一次会走到这里两趟（停服务 + 清扫上游），
				// 所以记一份挂起过的 PID，一个进程只挂一次。
				bool alreadySuspended = false;
				for (DWORD existing : _EduSuspendedGuards)
					if (existing == pid) { alreadySuspended = true; break; }
				if (alreadySuspended) continue;
				if (MSuspendProcessNt(pid, nullptr) == STATUS_SUCCESS) {
					_EduSuspendedGuards.push_back(pid);
					currentLogger->LogInfo(L"已挂起守护进程 %s [PID %lu]", guard.exe, pid);
					++stopped;
					continue;
				}
			}
			else {
				auto service = serviceByPid.find(pid);
				if (service != serviceByPid.end()) {
					SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
					if (manager != nullptr) {
						StopServiceEntry(manager, service->second.c_str());
						CloseServiceHandle(manager);
					}
					if (IsProcessGone(pid)) {
						++stopped;
						continue;
					}
				}
			}
			if (KillProcess(pid, true)) {
				currentLogger->LogInfo(L"已结束守护进程 %s [PID %lu]", guard.exe, pid);
				++stopped;
			}
		}
	}
	return stopped;
}

// 清一个宿主连带的东西：同文件夹（含子文件夹）里的进程、以及往上能追到的
// 非系统父进程。
//
// 只杀注入记录里那一个进程是不够的：红蜘蛛的学生端旁边通常还有 upgrader、
// 托盘、检查器等好几个 exe，上游还有个 checkrs.exe 服务盯着，杀一个它补一个。
// 父链用 %SystemRoot% 划界——explorer.exe、services.exe 这些都在系统目录下，
// 走到那里就停，不会顺着摸到用户桌面上。
int TrainerWorkerInternal::SweepEduCompanions(DWORD rootPid, const std::wstring& imagePath)
{
	if (rootPid <= 4) return 0;

	std::map<DWORD, DWORD> parents;
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
			parents[entry.th32ProcessID] = entry.th32ParentProcessID;
		CloseHandle(snapshot);
	}

	// 父链：[0] 是宿主自己，往上依次追加，遇到系统进程或本进程就停。
	std::vector<DWORD> ancestors;
	DWORD cursor = rootPid;
	for (int depth = 0; depth < 8; ++depth) {
		ancestors.push_back(cursor);
		auto it = parents.find(cursor);
		if (it == parents.end()) break;
		const DWORD parent = it->second;
		if (parent <= 4 || parent == GetCurrentProcessId()) break;
		std::wstring parentPath;
		if (!QueryProcessImagePath(parent, &parentPath)) break;
		if (IsSystemProcessImage(parentPath)) break;
		cursor = parent;
	}

	// 同目录 + 子目录
	std::wstring directory;
	const size_t slash = imagePath.find_last_of(L"\\/");
	if (slash != std::wstring::npos && slash > 2) directory = imagePath.substr(0, slash);

	std::vector<DWORD> companions;
	if (IsSafeSweepDirectory(directory)) {
		for (const auto& item : parents) {
			if (item.first <= 4 || item.first == GetCurrentProcessId()) continue;
			std::wstring path;
			if (!QueryProcessImagePath(item.first, &path)) continue;
			if (!IsUnderDirectory(path, directory)) continue;
			companions.push_back(item.first);
			if (static_cast<int>(companions.size()) >= kSweepProcessLimit) break;
		}
	}
	else if (!directory.empty()) {
		currentLogger->LogWarn(L"目录不适合整目录清理，只结束目标本身：%s", directory.c_str());
	}

	// pid → 服务名，整表拉一次：目录里可能有好几个服务宿主，挨个枚举太慢。
	std::map<DWORD, std::wstring> serviceByPid;
	BuildServicePidMap(&serviceByPid);
	SC_HANDLE scm = serviceByPid.empty() ? nullptr
		: OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);

	std::set<DWORD> handled;
	int killed = 0;
	auto killOne = [&](DWORD pid) {
		if (pid <= 4 || pid == GetCurrentProcessId()) return;
		if (!handled.insert(pid).second) return;
		if (IsProcessGone(pid)) return;
		// 服务宿主先走 SCM 正常停，直接杀会被恢复策略拉起来。
		auto service = serviceByPid.find(pid);
		if (service != serviceByPid.end() && StopServiceEntry(scm, service->second.c_str()))
			currentLogger->LogInfo(L"已停止服务 %s（PID %lu）", service->second.c_str(), pid);
		if (KillProcess(pid, true)) ++killed;
	};

	// 先断上游：反过来的话，父进程会在我们杀子进程的间隙把它重新拉起来。
	for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) killOne(*it);
	for (DWORD pid : companions) killOne(pid);

	if (scm != nullptr) CloseServiceHandle(scm);
	return killed;
}

// 「结束」之后的盯梢。
//
// 红蜘蛛这类产品有服务或守护进程盯着：学生端被杀，几秒后又被拉起来，界面上看
// 就是「点了没反应」。结束那一下能杀的都杀了，之后每 2 秒再看一眼，重新出现的
// 再收一次，并把父进程写进日志 —— 万一是内核里的东西在拉，这条日志就是线索。
void TrainerWorkerInternal::StepEduKillWatch()
{
	if (!_EduKillWatchActive) return;
	const ULONGLONG now = GetTickCount64();
	if (now >= _EduKillWatchDeadline || now >= _EduKillWatchHardDeadline) {
		_EduKillWatchActive = false;
		currentLogger->LogInfo(L"电子教室盯梢结束，共收了 %d 轮", _EduKillWatchRounds);
		return;
	}

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return;

	struct Hit { DWORD pid; DWORD parent; std::wstring name; bool guardOnly; };
	std::vector<Hit> hits;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		const DWORD pid = entry.th32ProcessID;
		if (pid <= 4 || pid == GetCurrentProcessId()) continue;

		bool isHost = false;
		for (const auto& name : _EduKillWatchHosts)
			if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) { isHost = true; break; }
		bool isGuard = false;
		if (!isHost) {
			for (const auto& name : _EduKillWatchGuards)
				if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) { isGuard = true; break; }
		}
		if (!isHost && !isGuard) continue;

		// 学生端同名的程序不少（Student.exe 满地都是），只认落在安装目录里的。
		// 上游进程名足够特别，只看名字。
		if (!_EduKillWatchDirectories.empty()) {
			std::wstring path;
			if (!QueryProcessImagePath(pid, &path)) continue;
			bool inDirectory = false;
			for (const auto& directory : _EduKillWatchDirectories)
				if (IsUnderDirectory(path, directory)) { inDirectory = true; break; }
			if (!inDirectory) continue;
		}

		Hit hit;
		hit.pid = pid;
		hit.parent = entry.th32ParentProcessID;
		hit.name = entry.szExeFile;
		hit.guardOnly = isGuard;
		hits.push_back(hit);
	}
	CloseHandle(snapshot);
	if (hits.empty()) return;

	++_EduKillWatchRounds;
	std::map<DWORD, std::wstring> serviceByPid;
	BuildServicePidMap(&serviceByPid);
	SC_HANDLE scm = serviceByPid.empty() ? nullptr : OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);

	// 纯守护一律挂起：杀掉会被 SCM 按恢复策略拉回来，等于陪着它转圈。
	// 学生端本身还是要结束，它才是用户看到的那层。
	auto stopOne = [&](DWORD pid, const std::wstring& name, bool suspendOnly) {
		if (suspendOnly && MSuspendProcessNt(pid, nullptr) == STATUS_SUCCESS) {
			currentLogger->LogInfo(L"已挂起守护进程 %s [PID %lu]", name.c_str(), pid);
			return;
		}
		auto service = serviceByPid.find(pid);
		if (service != serviceByPid.end()) {
			if (scm != nullptr) StopServiceEntry(scm, service->second.c_str());
			if (IsProcessGone(pid)) return;
			if (MSuspendProcessNt(pid, nullptr) == STATUS_SUCCESS) return;
		}
		KillProcess(pid, true);
		currentLogger->LogInfo(L"已再次结束电子教室 %s [PID %lu]", name.c_str(), pid);
	};

	for (const auto& hit : hits) {
		std::wstring parentPath;
		if (hit.parent > 4) QueryProcessImagePath(hit.parent, &parentPath);
		currentLogger->LogWarn(L"电子教室 %s [PID %lu] 又被拉起，父进程 [PID %lu] %s",
			hit.name.c_str(), hit.pid, hit.parent,
			parentPath.empty() ? L"（取不到路径）" : parentPath.c_str());

		// 先把上游摁住，再收它拉起来的那个。系统目录下的父进程不动（explorer、
		// services 这些），否则会顺手带走别的东西。
		if (hit.parent > 4 && hit.parent != GetCurrentProcessId() && !parentPath.empty() &&
			!IsSystemProcessImage(parentPath) && !IsProcessGone(hit.parent)) {
			const std::wstring parentName = parentPath.substr(parentPath.find_last_of(L"\\/") + 1);
			bool parentGuardOnly = false;
			for (const auto& name : _EduKillWatchGuards)
				if (_wcsicmp(parentName.c_str(), name.c_str()) == 0) { parentGuardOnly = true; break; }
			for (const auto& name : _EduKillWatchHosts)
				if (_wcsicmp(parentName.c_str(), name.c_str()) == 0) { parentGuardOnly = false; break; }
			currentLogger->LogWarn(L"结束它的上游 %s [PID %lu]", parentName.c_str(), hit.parent);
			stopOne(hit.parent, parentName, parentGuardOnly);
		}
		stopOne(hit.pid, hit.name, hit.guardOnly);
	}

	if (scm != nullptr) CloseServiceHandle(scm);
	EduKillWatchExtend();
}

// 还在往外冒就多看一会儿，但总量封顶，别让界面线程一直陪着。
void TrainerWorkerInternal::EduKillWatchExtend()
{
	const ULONGLONG now = GetTickCount64();
	const ULONGLONG extended = now + 30000;
	_EduKillWatchDeadline = extended > _EduKillWatchHardDeadline ? _EduKillWatchHardDeadline : extended;
}


// 「结束电子教室」里收拾其他教室的那一半。极域走的是 Kill()，这里是通用
// 宿主：产品表里认得的学生端都要收拾，连它的同目录进程和上游一起。
// product 给具体产品名时只动那一款，界面的选择框按名字逐个调。
int TrainerWorkerInternal::KillEduHosts(LPCWSTR product)
{
	const bool all = (product == nullptr || product[0] == L'\0');

	// 盯梢名单先立起来：这一下没收拾干净的话，StepEduKillWatch() 接着收。
	// 连着结束两款时不再区分产品，按「全部」记。
	const bool firstCall = !_EduKillWatchActive;
	if (firstCall) {
		_EduKillWatchProduct = all ? L"" : product;
		_EduKillWatchHosts.clear();
		_EduKillWatchGuards.clear();
		_EduKillWatchDirectories.clear();
		_EduKillWatchRounds = 0;
	}
	else if (all) _EduKillWatchProduct.clear();
	else if (!_EduKillWatchProduct.empty() && _wcsicmp(_EduKillWatchProduct.c_str(), product) != 0)
		_EduKillWatchProduct.clear();

	for (int i = 0; i < kEduProductCount; ++i) {
		if (!all && _wcsicmp(kEduProducts[i].name, product) != 0) continue;
		const EduProduct& item = kEduProducts[i];
		for (int r = 0; r < item.ruleCount; ++r) AppendUniqueName(&_EduKillWatchHosts, item.rules[r].exe);
		for (int g = 0; g < item.guardCount; ++g) AppendUniqueName(&_EduKillWatchGuards, item.guards[g].exe);
	}

	// 先把上游停掉。红蜘蛛的学生端是 checkrs.exe（服务）拉起来的，直接杀
	// REDAgent 的话，服务几秒后就把学生端重新启起来，用户看到的是「点了没用」。
	// 这里不看注入记录：学生端刚被守护拉起来、还没轮到扫描时也该能结束。
	// 但只对真的在跑的那几款动手 —— 结束极域时不该顺带把别家的守护挂起。
	for (int i = 0; i < kEduProductCount; ++i) {
		if (!all && _wcsicmp(kEduProducts[i].name, product) != 0) continue;
		if (!AnyProductHostRunning(kEduProducts[i]) && GetEduHostCountOf(kEduProducts[i].name) <= 0)
			continue;
		StopEduGuards(kEduProducts[i]);
	}

	// 先把目标快照下来：清扫过程中会顺手结束别的进程，边遍历边改列表容易漏。
	//
	// 目标有两个来源。产品表扫出来的是「正在跑但可能还没被注入」的（守护拉起
	// 学生端到下一轮扫描之间有十秒空档，菜单上的结束项就是冲着它去的）；注入
	// 记录补上产品表认不出的情况，比如用户装了别的版本、目录特征对不上。
	struct Target { DWORD pid; std::wstring name; std::wstring path; };
	std::vector<Target> targets;
	auto addTarget = [&targets](DWORD pid) {
		if (pid <= 4) return;
		for (const auto& existing : targets) if (existing.pid == pid) return;
		Target target;
		target.pid = pid;
		if (!QueryProcessImagePath(pid, &target.path)) return;
		const size_t slash = target.path.find_last_of(L"\\/");
		target.name = slash == std::wstring::npos ? target.path : target.path.substr(slash + 1);
		targets.push_back(target);
	};

	for (int i = 0; i < kEduProductCount; ++i) {
		if (!all && _wcsicmp(kEduProducts[i].name, product) != 0) continue;
		std::vector<DWORD> pids;
		CollectProductHostPids(kEduProducts[i], &pids);
		for (DWORD pid : pids) addTarget(pid);
	}
	for (const auto& item : _CustomInjected) {
		if (item.product.empty()) continue;
		if (!all && _wcsicmp(item.product.c_str(), product) != 0) continue;
		addTarget(item.pid);
	}
	if (targets.empty()) return 0;

	for (const auto& target : targets) {
		const size_t slash = target.path.find_last_of(L"\\/");
		if (slash != std::wstring::npos && slash > 2)
			AppendUniqueName(&_EduKillWatchDirectories, target.path.substr(0, slash).c_str());
	}

	const ULONGLONG now = GetTickCount64();
	_EduKillWatchDeadline = now + 90000;
	_EduKillWatchHardDeadline = now + 8 * 60 * 1000;
	_EduKillWatchActive = true;

	int killed = 0;
	for (const auto& target : targets)
		killed += SweepEduCompanions(target.pid, target.path);

	// 注入记录按「进程还在不在」收尾：连带的进程被清掉之后，宿主多半也没了。
	// TerminateProcess 成功到进程真的消失之间有空档，等一下再判，否则成功的会被
	// 记成失败。
	int removed = 0;
	for (auto it = _CustomInjected.begin(); it != _CustomInjected.end(); ) {
		if (it->product.empty()) { ++it; continue; }
		if (!all && _wcsicmp(it->product.c_str(), product) != 0) { ++it; continue; }
		if (IsProcessGone(it->pid) || WaitProcessGone(it->pid, 1500)) {
			currentLogger->LogInfo(L"已结束电子教室 %s [PID %lu]", it->name.c_str(), it->pid);
			it = _CustomInjected.erase(it);
			++removed;
		}
		else {
			currentLogger->LogWarn(L"结束电子教室 %s [PID %lu] 失败，转入盯梢", it->name.c_str(), it->pid);
			++it;
		}
	}

	if (killed > 0 || removed > 0) {
		NotifyEduHosts();
		UpdateState();
	}
	return killed > removed ? killed : removed;
}

int TrainerWorkerInternal::GetEduHostCountOf(LPCWSTR product)
{
	const bool all = (product == nullptr || product[0] == L'\0');
	int count = 0;
	for (const auto& item : _CustomInjected) {
		if (item.product.empty()) continue;
		if (!all && _wcsicmp(item.product.c_str(), product) != 0) continue;
		++count;
	}
	return count;
}

// 菜单据此决定「结束」这一条要不要出现。注入记录为空不代表没在跑：学生端
// 刚被守护进程拉起来的那十来秒里就是这种状态，用户看得见它在跑，菜单里却
// 只有「启动」，点哪都不对。
bool TrainerWorkerInternal::IsEduProductRunning(LPCWSTR product)
{
	const EduProduct* item = FindEduProduct(product);
	if (item == nullptr) return false;
	if (GetEduHostCountOf(product) > 0) return true;
	return AnyProductHostRunning(*item);
}

std::wstring TrainerWorkerInternal::GetEduPendingSummary()
{
	std::wstring text;
	int products = 0;
	for (int i = 0; i < kEduProductCount; ++i) {
		if (GetEduHostCountOf(kEduProducts[i].name) > 0) continue;  // 已接管的走另一档
		if (!AnyProductHostRunning(kEduProducts[i])) continue;
		if (products > 0) text += L"、";
		text += kEduProducts[i].name;
		++products;
	}
	return text;
}

// 菜单里要不要给「启动」这一条，看这里。判据要便宜：菜单是一次点击里现拼的。
bool TrainerWorkerInternal::CanStartEduHost(LPCWSTR product)
{
	if (product == nullptr || product[0] == L'\0') return false;
	if (GetEduHostCountOf(product) > 0) return false;

	for (int i = 0; i < kEduProductCount; ++i) {
		const EduProduct& item = kEduProducts[i];
		if (_wcsicmp(item.name, product) != 0) continue;
		// 学生端已经有进程在跑（只是还没被接管），那不叫「可以启动」。
		if (AnyProductHostRunning(item)) return false;
		// 本轮注入过、后来被结束掉的：路径还留着，一定能拉起来。
		if (_EduLaunchPaths.find(product) != _EduLaunchPaths.end()) return true;
		// 守护进程还挂在内存里（多半是我们自己挂起的），恢复它就能连锁拉起学生端。
		for (int g = 0; g < item.guardCount; ++g) {
			std::vector<DWORD> pids;
			CollectProcessIdsByName(item.guards[g].exe, &pids);
			if (!pids.empty()) return true;
		}
	}
	return false;
}

bool TrainerWorkerInternal::StartEduHost(LPCWSTR product)
{
	const bool all = (product == nullptr || product[0] == L'\0');

	// 盯梢得先撤：还在盯着的话，起来一个就被收一个，按下去等于没按。
	if (_EduKillWatchActive &&
		(all || _EduKillWatchProduct.empty() ||
			_wcsicmp(_EduKillWatchProduct.c_str(), product) == 0)) {
		_EduKillWatchActive = false;
		_EduKillWatchProduct.clear();
		_EduKillWatchHosts.clear();
		_EduKillWatchGuards.clear();
		_EduKillWatchDirectories.clear();
		currentLogger->LogInfo(L"已停止电子教室盯梢（要求启动）");
	}

	bool acted = false;
	for (int i = 0; i < kEduProductCount; ++i) {
		const EduProduct& item = kEduProducts[i];
		if (!all && _wcsicmp(item.name, product) != 0) continue;
		if (IsEduProductRunning(item.name)) {
			currentLogger->LogInfo(L"[%s] 已经在运行，不需要启动", item.name);
			continue;
		}

		// 起服务
		bool serviceStarted = false;
		for (int g = 0; g < item.guardCount; ++g)
			if (StartServicesByImage(item.guards[g].exe, item.guards[g].service) > 0) serviceStarted = true;

		// 服务起不来（被禁用、被安全软件拦住、或者压根没有服务）就恢复被挂起的
		// 守护进程。恢复不触发 SCM 的恢复计数，比重新起一个干净。
		bool guardResumed = false;
		for (int g = 0; g < item.guardCount; ++g) {
			std::vector<DWORD> pids;
			CollectProcessIdsByName(item.guards[g].exe, &pids);
			for (DWORD pid : pids) {
				if (MResumeProcessNt(pid, nullptr) == STATUS_SUCCESS) {
					currentLogger->LogInfo(L"已恢复守护进程 %s [PID %lu]", item.guards[g].exe, pid);
					guardResumed = true;
					// 已经醒了，别让它留在挂起表里，否则下一次结束会以为它还在睡。
					for (auto it = _EduSuspendedGuards.begin(); it != _EduSuspendedGuards.end(); ) {
						if (*it == pid) it = _EduSuspendedGuards.erase(it);
						else ++it;
					}
				}
			}
		}

		// 学生端已经在了，这就够了。
		if (AnyProductHostRunning(item)) {
			acted = true;
			continue;
		}

		// 服务和守护都动过，剩下的交给它们。红蜘蛛从恢复 checkrs 到拉起 REDAgent
		// 要十秒上下，原来在这里等 800 毫秒就判「没起来」，菜单弹「无法启动」，
		// 而它其实几秒后自己就出来了 —— 用户看到的就是「说没启动，其实启动了」。
		if (serviceStarted || guardResumed) {
			currentLogger->LogInfo(L"[%s] 已让守护进程去拉学生端，稍等它自己起来", item.name);
			acted = true;
			continue;
		}

		// 服务没起、守护进程也没有，才按记下的路径自己拉一个：
		// 联想传奇这种没有服务、守护进程又被结束掉的，只剩这一条路。
		auto saved = _EduLaunchPaths.find(item.name);
		if (saved == _EduLaunchPaths.end() || saved->second.empty()) {
			currentLogger->LogWarn(L"[%s] 没有在运行，也没记下它装在哪，无法启动", item.name);
			continue;
		}
		if (SysHlp::RunApplication(saved->second.c_str(), NULL)) {
			currentLogger->LogInfo(L"已启动电子教室 %s（%s）", item.name, saved->second.c_str());
			acted = true;
		}
		else {
			currentLogger->LogWarn(L"启动电子教室 %s 失败：%s", item.name, saved->second.c_str());
		}
	}

	// 守护进程拉起学生端要几秒到十几秒。这段时间把扫描间隔收紧到 2 秒，起来
	// 就立刻接管，界面标题不用等到下一个十秒窗口才改口。
	if (acted) {
		_EduStartWatchUntil = GetTickCount64() + 30000;
		if (_EduCompatEnabled) _LastEduCompatScan = 0;
	}
	return acted;
}

void TrainerWorkerInternal::SetEduCompatAutoInject(bool enabled)
{
	if (_EduCompatEnabled == enabled) return;
	_EduCompatEnabled = enabled;
	// 下次定时器一响就生效，不用等满 10 秒的扫描间隔。
	_LastEduCompatScan = 0;
	// 关掉时把失败记录清空：重新打开后应该立刻重试，而不是接着压 10 分钟。
	if (enabled) _EduInjectBlocked.clear();
	currentLogger->LogInfo(L"EduCompat autoInject=%d（界面切换）", _EduCompatEnabled ? 1 : 0);
}

void TrainerWorkerInternal::NotifyEduHosts()
{
	if (!_Callback) return;
	std::wstring summary = GetEduHostSummary();
	// 还没接管、但进程已经在跑的产品也报给界面：那个数（count）仍是 0，
	// 界面靠 summary 非空认出「待接管」，不然卡片会写「未运行」。
	if (summary.empty()) summary = GetEduPendingSummary();
	_Callback->OnUpdateEduHosts(GetEduHostCount(), summary.c_str());
	// 标题里要报产品名，注入成功当刻就刷新一次，别等下一轮 PID 重置。
	UpdateState();
}

void TrainerWorkerInternal::SyncCustomHosts()
{
	// 注入实例的窗口标题和极域不同，FindWindow 那套命令到不了它们，
	// 只能靠这条广播。对端收到 reset 会重读 ini 并补装/拆掉钩子。
	const int hosts = MsgCenterSendToCustomHosts(L"hk:reset", hWndMain);
	if (hosts > 0)
		currentLogger->LogInfo(L"已向 %d 个电子教室宿主同步显示设置", hosts);
}

bool TrainerWorkerInternal::IsCustomInjected(DWORD pid)
{
	for (const auto& item : _CustomInjected)
		if (item.pid == pid) return true;
	return false;
}

void TrainerWorkerInternal::UnloadAllCustomInjected()
{
	for (const auto& item : _CustomInjected) {
		if (IsProcessGone(item.pid)) {
			currentLogger->LogInfo(L"退出时跳过已退出的自定义注入 pid=%lu name=%s",
				item.pid, item.name.c_str());
			continue;
		}
		bool ok = false;
		if (item.is64) {
			const LPCWSTR dll = currentApp ? currentApp->GetPartFullPath(PART_HOOKER64) : nullptr;
			std::wstring ignored;
			ok = dll && RunInjector64(item.pid, dll, true, &ignored);
		}
		else {
			RemoteInvokeVUnloadAll(item.pid,
				currentApp ? currentApp->GetPartFullPath(PART_HOOKER) : L"",
				L"DzjsTrainerHooks.dll");
			ok = UnInjectDll(item.pid, L"DzjsTrainerHooks.dll");
		}
		currentLogger->LogInfo(L"退出时卸载自定义注入 pid=%lu is64=%d result=%d",
			item.pid, item.is64, ok);
	}
	_CustomInjected.clear();
}

bool TrainerWorkerInternal::UnLoadAllVirus()
{
	UnloadAllCustomInjected();
	if (_MasterHelperPid > 4) {
		if (UnInjectDll(_MasterHelperPid, L"DzjsTrainerHooks.dll"))
			currentLogger->Log(L"已强制卸载 MasterHelper 病毒");
		//KillProcess(_MasterHelperPid, false);
	}
	if (_StudentMainPid > 4)
		if (UnInjectDll(_StudentMainPid, L"DzjsTrainerHooks.dll"))
			currentLogger->Log(L"已强制卸载 StudentMain 病毒");

	return false;
}
bool TrainerWorkerInternal::UnLoadNetFilterDriver()
{
	return MUnLoadDriverServiceWithMessage(L"TDNetFilter");
}
bool TrainerWorkerInternal::UnLoadJiYuProtectDriver()
{
	return false;
}

bool TrainerWorkerInternal::SwitchFakeFull()
{
	if (_FakeFull) { 
		_FakeFull = false; 
		SendMessageToVirus(L"hk:fkfull:false");
		FakeFull(_FakeFull);
	}
	else if(_StudentMainRunningLock) { 
		_FakeFull = true; 
		SendMessageToVirus(L"hk:fkfull:true");
		FakeFull(_FakeFull);
	}

	return _FakeFull;
}
void TrainerWorkerInternal::FakeFull(bool fk) {
	if (_CurrentBroadcastWnd)
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBroadcastWnd, GWL_STYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBroadcastWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			/*HWND jiYuGBDeskRdWnd = FindWindowExW(currentGbWnd, NULL, NULL, L"TDDesk Render Window");
			if (jiYuGBDeskRdWnd != NULL) {

			}*/
			_FakeBroadcastFull = true;
			currentLogger->Log(L"调整广播窗口假装全屏状态");
		}
		else {
			_FakeBroadcastFull = false;
			FixWindow(_CurrentBroadcastWnd, (LPWSTR)L"屏幕广播");
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			SetWindowPos(_CurrentBroadcastWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
			currentLogger->Log(L"取消广播窗口假装全屏状态");
		}
	}
	if (_CurrentBlackScreenWnd) 
	{
		if (fk) {
			SetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBlackScreenWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			_FakeBlackScreenFull = true;
			currentLogger->Log(L"调整黑屏窗口假装全屏状态");
		}
		else {
			_FakeBlackScreenFull = false;
			FixWindow(_CurrentBlackScreenWnd, (LPWSTR)L"BlackScreen Window");
			currentLogger->Log(L"取消黑屏窗口假装全屏状态");
		}
	}
	if (!fk && !_CurrentBlackScreenWnd && !_CurrentBroadcastWnd && (_FakeBlackScreenFull || _FakeBroadcastFull)) {
		_FakeBroadcastFull = false;
		_FakeBlackScreenFull = false;
	}
}
void TrainerWorkerInternal::ManualFull(bool fk)
{
	currentLogger->Log(L"receive ManualFull %s", fk ? L"true" : L"false");
	if (_CurrentBroadcastWnd) 
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBroadcastWnd, GWL_STYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW) | WS_SYSMENU);
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBroadcastWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
		}
		else {
			FixWindow(_CurrentBroadcastWnd, (LPWSTR)L"屏幕广播");
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			SetWindowPos(_CurrentBroadcastWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
		}
	}
}
void TrainerWorkerInternal::ManualTop(bool fk)
{
	currentLogger->Log(L"receive ManualTop %s", fk ? L"true" : L"false");
	if (_CurrentBroadcastWnd)
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		else {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) ^ WS_EX_TOPMOST);
			SetWindowPos(_CurrentBroadcastWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}
}
bool TrainerWorkerInternal::ChecIsJIYuWindow(HWND hWnd, LPDWORD outPid, LPDWORD outTid) {
	if (_StudentMainPid == 0) return false;
	DWORD pid = 0, tid = GetWindowThreadProcessId(hWnd, &pid);
	if (outPid) *outPid = pid;
	if (outTid) *outTid = tid;
	return pid == _StudentMainPid;
}
bool TrainerWorkerInternal::CheckIsTargetWindow(LPWSTR text, HWND hWnd) {
	bool b = false;
	
	if (StrEqual(text, L"BlackScreen Window")) {
		b = true;
		_LastResoveBlackScreenWindow = true;
		if (!_FirstBlackScreenWindow) {
			_FirstBlackScreenWindow = true;
			if (_Callback) _Callback->OnResolveBlackScreenWindow();
		}
		_CurrentBlackScreenWnd = hWnd;
		if (_FakeBlackScreenFull) return false;
	}
	else if (CheckWindowTextIsGb(text)) {
		b = true;
		_LastResoveBroadcastWindow = true;
		_CurrentBroadcastWnd = hWnd;
		if (_FakeBroadcastFull) return false;
	}
	return b;
}
void TrainerWorkerInternal::FixWindow(HWND hWnd, LPWSTR text)
{
	_LastResolveWindowCount++;

	LONG oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);

	if (StrEqual(text, L"BlackScreen Window"))
	{
		oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
			SetWindowPos(hWnd, 0, 20, 20, 90, 150, SWP_NOZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
			ShowWindow(hWnd, SW_HIDE);
		}
	}
	//Un top
	if (CheckWindowTextIsGb(text))
	{
		if (!setAllowGbTop && (oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_TOPMOST);
			SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		if (!gbFullManual) 
		{
			//Set border and sizeable
			SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));
		}
	}
	else if(setAutoIncludeFullWindow)
	{
		if ((oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_TOPMOST);
			SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}

		//Set border and sizeable
		SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));

	}

	SetWindowPos(hWnd, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE | SWP_DRAWFRAME | SWP_NOACTIVATE);
}
bool TrainerWorkerInternal::CheckWindowTextIsGb(const wchar_t* text) {
	return StringHlp::StrContainsW(text, L"广播", nullptr) || StringHlp::StrContainsW(text, L"演示", nullptr)
		|| StringHlp::StrContainsW(text, L"共享", nullptr)
		|| StringHlp::StrEqualW(text, L"屏幕演播室窗口");
}

BOOL CALLBACK TrainerWorkerInternal::EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
	TrainerWorkerInternal *self =(TrainerWorkerInternal *)lParam;
	if (IsWindowVisible(hWnd) && self->ChecIsJIYuWindow(hWnd, NULL, NULL)) {
		WCHAR text[50];
		GetWindowText(hWnd, text, 50);
		if (StrEqual(text, L"JiYu Trainer Virus Window")) return TRUE;

		RECT rc;
		GetWindowRect(hWnd, &rc);
		if (self->CheckIsTargetWindow(text, hWnd)) {
			//JiYu window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
		else if (self->setAutoIncludeFullWindow && rc.top == 0 && rc.left == 0 && rc.right == self->screenWidth && rc.bottom == self->screenHeight) {
			//Full window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
	}
	return TRUE;
}
VOID CALLBACK TrainerWorkerInternal::TimerProc(HWND hWnd, UINT message, UINT_PTR iTimerID, DWORD dwTime)
{
	if (currentTrainerWorker != nullptr) 
	{
		if (iTimerID == TIMER_RESET_PID) {
			currentTrainerWorker->RunResetPid();
		}
		if (iTimerID == TIMER_CK) {
			currentTrainerWorker->RunCk();
		}
	}
}
LRESULT CALLBACK TrainerWorkerInternal::CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode < 0)
		return CallNextHookEx(currentTrainerWorker->hMsgBoxHook, nCode, wParam, lParam);
	switch (nCode)
	{
	case HCBT_ACTIVATE: {
		// 现在wParam中就是message box的句柄
		HWND hWnd = (HWND)wParam;
		HWND hWndOwner = GetWindow(hWnd, GW_OWNER);

		// 我们已经有了message box的句柄，在这里我们就可以定制message box了!
		if (hWndOwner && hWndOwner  == currentTrainerWorker->hWndMain)
		{
			//窗口在父窗口居中
			RECT rect; GetWindowRect(hWnd, &rect);
			RECT rectParent; GetWindowRect(hWndOwner, &rectParent);
			rect.left = ((rectParent.right - rectParent.left) - (rect.right - rect.left)) / 2 + rectParent.left;
			rect.top = ((rectParent.bottom - rectParent.top) - (rect.bottom - rect.top)) / 2 + rectParent.top;
			SetWindowPos(hWnd, 0, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}
		return 0;
	}
	}
	// Call the next hook, if there is one
	return CallNextHookEx(currentTrainerWorker->hMsgBoxHook, nCode, wParam, lParam);
}

bool UnDecryptJiyuKnock(BYTE* Data, DWORD cbData, WCHAR* ss)
{
	//反编译的代码
	__try {
		DWORD v5; // esi
		DWORD v6; // ecx
		BYTE *v7; // eax
		DWORD v8; // edx
		BYTE *i;
		v5 = cbData;
		v6 = cbData >> 2;
		v7 = Data;
		if (cbData >> 2)
		{
			v8 = cbData >> 2;
			do
			{
				*(DWORD *)v7 ^= 0x50434C45u;
				v7 += 4;
				--v8;
			} while (v8);
		}
		for (i = Data; v6; --v6)
		{
			*(DWORD *)i ^= 0x454C4350u;
			i += 4;
		}
		WORD v4[34];
		v4[0] = 0;
		memset(&v4[1], 0, 0x40u);

		int a1 = (int)&v4;

		int v13; // edi
		BYTE *v14; // eax
		__int16 v15; // cx
		v13 = a1 - Data[0];
		v14 = &Data[Data[0]];
		do
		{
			v15 = *(WORD *)v14;
			*(WORD *)&v14[v13 - (DWORD)Data] = *(WORD *)v14;
			v14 += 2;
		} while (v15);


		for (int i = 0; i < 32; i++) ss[i] = v4[i];
		return true;
	}
	__except (1) {
		return false;
	}
}
