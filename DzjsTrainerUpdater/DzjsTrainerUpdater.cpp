// DzjsTrainerUpdater.cpp : 主程序侧的更新入口。
//
// 更新的全部工作都在独立的 DzjsTrainerUpdater.exe 里完成：检查版本、下载、
// 校验、替换都由它自己做。主程序只负责把内嵌的更新程序释放出来并启动它，
// 自己不下载、不校验、也不等待父进程退出。
//
// 注意：本模块编译成独立的静态库，**不能直接调用 JTApp 的虚函数**。
// 虚函数走 vtable 槽位，而独立编译的库与主程序的 vtable 布局可能不一致，
// 会调到错误的函数。实际就因此崩溃过：GetSourceInstallerPath 落到了
// GetAppShowCmd 上（返回 1），返回值被当作指针解引用 → 0xC0000005。
// AppPublic.h 提供的 JTAppGetXxxDirect() 系列正是为此存在，一律走它们。

#include "stdafx.h"
#include "DzjsTrainerUpdater.h"
#include "UpdaterManifest.h"
#include "../DzjsTrainer/resource.h"
#include "../DzjsTrainer/AppPublic.h"
#include "../DzjsTrainer/DzjsTrainer.h"
#include "../DzjsTrainer/PathHelper.h"
#include "../DzjsTrainer/SysHlp.h"
#include <Wininet.h>
#include <shellapi.h>

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;

// 前向声明：JUpdater_ProbeManifest 定义在 ExtractBundledUpdater 之前。
static bool ExtractBundledUpdater(LPCWSTR currentDir, std::wstring& updaterPath);

UPEXPORT_CFUNC(BOOL) JUpdater_CheckInternet()
{
	return InternetGetConnectedState(NULL, 0);
}

// 只读探测：拉清单、填进定长缓冲。不下载、不替换、无副作用。
UPEXPORT_CFUNC(BOOL) JUpdater_FetchManifest(JUpdaterManifestInfo* info)
{
	if (!info) return FALSE;
	ZeroMemory(info, sizeof(*info));

	UpdaterManifest::Info manifest;
	const bool ok = UpdaterManifest::Fetch(manifest);
	// 失败原因先落盘（写进跨模块缓冲），再决定返回值：
	// 界面要拿它给用户看，只回 FALSE 等于把三种完全不同的失败混成一句
	// "无法获取更新信息"。
	wcsncpy_s(info->error, _countof(info->error), manifest.error.c_str(), _TRUNCATE);
	if (!ok) return FALSE;

	wcsncpy_s(info->version, _countof(info->version), manifest.version.c_str(), _TRUNCATE);
	wcsncpy_s(info->notes, _countof(info->notes), manifest.notes.c_str(), _TRUNCATE);
	wcsncpy_s(info->url, _countof(info->url), manifest.url.c_str(), _TRUNCATE);
	wcsncpy_s(info->sha256, _countof(info->sha256), manifest.sha256.c_str(), _TRUNCATE);
	return TRUE;
}

UPEXPORT_CFUNC(int) JUpdater_CompareVersion(const wchar_t* left, const wchar_t* right)
{
	return UpdaterManifest::CompareVersions(left ? left : L"", right ? right : L"");
}

// 把清单文本（key=value，一行一个字段）解析到定长缓冲里。
static void ParseManifestText(const std::string& text, JUpdaterManifestInfo* info)
{
	std::string version, sha256, url, notes;
	size_t begin = 0;
	while (begin <= text.size()) {
		const size_t end = text.find('\n', begin);
		std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		const size_t split = line.find('=');
		if (split != std::string::npos) {
			const std::string key = line.substr(0, split);
			std::string value = line.substr(split + 1);
			if (key == "notes") {
				// 写端把换行转义成 \n，这里还原。
				std::string restored;
				for (size_t i = 0; i < value.size(); ++i) {
					if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
						restored.push_back('\n');
						++i;
					} else {
						restored.push_back(value[i]);
					}
				}
				notes = restored;
			} else if (key == "version") version = value;
			else if (key == "sha256") sha256 = value;
			else if (key == "url") url = value;
		}
		if (end == std::string::npos) break;
		begin = end + 1;
	}

	auto toWide = [](const std::string& source, wchar_t* target, size_t count) {
		if (source.empty()) { target[0] = L'\0'; return; }
		const int chars = MultiByteToWideChar(CP_UTF8, 0, source.c_str(),
			static_cast<int>(source.size()), nullptr, 0);
		if (chars <= 0) { target[0] = L'\0'; return; }
		std::wstring wide(static_cast<size_t>(chars), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, source.c_str(), static_cast<int>(source.size()), &wide[0], chars);
		wcsncpy_s(target, count, wide.c_str(), _TRUNCATE);
	};
	toWide(version, info->version, _countof(info->version));
	toWide(notes, info->notes, _countof(info->notes));
	toWide(url, info->url, _countof(info->url));
	toWide(sha256, info->sha256, _countof(info->sha256));
}

// 诊断辅助：把当前线程的令牌状态打进日志。
//
// 两种 OpenAsSelf 都试，这一点很关键：实测 OpenAsSelf=FALSE 在
// SecurityIdentification 级别下会失败并返回 1346，只看它会把"处于 Identification
// 模拟"误判成"没有模拟"。OpenAsSelf=TRUE 则能成功打开。
static void LogThreadTokenState(Logger* logger, LPCWSTR stage)
{
	if (!logger) return;

	HANDLE strictToken = nullptr;
	SetLastError(0);
	const BOOL strictOpened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &strictToken);
	const DWORD strictError = strictOpened ? 0 : GetLastError();
	if (strictToken) CloseHandle(strictToken);

	HANDLE looseToken = nullptr;
	SetLastError(0);
	const BOOL looseOpened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &looseToken);
	const DWORD looseError = looseOpened ? 0 : GetLastError();
	DWORD tokenType = 0;
	DWORD tokenLevel = 0;
	DWORD returned = 0;
	if (looseOpened) {
		GetTokenInformation(looseToken, TokenType, &tokenType, sizeof(tokenType), &returned);
		GetTokenInformation(looseToken, TokenImpersonationLevel, &tokenLevel,
			sizeof(tokenLevel), &returned);
		CloseHandle(looseToken);
	}

	// type: 1=Primary 2=Impersonation；level: 0=Anonymous 1=Identification
	// 2=Impersonation 3=Delegation
	logger->LogError2(L"令牌[%s] FALSE(opened=%d err=%lu) TRUE(opened=%d err=%lu type=%lu level=%lu)",
		stage, static_cast<int>(strictOpened), static_cast<unsigned long>(strictError),
		static_cast<int>(looseOpened), static_cast<unsigned long>(looseError),
		static_cast<unsigned long>(tokenType), static_cast<unsigned long>(tokenLevel));
}

// 诊断辅助：确认启动子进程时受哪些令牌约束。
//
// 背景：CreateProcess 返回 1346(ERROR_BAD_IMPERSONATION_LEVEL)，但
// LogThreadTokenState 显示线程上 OpenThreadToken 两种写法都是 1008(NO_TOKEN)，
// 说明"线程在模拟"这个假设不成立。下面三项是之前缺的数据。
static void LogLaunchEnvironment(Logger* logger, LPCWSTR targetPath)
{
	if (!logger) return;

	// ---- 1) 线程主令牌 ----
	// GetThreadInformation 直接读线程上的主令牌句柄，不需要 TOKEN_QUERY 权限，
	// 因此能绕开 OpenThreadToken 的访问拒绝，区分"没有"与"有但打不开"。
	// ThreadImpersonationToken(=5) 属于 THREADINFOCLASS，声明在 winternl.h；
	// 本翻译单元不引 NT 内部头，直接用数值传递，避免多一个头文件依赖。
	typedef BOOL(WINAPI* PFN_GetThreadInformation)(HANDLE, int, PVOID, DWORD);
	const int kThreadImpersonationToken = 5;
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	PFN_GetThreadInformation getThreadInfo = kernel
		? reinterpret_cast<PFN_GetThreadInformation>(
			reinterpret_cast<void*>(GetProcAddress(kernel, "GetThreadInformation")))
		: nullptr;
	if (getThreadInfo) {
		HANDLE threadPrimary = nullptr;
		const BOOL got = getThreadInfo(GetCurrentThread(), kThreadImpersonationToken,
			&threadPrimary, sizeof(threadPrimary));
		DWORD primaryLevel = 0;
		DWORD primaryType = 0;
		DWORD cb = 0;
		if (got && threadPrimary) {
			GetTokenInformation(threadPrimary, TokenImpersonationLevel, &primaryLevel, sizeof(primaryLevel), &cb);
			GetTokenInformation(threadPrimary, TokenType, &primaryType, sizeof(primaryType), &cb);
			CloseHandle(threadPrimary);
		}
		logger->LogError2(L"线程主令牌 GetThreadInformation=%d token=%p type=%lu level=%lu",
			static_cast<int>(got), threadPrimary,
			static_cast<unsigned long>(primaryType), static_cast<unsigned long>(primaryLevel));
	}
	else {
		logger->LogError2(L"线程主令牌 GetThreadInformation 不可用（系统版本过低）");
	}

	// ---- 2) 进程令牌：受限 / 完整性 / 会话 ----
	HANDLE processToken = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken)) {
		DWORD restricted = 0;
		DWORD cb = 0;
		const BOOL gotRestricted = GetTokenInformation(processToken, TokenIsRestricted,
			&restricted, sizeof(restricted), &cb);

		DWORD integrityRid = 0;
		BYTE labelBuffer[256] = {};
		if (GetTokenInformation(processToken, TokenIntegrityLevel,
			labelBuffer, sizeof(labelBuffer), &cb)) {
			TOKEN_MANDATORY_LABEL* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(labelBuffer);
			if (label->Label.Sid) {
				const UCHAR* countPtr = GetSidSubAuthorityCount(label->Label.Sid);
				if (countPtr && *countPtr > 0) {
					const DWORD* last = GetSidSubAuthority(label->Label.Sid, *countPtr - 1);
					if (last) integrityRid = *last;
				}
			}
		}

		DWORD sessionId = 0;
		GetTokenInformation(processToken, TokenSessionId, &sessionId, sizeof(sessionId), &cb);
		CloseHandle(processToken);

		// restricted=1 表示令牌被 CreateRestrictedToken 收过权限，
		// 这种令牌创建进程时会走不同分支，是 1346 的已知诱因之一。
		logger->LogError2(L"进程令牌 restricted(got=%d)=%lu integrity=0x%lX session=%lu",
			static_cast<int>(gotRestricted), static_cast<unsigned long>(restricted),
			static_cast<unsigned long>(integrityRid), static_cast<unsigned long>(sessionId));
	}
	else {
		logger->LogError2(L"进程令牌打开失败 err=%lu", static_cast<unsigned long>(GetLastError()));
	}

	// ---- 3) 目标清单的权限要求 ----
	// requireAdministrator / uiAccess 决定 CreateProcess 是否要提升或重建令牌。
	HMODULE module = LoadLibraryExW(targetPath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!module) {
		logger->LogError2(L"目标清单：无法装载文件 err=%lu", static_cast<unsigned long>(GetLastError()));
		return;
	}
	HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
	if (!resource) {
		logger->LogError2(L"目标清单：没有 RT_MANIFEST(24) 资源");
		FreeLibrary(module);
		return;
	}
	const DWORD size = SizeofResource(module, resource);
	HGLOBAL loaded = LoadResource(module, resource);
	const char* raw = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
	if (!raw || size == 0) {
		logger->LogError2(L"目标清单：资源为空");
		FreeLibrary(module);
		return;
	}
	const std::string manifest(raw, size);
	const bool hasUiAccess = manifest.find("uiAccess=\"true\"") != std::string::npos;
	std::string level = "(未声明)";
	const size_t levelPos = manifest.find("requestedExecutionLevel");
	if (levelPos != std::string::npos) {
		const size_t attrPos = manifest.find("level=\"", levelPos);
		if (attrPos != std::string::npos) {
			const size_t begin = attrPos + 7;
			const size_t end = manifest.find('"', begin);
			if (end != std::string::npos) level.assign(manifest, begin, end - begin);
		}
	}
	// 显式转宽再输出，不依赖 %hs 的格式化实现差异。
	wchar_t levelWide[64] = {};
	MultiByteToWideChar(CP_UTF8, 0, level.c_str(), -1, levelWide, _countof(levelWide));
	logger->LogError2(L"目标清单 requestedExecutionLevel=%s uiAccess=%d",
		levelWide, hasUiAccess ? 1 : 0);
	FreeLibrary(module);
}

// 释放内嵌更新器、让它以 --manifest 取一次清单、再把结果读回来。
//
// 为什么绕这一圈：主程序是 GUI 进程，实测在里面调 WinINet 会直接
// ERROR_INTERNET_CANNOT_CONNECT（同一个账号、同样的调用，控制台进程却正常）。
// 更新器是控制台进程，让它代取即可绕开这个问题。
UPEXPORT_CFUNC(BOOL) JUpdater_ProbeManifest(JUpdaterManifestInfo* info)
{
	if (!info) return FALSE;
	ZeroMemory(info, sizeof(*info));

	const LPCWSTR currentDir = JTAppGetCurrentDirDirect();
	Logger* logger = JTAppGetLoggerDirect();

	std::wstring updaterPath;
	if (!ExtractBundledUpdater(currentDir, updaterPath)) {
		if (logger) logger->LogError(L"释放独立更新程序失败（探测清单）");
		return FALSE;
	}

	WCHAR tempDir[MAX_PATH] = {};
	if (!GetTempPathW(_countof(tempDir), tempDir)) return FALSE;
	WCHAR tempFile[MAX_PATH] = {};
	swprintf_s(tempFile, L"%sDzjsTrainer-manifest-%lu.txt", tempDir,
		static_cast<unsigned long>(GetCurrentProcessId()));
	DeleteFileW(tempFile);

	WCHAR args[1024] = {};
	// --no-elevate：这个模式只读远端、不碰任何文件，不该弹 UAC。
	swprintf_s(args, _countof(args), L"--manifest \"%s\" --no-elevate", tempFile);

	std::wstring command = L"\"" + updaterPath + L"\" " + args;
	std::vector<WCHAR> commandLine(command.begin(), command.end());
	commandLine.push_back(L'\0');

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	BOOL ok = FALSE;

	// 线程令牌处于模拟态、且模拟级别 ≤ Identification 时，CreateProcess 会失败并返回
	// ERROR_BAD_IMPERSONATION_LEVEL(1346)（2026-09-19 实测确认）。这里先主动撤掉模拟；
	// 未在模拟时下面两种写法都无害。
	//
	// 注意：**不要用 OpenThreadToken 的结果去判断"是否在模拟"** —— 实测 OpenAsSelf=FALSE
	// 在 Identification 级别下会失败并返回 1346，传 TRUE 才能打开（见 LogThreadTokenState）。
	LogThreadTokenState(logger, L"调用前");

	HANDLE savedToken = nullptr;
	// OpenAsSelf 用 TRUE：实测在 Identification 级别下 TRUE 能打开、FALSE 会失败(1346)。
	// 这里只为把原令牌存下来以便恢复，用可靠的那个。
	OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &savedToken);
	// 两种"停止模拟"的写法都上：RevertToSelf 未模拟时无害；
	// SetThreadToken(NULL, NULL) 是另一种等价写法，防止某种模拟来源 RevertToSelf 撤不掉。
	const BOOL reverted = RevertToSelf();
	const BOOL cleared = SetThreadToken(nullptr, nullptr);
	if (logger) {
		logger->LogError2(L"撤销模拟：RevertToSelf=%d SetThreadToken(NULL,NULL)=%d",
			static_cast<int>(reverted), static_cast<int>(cleared));
	}
	LogThreadTokenState(logger, L"撤销后");

	if (CreateProcessW(updaterPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		WaitForSingleObject(process.hProcess, 30000);
		DWORD exitCode = 1;
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		ok = exitCode == 0;
		if (!ok && logger) logger->LogError2(L"更新器取清单失败：exit=%lu", exitCode);
	} else if (logger) {
		// 先把错误码存下来：后面几次调用会覆盖 GetLastError。
		const DWORD launchError = GetLastError();
		const DWORD fileAttributes = GetFileAttributesW(updaterPath.c_str());
		logger->LogError2(L"启动更新器失败（探测清单）：%lu 路径=%s attr=0x%08lX",
			static_cast<unsigned long>(launchError), updaterPath.c_str(),
			static_cast<unsigned long>(fileAttributes));
		LogThreadTokenState(logger, L"失败时");
		LogLaunchEnvironment(logger, updaterPath.c_str());
		HANDLE processToken = nullptr;
		DWORD processType = 0;
		DWORD processLevel = 0;
		DWORD returned = 0;
		const BOOL processOpened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken);
		if (processOpened) {
			GetTokenInformation(processToken, TokenType, &processType, sizeof(processType), &returned);
			GetTokenInformation(processToken, TokenImpersonationLevel, &processLevel,
				sizeof(processLevel), &returned);
			CloseHandle(processToken);
		}
		logger->LogError2(L"进程令牌 opened=%d type=%d level=%d",
			static_cast<int>(processOpened), static_cast<int>(processType),
			static_cast<int>(processLevel));
		// UIAccess 进程创建子进程有额外约束（新进程需要同样具备 UIAccess 资格），
		// 把这两个标志也记下来 —— 主程序点过"启用超级置顶"后就是 UIAccess 进程。
		HANDLE infoToken = nullptr;
		DWORD uiAccessFlag = 0;
		DWORD elevatedFlag = 0;
		if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &infoToken)) {
			GetTokenInformation(infoToken, TokenUIAccess, &uiAccessFlag, sizeof(uiAccessFlag), &returned);
			GetTokenInformation(infoToken, TokenElevation, &elevatedFlag, sizeof(elevatedFlag), &returned);
			CloseHandle(infoToken);
		}
		logger->LogError2(L"进程属性 UIAccess=%lu Elevated=%lu",
			static_cast<unsigned long>(uiAccessFlag), static_cast<unsigned long>(elevatedFlag));

		// 回退：CreateProcess 受调用线程令牌状态约束，改走 shell 代发 —— 由 shell
		// 进程创建目标，不再受本线程令牌影响。成功/失败都记一条，便于判断。
		SHELLEXECUTEINFOW shell = {};
		shell.cbSize = sizeof(shell);
		shell.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
		shell.lpFile = updaterPath.c_str();
		shell.lpParameters = args;
		shell.nShow = SW_HIDE;
		if (ShellExecuteExW(&shell) && shell.hProcess) {
			WaitForSingleObject(shell.hProcess, 30000);
			DWORD shellExit = 1;
			GetExitCodeProcess(shell.hProcess, &shellExit);
			CloseHandle(shell.hProcess);
			ok = shellExit == 0;
			logger->LogError2(L"探测：CreateProcess 失败后改走 ShellExecute，exit=%lu",
				static_cast<unsigned long>(shellExit));
		} else {
			logger->LogError2(L"探测：ShellExecute 回退也失败 err=%lu",
				static_cast<unsigned long>(GetLastError()));
		}
	}

	// 恢复原来的模拟状态，别影响调用方后续操作。
	if (savedToken) {
		SetThreadToken(nullptr, savedToken);
		CloseHandle(savedToken);
	}

	if (ok) {
		HANDLE file = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			ok = FALSE;
		} else {
			std::string text;
			char buffer[4096];
			DWORD read = 0;
			while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read)
				text.append(buffer, read);
			CloseHandle(file);
			ParseManifestText(text, info);
			ok = info->version[0] != L'\0' && info->url[0] != L'\0';
		}
	}

	DeleteFileW(tempFile);
	return ok ? TRUE : FALSE;
}

static bool WriteUpdaterTo(const std::wstring& directory, const BYTE* data, DWORD size, std::wstring& updaterPath)
{
	if (directory.empty()) return false;
	CreateDirectoryW(directory.c_str(), nullptr);
	updaterPath = directory + L"\\DzjsTrainerUpdater.exe";
	HANDLE file = CreateFileW(updaterPath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
	FlushFileBuffers(file);
	CloseHandle(file);
	if (!ok) DeleteFileW(updaterPath.c_str());
	return ok;
}

// The updater is written next to the main program so the user can see it, re-run
// it, and so it never runs elevated out of a user-writable temp directory. A
// read-only install directory falls back to the per-user temp directory.
static bool ExtractBundledUpdater(LPCWSTR currentDir, std::wstring& updaterPath)
{
	HINSTANCE module = GetModuleHandleW(nullptr);
	HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_UPDATER_EXE), L"BIN");
	if (!resource) return false;
	HGLOBAL loaded = LoadResource(module, resource);
	const BYTE* data = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
	const DWORD size = loaded ? SizeofResource(module, resource) : 0;
	if (!data || size < sizeof(IMAGE_DOS_HEADER)) return false;

	if (currentDir && currentDir[0] != L'\0' && WriteUpdaterTo(currentDir, data, size, updaterPath))
		return true;

	WCHAR tempRoot[MAX_PATH] = {};
	if (!GetTempPathW(_countof(tempRoot), tempRoot)) return false;
	return WriteUpdaterTo(std::wstring(tempRoot) + L"DzjsTrainerUpdater", data, size, updaterPath);
}

// 借子进程下载：主程序进程里 WinHTTP 的 TLS 会失败，这里的子进程是干净进程。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchDownload(LPCWSTR url, LPCWSTR destPath,
	LPCWSTR sha256Hex, DWORD timeoutMs, HWND notifyWindow)
{
	if (!url || url[0] == L'\0' || !destPath || destPath[0] == L'\0') return FALSE;
	const LPCWSTR currentDir = JTAppGetCurrentDirDirect();
	Logger* logger = JTAppGetLoggerDirect();

	std::wstring updaterPath;
	if (!ExtractBundledUpdater(currentDir, updaterPath)) {
		if (logger) logger->LogError(L"释放独立更新程序失败（下载模式）");
		return FALSE;
	}

	// 命令行里要带引号，但本文件不想掺进转义字符，用字符码拼。
	// 第一段必须是更新器自己的路径：CreateProcess 的 lpCommandLine 会被子进程
	// 当成完整命令行，它把第一个 token 当 argv[0]。少了这一段，子进程的
	// "--download" 就变成了程序名，参数整体左移，最终掉进交互式安装流程卡死。
	const WCHAR quote = static_cast<WCHAR>(34);
	std::wstring command;
	command.push_back(quote);
	command += updaterPath;
	command.push_back(quote);
	command += L" --download ";
	command.push_back(quote);
	command += destPath;
	command.push_back(quote);
	command += L" --url ";
	command.push_back(quote);
	command += url;
	command.push_back(quote);
	if (sha256Hex && sha256Hex[0] != L'\0') {
		command += L" --sha256 ";
		command += sha256Hex;
	}
	// 把主窗口句柄交给子进程，它边下边把 (total, done) 发回来。
	// 用十进制就够了，省得再操心十六进制解析。
	if (notifyWindow) {
		wchar_t handleText[32] = {};
		swprintf_s(handleText, L"%llu",
			static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(notifyWindow)));
		command += L" --notify ";
		command += handleText;
	}
	command += L" --no-elevate";

	std::vector<WCHAR> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(static_cast<WCHAR>(0));

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(updaterPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
		CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		if (logger) logger->LogError(L"启动下载进程失败：%s (%d) %s",
			PRINT_LAST_ERROR_STR, updaterPath.c_str());
		return FALSE;
	}
	const DWORD waited = WaitForSingleObject(process.hProcess, timeoutMs ? timeoutMs : 300000);
	DWORD exitCode = 1;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	if (waited != WAIT_OBJECT_0) {
		if (logger) logger->LogError(L"下载进程超时（%lu 毫秒）", static_cast<unsigned long>(timeoutMs));
		return FALSE;
	}
	if (exitCode != 0) {
		// 子进程是静默启动的（CREATE_NO_WINDOW），它的输出用户看不见，
		// 所以把退出码翻成人话落进日志 —— 否则"更新失败"四个字等于没线索。
		const wchar_t* reason = L"下载进程异常退出";
		switch (exitCode) {
		case 5: reason = L"下载失败或下载地址不被信任"; break;
		case 6: reason = L"更新包 SHA-256 与清单不符"; break;
		case 11: reason = L"缺少更新包校验值，已拒绝下载"; break;
		case 12: reason = L"服务器身份校验未通过，已拒绝下载"; break;
		default: break;
		}
		if (logger) logger->LogError(L"下载进程退出码 %lu（%s）",
			static_cast<unsigned long>(exitCode), reason);
		return FALSE;
	}
	return GetFileAttributesW(destPath) != INVALID_FILE_ATTRIBUTES ? TRUE : FALSE;
}

// 只替换模式：给出已下载好的包，让更新器等主程序退出后换上它。
UPEXPORT_CFUNC(BOOL) JUpdater_LaunchApply(LPCWSTR packagePath, LPCWSTR sha256Hex)
{
	if (!packagePath || packagePath[0] == L'\0') return FALSE;
	const LPCWSTR currentDir = JTAppGetCurrentDirDirect();
	Logger* logger = JTAppGetLoggerDirect();

	std::wstring updaterPath;
	if (!ExtractBundledUpdater(currentDir, updaterPath)) {
		if (logger) logger->LogError(L"释放独立更新程序失败（替换模式）");
		return FALSE;
	}

	WCHAR targetPath[MAX_PATH] = {};
	if (currentDir && currentDir[0] != L'\0') {
		wcsncpy_s(targetPath, currentDir, _TRUNCATE);
		wcsncat_s(targetPath, L"\\DzjsTrainer.exe", _TRUNCATE);
	}

	WCHAR args[2048] = {};
	if (sha256Hex && sha256Hex[0] != L'\0') {
		swprintf_s(args, _countof(args), L"--apply \"%s\" --target \"%s\" --sha256 %s",
			packagePath, targetPath, sha256Hex);
	}
	else {
		swprintf_s(args, _countof(args), L"--apply \"%s\" --target \"%s\"",
			packagePath, targetPath);
	}

	if (!SysHlp::RunApplication(updaterPath.c_str(), args)) {
		if (logger) logger->LogError(L"启动替换程序失败：%s (%d) %s",
			PRINT_LAST_ERROR_STR, updaterPath.c_str());
		return FALSE;
	}
	return TRUE;
}

UPEXPORT_CFUNC(BOOL) JUpdater_LaunchUpdater()
{
	// Non-virtual accessors only; see the note at the top of this file.
	const LPCWSTR currentDir = JTAppGetCurrentDirDirect();
	Logger* logger = JTAppGetLoggerDirect();

	std::wstring updaterPath;
	if (!ExtractBundledUpdater(currentDir, updaterPath)) {
		if (logger) logger->LogError(L"释放独立更新程序失败");
		return FALSE;
	}

	WCHAR targetPath[MAX_PATH] = {};
	if (currentDir && currentDir[0] != L'\0') {
		wcsncpy_s(targetPath, currentDir, _TRUNCATE);
		wcsncat_s(targetPath, L"\\DzjsTrainer.exe", _TRUNCATE);
	}

	WCHAR args[2048] = {};
	const LPCWSTR sourcePath = JTAppGetSourceInstallerPathDirect();
	if (targetPath[0] != L'\0' && sourcePath && sourcePath[0] != L'\0')
		swprintf_s(args, _countof(args), L"--target \"%s\" --source \"%s\"", targetPath, sourcePath);
	else if (targetPath[0] != L'\0')
		swprintf_s(args, _countof(args), L"--target \"%s\"", targetPath);

	// The updater requests elevation itself, so a declined UAC prompt is reported
	// by the updater instead of being swallowed here.
	if (!SysHlp::RunApplication(updaterPath.c_str(), args)) {
		if (logger) logger->LogError(L"启动独立更新程序失败：%s (%d) %s",
			PRINT_LAST_ERROR_STR, updaterPath.c_str());
		return FALSE;
	}
	return TRUE;
}
