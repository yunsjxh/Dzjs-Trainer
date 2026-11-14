#include "HooksCommon.h"
#include "GuardProcess.h"

// ===== GuardProcess ============================================================
// 从 DzjsTrainerHooks.cpp 机械迁出，函数体逐字节保持原样。

BOOL WINAPI hkDeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned,  LPOVERLAPPED lpOverlapped) {
	VTraceHook(L"hkDeviceIoControl");
	// 只记录：没打开「拦截极域驱动访问」时一律放行。
	if (!blockDriverAccess) {
		VRecordHook(L"DeviceIoControl", L"device=%p code=0x%lX", hDevice, dwIoControlCode);
		return raDeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
	}
	if (hDeviceTDKeybd) {
		if (hDevice == hDeviceTDKeybd) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (hDeviceTDProcHook) {
		if (hDevice == hDeviceTDProcHook) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (hDeviceTDNetFilter) {
		// 极域靠 TDNetFilter 连教师端。allowAllRunOp 为真（界面上「拦截远程运行程序」是关的）
		// 表示不干预远程行为，此时必须放行 —— 否则极域连不上教师端，界面会卡死。
		// 原先这里无条件挡掉，全量集一装就必然出问题。
		if (!allowAllRunOp && hDevice == hDeviceTDNetFilter) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	return raDeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
}

HANDLE WINAPI hkCreateFileA(LPCSTR lpFileName,  DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile ) {
	VTraceHook(L"hkCreateFileA");
	if (!blockDriverAccess) {
		VRecordHook(L"CreateFileA", L"name=%S", lpFileName ? lpFileName : "");
		return faCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}
	if (loaded)
	{
		if (StringHlp::StrEqualA(lpFileName, "\\\\.\\TDKeybd")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualA(lpFileName, "\\\\.\\TDProcHook")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (!allowAllRunOp && StringHlp::StrEqualA(lpFileName, "\\\\.\\TDNetFilter")) {
			// 极域靠 TDNetFilter 连教师端；allowAllRunOp 为真时必须放行，否则会卡死。
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
	}
	return faCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI hkCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,  DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
	VTraceHook(L"hkCreateFileW");
	if (!blockDriverAccess) {
		VRecordHook(L"CreateFileW", L"name=%s", lpFileName ? lpFileName : L"");
		return faCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}
	if (loaded)
	{
		if (StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDKeybd")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDProcHook")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		if (!allowAllRunOp && StringHlp::StrEqualW(lpFileName, L"\\\\.\\TDNetFilter")) {
			// 同上：allowAllRunOp 为真（不干预远程行为）时不能挡极域的网络过滤驱动。
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
	}
	return faCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL WINAPI hkExitWindowsEx(UINT uFlags, DWORD dwReason) {
	VTraceHook(L"hkExitWindowsEx");
	// 只记录：没打开「阻止远程结束进程 / 关机」时直接放行，也不弹确认框。
	if (!ProhibitKillProcess) {
		VRecordHook(L"ExitWindowsEx", L"flags=0x%X", uFlags);
		return faExitWindowsEx(uFlags, dwReason);
	}
	const wchar_t* op = L"";
	if((uFlags & EWX_POWEROFF) == EWX_POWEROFF) op = L"关闭您的计算机";
	else if ((uFlags & EWX_REBOOT) == EWX_REBOOT) op = L"重启您的计算机";
	else if ((uFlags & EWX_LOGOFF) == EWX_LOGOFF) op = L"注销您的计算机";
	wchar_t str[37];
	swprintf_s(str, L"极域电子教室试图%s，是否允许极域继续操作？", op);
	if (MessageBox(NULL, str, L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDYES) 
		return faExitWindowsEx(uFlags, dwReason);
	SetLastError(ERROR_ACCESS_DENIED);
	return FALSE;
}

HINSTANCE WINAPI hkShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR lpDirectory, INT nShowCmd) {

	/*if (StrEqual(lpOperation, L"open") || StrEqual(lpOperation, L"runas")) {
		if (VGetExtension(wstring(lpFile)) == L".exe") {
			if (bandAllRunOp || !VShowOpConfirmDialog(lpFile, lpParameters))
				return hInst;
		}
	}*/
	return faShellExecuteW(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, nShowCmd);
}

BOOL WINAPI hkShellExecuteExW(SHELLEXECUTEINFOW *pExecInfo) {

	/*if (StrEqual(pExecInfo->lpFile, L"open") || StrEqual(pExecInfo->lpFile, L"runas")) {
		if (bandAllRunOp || VGetExtension(wstring(pExecInfo->lpFile)) == L".exe") {
			if (!VShowOpConfirmDialog(pExecInfo->lpFile, pExecInfo->lpParameters)) 
				return TRUE;
		}
	}*/
	return faShellExecuteExW(pExecInfo);

}

BOOL VInjectHookIntoProcess(HANDLE process)
{
	if (!process)
		return FALSE;

	WCHAR modulePath[MAX_PATH] = { 0 };
	DWORD pathLength = GetModuleFileNameW(hInst, modulePath, _countof(modulePath));
	if (pathLength == 0 || pathLength >= _countof(modulePath))
	{
		VDebugLog(L"capture-helper load skipped reason=module-path error=%lu", GetLastError());
		return FALSE;
	}

	const SIZE_T pathBytes = (static_cast<SIZE_T>(pathLength) + 1) * sizeof(WCHAR);
	LPVOID remotePath = VirtualAllocEx(process, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remotePath)
	{
		VDebugLog(L"capture-helper load failed stage=VirtualAllocEx error=%lu", GetLastError());
		return FALSE;
	}

	SIZE_T written = 0;
	if (!WriteProcessMemory(process, remotePath, modulePath, pathBytes, &written) || written != pathBytes)
	{
		VDebugLog(L"capture-helper load failed stage=WriteProcessMemory error=%lu written=%Iu expected=%Iu", GetLastError(), written, pathBytes);
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	LPTHREAD_START_ROUTINE loadLibrary = kernel32
		? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"))
		: NULL;
	if (!loadLibrary)
	{
		VDebugLog(L"capture-helper load failed stage=LoadLibraryW-address error=%lu", GetLastError());
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	HANDLE remoteThread = CreateRemoteThread(process, NULL, 0, loadLibrary, remotePath, 0, NULL);
	if (!remoteThread)
	{
		VDebugLog(L"capture-helper load failed stage=CreateRemoteThread error=%lu", GetLastError());
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		return FALSE;
	}

	DWORD waitResult = WaitForSingleObject(remoteThread, 5000);
	DWORD remoteModule = 0;
	if (waitResult == WAIT_OBJECT_0)
		GetExitCodeThread(remoteThread, &remoteModule);
	BOOL result = waitResult == WAIT_OBJECT_0 && remoteModule != 0;
	VDebugLog(L"capture-helper load result=%d wait=%lu module=%p error=%lu", result, waitResult,
		reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(remoteModule)), result ? ERROR_SUCCESS : GetLastError());
	CloseHandle(remoteThread);
	VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
	return result;
}

BOOL WINAPI hkCreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
	VTraceHook(L"hkCreateProcessW");
	const bool isCaptureHelper =
		(lpApplicationName && StrStrIW(lpApplicationName, L"DispcapHelper.exe")) ||
		(lpCommandLine && StrStrIW(lpCommandLine, L"DispcapHelper.exe"));
	// 往采集助手里注入本模块属于"替换"动作：只有打开了视频流保护才做。
	// 关着的时候连注入都不该发生 —— 否则等于在没被要求的情况下改了别人进程的行为。
	if (isCaptureHelper && VVideoReplacementEnabled())
	{
		const bool callerRequestedSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
		BOOL result = faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
			bInheritHandles, dwCreationFlags | CREATE_SUSPENDED, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
		DWORD error = GetLastError();
		BOOL moduleLoaded = FALSE;
		if (result && lpProcessInformation)
			moduleLoaded = VInjectHookIntoProcess(lpProcessInformation->hProcess);
		if (result && lpProcessInformation && !callerRequestedSuspended)
			ResumeThread(lpProcessInformation->hThread);
		LONG logIndex = InterlockedIncrement(&captureHelperProcessLogCount);
		VDebugLog(L"CreateProcessW capture-helper index=%ld result=%d moduleLoaded=%d error=%lu pid=%lu application=%s command=%s",
			logIndex, result, moduleLoaded, error, result && lpProcessInformation ? lpProcessInformation->dwProcessId : 0,
			lpApplicationName ? lpApplicationName : L"(null)", lpCommandLine ? lpCommandLine : L"(null)");
		SetLastError(error);
		return result;
	}

	bool canContinue = true;
	if (allowAllRunOp) {
		VRecordHook(L"CreateProcessW", L"app=%s",
			lpApplicationName ? lpApplicationName : (lpCommandLine ? lpCommandLine : L""));
		return faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	}

	LPCWSTR exePath = NULL;
	if (!StrEmepty(lpApplicationName) && _waccess_s(lpApplicationName, 0) == 0) exePath = lpApplicationName;
	else if (!StrEmepty(lpCommandLine) && _waccess_s(lpCommandLine, 0) == 0) exePath = lpCommandLine;
	else if(!StrEmepty(lpApplicationName)) exePath = lpApplicationName;
	else if (!StrEmepty(lpCommandLine)) exePath = lpCommandLine;

	LPCWSTR lowStr = StringHlp::StrLoW(exePath);
	if (StringHlp::StrContainsW(lowStr, L"shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	} 
	else if (StringHlp::StrContainsW(lowStr, L"dzjstrainer.exe", NULL)
		|| StringHlp::StrContainsW(lowStr, L"explorer", NULL)
		|| StringHlp::StrContainsW(lowStr, L"sogouinput", NULL)
		|| StringHlp::StrContainsW(lowStr, L"ime", NULL)
		|| StringHlp::StrContainsW(lowStr, L"baidupinyin", NULL)
		|| VIsOpInWhiteList(lowStr)) canContinue = true;
	else if (StringHlp::StrContainsW(lowStr, L"tdchalk.exe", NULL))
		canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(lpApplicationName, lpCommandLine)) canContinue = false;

	if(canContinue) return faCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	else return TRUE;
}

BOOL WINAPI hkCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
{
	VTraceHook(L"hkCreateProcessA");
	const bool isCaptureHelper =
		(lpApplicationName && StrStrIA(lpApplicationName, "DispcapHelper.exe")) ||
		(lpCommandLine && StrStrIA(lpCommandLine, "DispcapHelper.exe"));
	if (isCaptureHelper && VVideoReplacementEnabled())
	{
		const bool callerRequestedSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
		BOOL result = faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
			bInheritHandles, dwCreationFlags | CREATE_SUSPENDED, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
		DWORD error = GetLastError();
		BOOL moduleLoaded = FALSE;
		if (result && lpProcessInformation)
			moduleLoaded = VInjectHookIntoProcess(lpProcessInformation->hProcess);
		if (result && lpProcessInformation && !callerRequestedSuspended)
			ResumeThread(lpProcessInformation->hThread);
		LONG logIndex = InterlockedIncrement(&captureHelperProcessLogCount);
		VDebugLog(L"CreateProcessA capture-helper index=%ld result=%d moduleLoaded=%d error=%lu pid=%lu",
			logIndex, result, moduleLoaded, error, result && lpProcessInformation ? lpProcessInformation->dwProcessId : 0);
		SetLastError(error);
		return result;
	}

	bool canContinue = true;
	if (allowAllRunOp) {
		VRecordHook(L"CreateProcessA", L"app=%S",
			lpApplicationName ? lpApplicationName : (lpCommandLine ? lpCommandLine : ""));
		return faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	}

	LPCWSTR lpApplicationNameW = StringHlp::AnsiToUnicode(lpApplicationName);
	LPCWSTR lpCommandLineW = StringHlp::AnsiToUnicode(lpCommandLine);

	LPCSTR exePath = NULL;
	if (!StringHlp::StrEmeptyA(lpApplicationName) && _waccess_s(lpApplicationNameW, 0) == 0) exePath = lpApplicationName;
	else if (!StringHlp::StrEmeptyA(lpCommandLine) && _waccess_s(lpCommandLineW, 0) == 0) exePath = lpCommandLine;
	else if (!StringHlp::StrEmeptyA(lpApplicationName)) exePath = lpApplicationName;
	else if (!StringHlp::StrEmeptyA(lpCommandLine)) exePath = lpCommandLine;

	LPCSTR lowStr = StringHlp::StrLoA(exePath);
	LPCWSTR lowStrW = StringHlp::AnsiToUnicode(lowStr);

	if (StringHlp::StrContainsA(lowStr, "shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	}
	else if (StringHlp::StrContainsA(lowStr, "dzjstrainer.exe", NULL)
		|| StringHlp::StrContainsA(lowStr, "explorer", NULL)
		|| StringHlp::StrContainsA(lowStr, "sogouinput", NULL)
		|| StringHlp::StrContainsA(lowStr, "ime", NULL)
		|| StringHlp::StrContainsA(lowStr, "baidupinyin", NULL)
		|| VIsOpInWhiteList(lowStrW)) canContinue = true;
	else if (StringHlp::StrContainsA(lowStr, "tdchalk.exe", NULL))
		canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(lpApplicationNameW, lpCommandLineW)) canContinue = false;

	StringHlp::FreeStringPtr(lpApplicationNameW);
	StringHlp::FreeStringPtr(lpCommandLineW);
	StringHlp::FreeStringPtr(lowStrW);

	if (canContinue) return faCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
	else return TRUE;
}

UINT WINAPI hkWinExec(LPCSTR lpCmdLine, UINT uCmdShow) {
	VTraceHook(L"hkWinExec");
	bool canContinue = true;
	if (allowAllRunOp) {
		VRecordHook(L"WinExec", L"cmd=%S", lpCmdLine ? lpCmdLine : "");
		return faWinExec(lpCmdLine, uCmdShow);
	}

	LPCWSTR uniStr = StringHlp::AnsiToUnicode(lpCmdLine);
	LPCWSTR lowStr = StringHlp::StrLoW(uniStr);
	if (StringHlp::StrContainsW(lowStr, L"shutdown.exe", NULL)) {
		if (MessageBox(NULL, L"极域电子教室试图关机或重启，是否允许极域继续操作？", L"JiYu Killer 防护警告", MB_ICONEXCLAMATION | MB_YESNO) == IDNO) canContinue = false;
	}
	else if (StringHlp::StrContainsW(lowStr, L"dzjstrainer.exe", NULL)
		|| StringHlp::StrContainsW(lowStr, L"explorer", NULL)
		|| StringHlp::StrContainsW(lowStr, L"sogouinput", NULL)
		|| StringHlp::StrContainsW(lowStr, L"baidupinyin", NULL)
		|| StringHlp::StrContainsW(lowStr, L"ime", NULL)
		|| VIsOpInWhiteList(lowStr)) canContinue = true;
	else if (StringHlp::StrContainsW(lowStr, L"tdchalk.exe", NULL)) canContinue = false;
	else if (bandAllRunOp || !VShowOpConfirmDialog(uniStr, L"")) canContinue = false;

	delete uniStr;
	if (canContinue) return faWinExec(lpCmdLine, uCmdShow);
	else return 32;
}

BOOL WINAPI hkTerminateProcess(HANDLE hProcess, UINT uExitCode)
{
	VTraceHook(L"hkTerminateProcess");
	if (loaded && ProhibitKillProcess  && hProcess != GetCurrentProcess()) {
		VRecordHook(L"TerminateProcess", L"handle=%p exitCode=%u", hProcess, uExitCode);
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return faTerminateProcess(hProcess, uExitCode);
}

HRESULT WINAPI hkFilterConnectCommunicationPort(LPCWSTR lpPortName, DWORD dwOptions, LPCVOID lpContext, WORD wSizeOfContext, LPSECURITY_ATTRIBUTES lpSecurityAttributes, HANDLE * hPort)
{
	VTraceHook(L"hkFilterConnectCommunicationPort");
	// 只记录：没打开「拦截极域驱动访问」时放行。
	if (!blockDriverAccess) {
		VRecordHook(L"FilterConnectCommunicationPort", L"port=%s", lpPortName ? lpPortName : L"");
		return faFilterConnectCommunicationPort(lpPortName, dwOptions, lpContext, wSizeOfContext, lpSecurityAttributes,  hPort);
	}
	// \TDFileFilterPort 是极域跟它的文件过滤驱动通信的端口。挡掉它，极域会一直等
	// 驱动响应 —— 表现就是"卡死"：钩子都装好了、日志里却没有任何钩子活动。
	// 与 TDNetFilter 同理：allowAllRunOp 为真（不干预远程行为）时必须放行。
	if (loaded && !allowAllRunOp && StrEqual(lpPortName, L"\\TDFileFilterPort")) {
		VDebugLog(L"FilterConnectCommunicationPort blocked port=%s (allowAllRunOp=0)", lpPortName);
		return S_FALSE;
	}
	return faFilterConnectCommunicationPort(lpPortName, dwOptions, lpContext, wSizeOfContext, lpSecurityAttributes,  hPort);
}

BOOL WINAPI hkPostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (loaded && hWnd == mainWindow && VIsInIllegalWindowMessage(Msg) && ProhibitCloseWindow)
		return 0;
	if (loaded && VIsInIllegalWindowMessage(Msg)  && ProhibitCloseWindow) {
		DWORD pid;
		GetWindowThreadProcessId(hWnd, &pid);
		if(pid != currentPid)
			return 0;
	}
	return faPostMessageW(hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI hkSendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (loaded && hWnd == mainWindow) return 0;
	if (loaded && VIsInIllegalWindowMessage(Msg) && ProhibitCloseWindow) {
		DWORD pid;
		GetWindowThreadProcessId(hWnd, &pid);
		if (pid != currentPid)
			return 0;
	}
	return faSendMessageW(hWnd, Msg,  wParam,  lParam);
}
