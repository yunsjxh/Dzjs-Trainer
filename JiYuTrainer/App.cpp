#include "stdafx.h"
#include "resource.h"
#include "JiYuTrainer.h"
#include "App.h"
#include "PathHelper.h"
#include "MD5Utils.h"
#include "SysHlp.h"
#include "StringHlp.h"
#include "NtHlp.h"
#include "KernelUtils.h"
#include "DriverLoader.h"
#include "JyUdpAttack.h"
#include "GuardTerminator.h"
#include "RepositoryCleanup.h"
#include "RegHlp.h"
#include "TxtUtils.h"
#include "ActiveDefenseDemo.h"
#include "AvIntegrated.h"
#include "../WindowCaptureProtection.h"

#include <Shlwapi.h>
#include <winioctl.h>
#include <CommCtrl.h>
#include <ShellAPI.h>
#include <dbghelp.h>
#include <locale>
#include "../JiYuTrainerUI/MainWindow.h"
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#define CMD_HELP L"\
工具命令：\n\
-install-full 开始完整安装命令\n\
-config       打开 Dzjs Trainer 高级配置\n\
-hidden       静默运行模式\n\
-killst       杀死极域电子教室\n\
\n\
调试命令：\n\
-bugreport -bugfile [bugFilePath]\n\
-break\n\
-crash-test\n\
-force-md5-check\n\
\n\
程序内部命令：\n\
-f [sourceFilePath]\n\
-r[1|2|3]\n\
-ic\n\
-rc\n\
-b\n\
-h\n\
"

extern LoggerInternal * currentLogger;
extern JTApp * currentApp;

extern NtCloseFun NtClose;

extern "C" HINSTANCE JTAppGetInstanceDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetInstance() : GetModuleHandleW(nullptr);
}

extern "C" int JTAppGetShowCmdDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetAppShowCmd() : SW_SHOWDEFAULT;
}

extern "C" Logger* JTAppGetLoggerDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetLogger() : nullptr;
}

extern "C" SettingHlp* JTAppGetSettingsDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetSettings() : nullptr;
}

extern "C" TrainerWorker* JTAppGetTrainerWorkerDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetTrainerWorker() : nullptr;
}

extern "C" LPCWSTR JTAppGetAvDriverPathDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::GetPartFullPath(PART_AV_DRIVER) : nullptr;
}

extern "C" LPVOID JTAppRunOperationDirect(AppOperation op)
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app ? app->JTAppInternal::RunOperation(op) : nullptr;
}

extern "C" BOOL JTAppRequestRepositoryCleanupDirect()
{
	auto app = static_cast<JTAppInternal*>(currentApp);
	return app && app->RequestRepositoryCleanup() ? TRUE : FALSE;
}

namespace {
bool ResourceMatchesFile(HINSTANCE module, LPWSTR resourceId, LPCWSTR resourceType, LPCWSTR filePath)
{
	HRSRC resource = FindResourceW(module, resourceId, resourceType);
	if (!resource) return false;

	HGLOBAL loadedResource = LoadResource(module, resource);
	if (!loadedResource) return false;

	const BYTE* resourceData = static_cast<const BYTE*>(LockResource(loadedResource));
	const DWORD resourceSize = SizeofResource(module, resource);
	if (!resourceData || !resourceSize) return false;

	HANDLE file = CreateFileW(
		filePath,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (file == INVALID_HANDLE_VALUE) return false;

	LARGE_INTEGER fileSize = {};
	bool matches = GetFileSizeEx(file, &fileSize) &&
		fileSize.QuadPart == static_cast<LONGLONG>(resourceSize);
	BYTE buffer[64 * 1024];
	DWORD offset = 0;
	while (matches && offset < resourceSize) {
		const DWORD remaining = resourceSize - offset;
		const DWORD chunkSize = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		DWORD bytesRead = 0;
		matches = ReadFile(file, buffer, chunkSize, &bytesRead, NULL) &&
			bytesRead == chunkSize &&
			memcmp(buffer, resourceData + offset, chunkSize) == 0;
		offset += chunkSize;
	}

	CloseHandle(file);
	return matches;
}

const wchar_t kRandomDriverNameAlphabet[] = L"abcdefghjkmnpqrstuvwxyz23456789";

// 以本地日期生成当天稳定的驱动基名。当天重启会复用同一个服务，跨天
// 才会切换名称；名称不写入配置文件，旧服务由 DriverLoader 清理。
std::wstring GenerateDateBasedDriverName()
{
	SYSTEMTIME localTime = {};
	GetLocalTime(&localTime);
	ULONG state = 2166136261UL;
	const WORD dateParts[] = { localTime.wYear, localTime.wMonth, localTime.wDay };
	for (WORD part : dateParts) {
		state ^= static_cast<ULONG>(part & 0xFFU);
		state *= 16777619UL;
		state ^= static_cast<ULONG>((part >> 8) & 0xFFU);
		state *= 16777619UL;
	}
	state ^= 0x4A545244UL;
	state *= 16777619UL;

	wchar_t generated[9] = {};
	for (size_t index = 0; index < 8; ++index) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		generated[index] = kRandomDriverNameAlphabet[
			state % (_countof(kRandomDriverNameAlphabet) - 1)];
	}
	return std::wstring(generated, 8);
}

// Service identity uses a separate date mixer and is intentionally not
// derived from the image filename.
std::wstring GenerateDateBasedServiceName()
{
	SYSTEMTIME localTime = {};
	GetLocalTime(&localTime);
	unsigned long long state =
		(static_cast<unsigned long long>(localTime.wYear) << 32) |
		(static_cast<unsigned long long>(localTime.wMonth) << 16) |
		static_cast<unsigned long long>(localTime.wDay);
	state ^= 0xD6E8FEB86659FD93ULL;
	for (size_t index = 0; index < 4; ++index) {
		state ^= state >> 30;
		state *= 0xBF58476D1CE4E5B9ULL;
		state ^= state >> 27;
		state *= 0x94D049BB133111EBULL;
		state ^= state >> 31;
	}

	wchar_t generated[11] = {};
	for (size_t index = 0; index < 10; ++index) {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		state *= 0x2545F4914F6CDD1DULL;
		generated[index] = kRandomDriverNameAlphabet[
			(state >> 32) % (_countof(kRandomDriverNameAlphabet) - 1)];
	}
	return std::wstring(generated, 10);
}

// ---------------------------------------------------------------------------
// AV 内核驱动命名：与主驱动不同的算法（防止同一命名特征同时命中两个驱动）。
// 文件名：Jenkins one-at-a-time 混合日期 + splitmix32 展开，9 字符纯字母。
// 服务名：wang hash 混合日期（打包顺序与主驱动不同）+ xorshift64* 展开，
// 11 字符全字母数字。四种名字长度各异（8/9/10/11），天然互不冲突。
// 同样按本地日期稳定生成、跨天切换，遗留服务由 DriverLoader 清扫回收。
// ---------------------------------------------------------------------------
const wchar_t kAvFileNameAlphabet[] = L"abcdefghjkmnpqrstuvwxyz";
const wchar_t kAvServiceNameAlphabet[] = L"abcdefghijklmnopqrstuvwxyz0123456789";

std::wstring GenerateDateBasedAvFileName()
{
	SYSTEMTIME localTime = {};
	GetLocalTime(&localTime);
	ULONG hash = 0x9E3779B9UL;
	const BYTE dateBytes[] = {
		static_cast<BYTE>(localTime.wYear & 0xFFU),
		static_cast<BYTE>((localTime.wYear >> 8) & 0xFFU),
		static_cast<BYTE>(localTime.wMonth & 0xFFU),
		static_cast<BYTE>(localTime.wDay & 0xFFU),
		0x41, 0x56
	};
	for (BYTE value : dateBytes) {
		hash += value;
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;

	wchar_t generated[10] = {};
	ULONG state = hash;
	for (size_t index = 0; index < 9; ++index) {
		state += 0x9E3779B9UL;
		ULONG mixed = state;
		mixed ^= mixed >> 16;
		mixed *= 0x85EBCA6BUL;
		mixed ^= mixed >> 13;
		mixed *= 0xC2B2AE35UL;
		mixed ^= mixed >> 16;
		generated[index] = kAvFileNameAlphabet[
			mixed % (_countof(kAvFileNameAlphabet) - 1)];
	}
	return std::wstring(generated, 9);
}

std::wstring GenerateDateBasedAvServiceName()
{
	SYSTEMTIME localTime = {};
	GetLocalTime(&localTime);
	ULONG key = (static_cast<ULONG>(localTime.wDay) << 24) |
		(static_cast<ULONG>(localTime.wMonth) << 12) |
		static_cast<ULONG>(localTime.wYear & 0x0FFFU);
	key ^= 0xA5A5F00DUL;
	key = ~key + (key << 15);
	key ^= key >> 12;
	key += key << 2;
	key ^= key >> 4;
	key *= 2057UL;
	key ^= key >> 16;

	wchar_t generated[12] = {};
	unsigned long long state =
		(static_cast<unsigned long long>(key) << 32) ^ 0x6A09E667F3BCC909ULL;
	for (size_t index = 0; index < 11; ++index) {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		generated[index] = kAvServiceNameAlphabet[static_cast<ULONG>(
			(state * 0x2545F4914F6CDD1DULL) >> 32) % (_countof(kAvServiceNameAlphabet) - 1)];
	}
	return std::wstring(generated, 11);
}

std::wstring g_driverServiceName;
std::wstring g_avDriverServiceName;
}

extern "C" LPCWSTR JTAppGetDriverServiceNameDirect()
{
	if (g_driverServiceName.empty())
		g_driverServiceName = GenerateDateBasedServiceName();
	return g_driverServiceName.c_str();
}

extern "C" LPCWSTR JTAppGetAvServiceNameDirect()
{
	if (g_avDriverServiceName.empty())
		g_avDriverServiceName = GenerateDateBasedAvServiceName();
	return g_avDriverServiceName.c_str();
}

JTAppInternal::JTAppInternal(HINSTANCE hInstance)
{
	this->hInstance = hInstance;
	this->_DialogBoxParamW = (fnDialogBoxParamW)GetProcAddress(GetModuleHandle(L"user32.dll"), "DialogBoxParamW");
}
JTAppInternal::~JTAppInternal()
{
	ExitClear();
}

bool JTAppInternal::PrepareAndStartProtectionEarly()
{
	// The updater is the only executable role that must not arm protection.
	// Normal runs deliberately reach this point before command-line parsing,
	// settings, installation checks, workers, and UI initialization.
	if (appIsInstaller)
		return true;
	if (!SysHlp::Is64BitOS())
		return true;

	// Reuse today's service when it is already valid. Otherwise release the
	// randomized main-driver image needed for self-protection. Hooks, workers,
	// and UI remain deferred until both kernel protection services are ready.
	if (!XTryReuseInstalledDriver() &&
		!ResourceMatchesFile(hInstance, MAKEINTRESOURCE(IDR_DLL_DRIVER), L"BIN", fullDriverPath.c_str())) {
		std::wstring stagedDriverPath = fullDriverPath + L".new";
		DeleteFileW(stagedDriverPath.c_str());
		if (InstallResFile(hInstance, MAKEINTRESOURCE(IDR_DLL_DRIVER), L"BIN", stagedDriverPath.c_str()) != ExtractSuccess ||
			!MoveFileExW(stagedDriverPath.c_str(), fullDriverPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			appLogger->LogWarn(L"启动前释放随机内核驱动失败：%s (%d)", fullDriverPath.c_str(), GetLastError());
			DeleteFileW(stagedDriverPath.c_str());
			return false;
		}
		appLogger->LogInfo(L"启动前已释放随机内核驱动：%s", fullDriverPath.c_str());
	}

	LoadDriver();
	if (!XDriverLoaded()) return false;

	// The AV driver used to be staged and loaded after installation, leaving a
	// startup window where only the primary driver was active. Prepare a missing
	// image here and load the AV service before any worker or UI is created.
	if (!ResourceMatchesFile(hInstance, MAKEINTRESOURCE(IDR_AV_DRIVER), L"BIN", fullAvDriverPath.c_str())) {
		if (Path::Exists(fullAvDriverPath)) {
			// A different existing image may still be mapped by a prior service.
			// Leave replacement to CheckAndInstall instead of overwriting it early.
			appLogger->LogWarn(L"启动前 AV 驱动镜像与资源不一致，延后至安装阶段处理：%s", fullAvDriverPath.c_str());
		}
		else {
			std::wstring stagedAvPath = fullAvDriverPath + L".new";
			DeleteFileW(stagedAvPath.c_str());
			if (InstallResFile(hInstance, MAKEINTRESOURCE(IDR_AV_DRIVER), L"BIN", stagedAvPath.c_str()) != ExtractSuccess ||
				!MoveFileExW(stagedAvPath.c_str(), fullAvDriverPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				appLogger->LogWarn(L"启动前释放 AV 内核驱动失败：%s (%d)", fullAvDriverPath.c_str(), GetLastError());
				DeleteFileW(stagedAvPath.c_str());
				return false;
			}
			appLogger->LogInfo(L"启动前已释放 AV 内核驱动：%s", fullAvDriverPath.c_str());
		}
	}

	if (!Path::Exists(fullAvDriverPath)) {
		appLogger->LogWarn(L"启动前未找到 AV 内核驱动镜像：%s", fullAvDriverPath.c_str());
		return false;
	}
	if (!AvIntegratedEnsureLoaded(fullAvDriverPath.c_str())) {
		appLogger->LogWarn(L"启动前 AV 内核保护未建立，稍后安装流程将再次尝试");
		return false;
	}
	appLogger->LogInfo(L"启动前内核保护已建立：主驱动和 AV 驱动均已就绪");
	return true;
}

int JTAppInternal::CheckAndInstall()
{
	//是可移动设备中比如U盘
	if (!appIsInstaller && !appForceIntallInCurrentDir && SysHlp::CheckIsPortabilityDevice(fullDir.c_str()))
	{
		//则复制本体和ini至临时目录，然后使用bat启动（可以不占用u盘，方便弹出）
		//创建临时目录
		WCHAR szTempPath[MAX_PATH];
		GetTempPath(MAX_PATH, szTempPath);
		wcscat_s(szTempPath, L"\\JiYuTrainer");
		if (!Path::Exists(szTempPath) && !CreateDirectory(szTempPath, NULL)) {
			appLogger->LogError2(L"创建临时目录失败：%s (%d)", PRINT_LAST_ERROR_STR);
			return -1;
		}
		WCHAR szTempMainPath[MAX_PATH];
		WCHAR szTempMainStartBatPath[MAX_PATH];
		wcscpy_s(szTempMainPath, szTempPath);
		wcscat_s(szTempMainPath, L"\\DzjsTrainer.exe");
		wcscpy_s(szTempMainStartBatPath, szTempPath);
		wcscat_s(szTempMainStartBatPath, L"\\JiYuTrainerStart.bat");
		//复制本体
		if (!CopyFile(fullPath.c_str(), szTempMainPath, FALSE)) {
			appLogger->LogError2(L"创建主程序失败：%s (%d)", PRINT_LAST_ERROR_STR);
			return -1;
		}

		bool useBatStart = false;

		//写入启动bat
		std::wstring szTempMainStartBatPathwz;
		std::wstring szTempMainStartBatPathct = FormatString(L"start \"\" \"%s\" -f \"%s\"\nexit", szTempMainPath, fullPath.c_str());
		szTempMainStartBatPathwz = szTempMainStartBatPath;
		if (TxtUtils::WriteStringToTxt(szTempMainStartBatPathwz, szTempMainStartBatPathct)) useBatStart = true;

		appLogger->Log(L"Installer finish, start app : %s", szTempMainPath);

		//启动 exe 并转交控制权
		std::wstring runBatContent;
		if(useBatStart)
			runBatContent = FormatString(L"/c start \"\" \"%s\"", szTempMainStartBatPath);
		else 
			runBatContent = FormatString(L"/c start \"\" \"%s\" -f \"%s\"", szTempMainPath, fullPath.c_str());
		appIsInstaller = true;
		if (!SysHlp::RunApplicationPriviledge(L"cmd", runBatContent.c_str())) {
			appLogger->LogError2(L"启动主程序失败：%s (%d)", PRINT_LAST_ERROR_STR);
			return -1;
		}
		return 0;
	}

	// The hook image is embedded in this executable. Keep only the current
	// embedded version on disk because LoadLibrary-based remote injection needs a path.
	if (!ResourceMatchesFile(hInstance, MAKEINTRESOURCE(IDR_DLL_HOOKS), L"BIN", fullHookerPath.c_str())) {
		std::wstring stagedHookPath = fullHookerPath + L".new";
		DeleteFile(stagedHookPath.c_str());
		if (InstallResFile(hInstance, MAKEINTRESOURCE(IDR_DLL_HOOKS), L"BIN", stagedHookPath.c_str()) != EXTRACT_RES::ExtractSuccess ||
			!MoveFileEx(stagedHookPath.c_str(), fullHookerPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			appLogger->LogError2(L"Unable to install embedded JiYuTrainerHooks.dll: %s (%d)", PRINT_LAST_ERROR_STR);
			DeleteFile(stagedHookPath.c_str());
			return -1;
		}
		appLogger->LogInfo(L"已释放内嵌 JiYuTrainerHooks.dll");
	}

	// Reuse an installed driver after validating its protocol. Extract only when
	// the service is missing, cannot start, or fails compatibility validation.
	if (SysHlp::Is64BitOS() && !XTryReuseInstalledDriver()) {
		std::wstring stagedDriverPath = fullDriverPath + L".new";
		DeleteFile(stagedDriverPath.c_str());
		if (InstallResFile(hInstance, MAKEINTRESOURCE(IDR_DLL_DRIVER), L"BIN", stagedDriverPath.c_str()) != EXTRACT_RES::ExtractSuccess ||
			!MoveFileEx(stagedDriverPath.c_str(), fullDriverPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			appLogger->LogError2(L"Unable to replace kernel driver %s: %s (%d)", fullDriverPath.c_str(), PRINT_LAST_ERROR_STR);
			DeleteFile(stagedDriverPath.c_str());
			return -1;
		}
		appLogger->LogInfo(L"已从 EXE 资源同步内嵌内核驱动: %s", fullDriverPath.c_str());
	}
	// 清理旧版固定文件名驱动的残留文件（对应服务已由 XTryReuseInstalledDriver 清扫）。
	{
		std::wstring legacyDriverPath = fullDir + L"\\JiYuTrainerDriver.sys";
		if (_wcsicmp(legacyDriverPath.c_str(), fullDriverPath.c_str()) != 0) {
			if (Path::Exists(legacyDriverPath) && !DeleteFileW(legacyDriverPath.c_str()))
				appLogger->LogWarn(L"旧版驱动文件暂时无法删除（可能仍被遗留服务占用）：%s (%d)", PRINT_LAST_ERROR_STR);
			std::wstring legacyStagedPath = legacyDriverPath + L".new";
			if (Path::Exists(legacyStagedPath)) DeleteFileW(legacyStagedPath.c_str());
		}
	}
	if (SysHlp::Is64BitOS()) {
		const bool avDriverMatches = ResourceMatchesFile(
			hInstance, MAKEINTRESOURCE(IDR_AV_DRIVER), L"BIN", fullAvDriverPath.c_str());
		bool avReplacementAllowed = true;
		if (!avDriverMatches && Path::Exists(fullAvDriverPath)) {
			if (!AvIntegratedPrepareForReplacement()) {
				avReplacementAllowed = false;
				appLogger->LogWarn(
					L"检测到旧版 AV 内核驱动，但旧 AV 服务未能授权卸载；保留现有文件，错误码=%lu",
					GetLastError());
			}
			else {
				// The SCM service and the image are separate objects. The service
				// can be stopped/deleted while CREATE_NEW below still sees the old
				// image, so remove the stale file before extracting the resource.
				for (int attempt = 0; attempt < 20 && Path::Exists(fullAvDriverPath); ++attempt) {
					if (DeleteFileW(fullAvDriverPath.c_str())) break;
					Sleep(100);
				}
				if (Path::Exists(fullAvDriverPath)) {
					avReplacementAllowed = false;
					appLogger->LogWarn(L"旧版 AV 内核驱动已卸载但文件仍被占用，无法替换，错误码=%lu", GetLastError());
				}
			}
		}
		if (avReplacementAllowed &&
			!ResourceMatchesFile(hInstance, MAKEINTRESOURCE(IDR_AV_DRIVER), L"BIN", fullAvDriverPath.c_str()) &&
			InstallResFile(hInstance, MAKEINTRESOURCE(IDR_AV_DRIVER), L"BIN", fullAvDriverPath.c_str()) != EXTRACT_RES::ExtractSuccess) {
			appLogger->LogWarn(L"无法同步内嵌 AV 内核驱动，AV 内核功能未启用");
		}
		// 清理旧版固定文件名 AV 驱动残留（对应服务已由 DriverLoader 清扫）。
		{
			std::wstring legacyAvDriverPath = fullDir + L"\\JiYuAvKernel.sys";
			if (_wcsicmp(legacyAvDriverPath.c_str(), fullAvDriverPath.c_str()) != 0 &&
				Path::Exists(legacyAvDriverPath) &&
				!DeleteFileW(legacyAvDriverPath.c_str()))
				appLogger->LogWarn(L"旧版 AV 驱动文件暂时无法删除（可能仍被遗留服务占用）：%s (%d)", PRINT_LAST_ERROR_STR);
		}
	}

	//更新器
	if (appIsInstaller) {
		//须更新主exe
		std::wstring mainExePath = fullDir + L"\\DzjsTrainer.exe";
		if (Path::Exists(mainExePath) && !DeleteFile(mainExePath.c_str())) {
			appLogger->LogError2(L"无法更新原主exe ：%s (%d)", PRINT_LAST_ERROR_STR);
			return -1;
		}
		//更新来源exe
		if (Path::Exists(fullSourceInstallerPath) && DeleteFile(fullSourceInstallerPath.c_str()) && !CopyFile(fullPath.c_str(), fullSourceInstallerPath.c_str(), TRUE)) 
			appLogger->LogError2(L"无法更新原源 exe ：%s (%d) %s", PRINT_LAST_ERROR_STR, fullSourceInstallerPath.c_str());
		if (CopyFile(fullPath.c_str(), mainExePath.c_str(), TRUE)) {
			//启动已更新完成的主程序，并删除本体
			SysHlp::RunApplicationPriviledge(mainExePath.c_str(), FormatString(L"-rc %s", 300, fullPath.c_str()).c_str());
			return 0;
		}
		else {
			appLogger->LogError2(L"创建主 exe 失败：%s (%d) %s", PRINT_LAST_ERROR_STR, mainExePath.c_str());
			return -1;
		}
	}
	return 0;
}
void JTAppInternal::UnInstall() 
{
	//卸载病毒
	if (appWorker) {
		appWorker->RunOperation(TrainerWorkerOpVirusBoom);
		appWorker->RunOperation(TrainerWorkerOpForceUnLoadVirus);
	}
	//稍后删除本体
	Sleep(1000);

	if (Path::Exists(fullDriverPath)) DeleteFile(fullDriverPath.c_str());
	{
		// 旧版固定文件名驱动残留
		std::wstring legacyDriverPath = fullDir + L"\\JiYuTrainerDriver.sys";
		if (Path::Exists(legacyDriverPath)) DeleteFileW(legacyDriverPath.c_str());
	}
	if (Path::Exists(fullAvDriverPath)) DeleteFile(fullAvDriverPath.c_str());
	{
		// 旧版固定文件名 AV 驱动残留
		std::wstring legacyAvDriverPath = fullDir + L"\\JiYuAvKernel.sys";
		if (Path::Exists(legacyAvDriverPath)) DeleteFileW(legacyAvDriverPath.c_str());
	}
	if (Path::Exists(fullLogPath)) DeleteFile(fullLogPath.c_str());
	if (Path::Exists(fullIniPath)) DeleteFile(fullIniPath.c_str());

	//写入删除本体exe的bat
	std::wstring uninstallBatPath = fullDir + L"\\uninstall-final.bat";
	std::wstring uninstallBatContent = FormatString(L"@echo off\n@ping 127.0.0.1 -n 6 > nul\ndel /F /Q %s\
\ndel /F /Q %s\ndel /F /Q %s\ndel %%%%0", fullPath.c_str(), fullHookerPath.c_str());

	if(TxtUtils::WriteStringToTxt(uninstallBatPath, uninstallBatContent))
		SysHlp::RunApplicationPriviledge(uninstallBatPath.c_str(), NULL);

	TerminateProcess(GetCurrentProcess(), 0);
}

EXTRACT_RES JTAppInternal::InstallResFile(HINSTANCE resModule, LPWSTR resId, LPCWSTR resType, LPCWSTR extractTo)
{
	appLogger->Log(L"安装模块文件：(%d) %s", resId, extractTo);

	EXTRACT_RES result = ExtractUnknow;
	HRSRC hResource = NULL;
	HGLOBAL hGlobal = NULL;
	LPVOID pData = NULL;
	DWORD dwSize = NULL;
	DWORD writed;
	HANDLE hFile = CreateFile(extractTo, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		result = ExtractCreateFileError;
		appLogger->LogError2(L"创建模块文件 失败：%s (%d)  %s", PRINT_LAST_ERROR_STR, extractTo);
		return result;
	}

	hResource = FindResourceW(resModule, resId, resType);
	if (!hResource) {
		result = ExtractReadResError;
		goto RETURN;
	}
	hGlobal = LoadResource(resModule, hResource);
	if (!hGlobal) {
		result = ExtractReadResError;
		goto RETURN;
	}
	pData = LockResource(hGlobal);
	if (!pData)
	{
		result = ExtractReadResError;
		goto RETURN;
	}
	dwSize = SizeofResource(resModule, hResource);
	if (!WriteFile(hFile, pData, dwSize, &writed, NULL)) {
		result = ExtractWriteFileError;
		appLogger->LogError2(L"创建模块文件失败，写入文件错误：%s (%d) %s ", PRINT_LAST_ERROR_STR, extractTo);
		CloseHandle(hFile);
		return result;
	}

	SetFileAttributes(extractTo, FILE_ATTRIBUTE_HIDDEN);
	CloseHandle(hFile);
	result = ExtractSuccess;
	return result;

RETURN:
	appLogger->LogError2(L"创建模块文件失败，资源提取错误：%s (%d) %s ", PRINT_LAST_ERROR_STR, extractTo);
	CloseHandle(hFile);
	return result;
}
bool JTAppInternal::IsCommandExists(LPCWSTR cmd)
{
	return FindArgInCommandLine(appArgList, appArgCount, cmd) >= 0;
}
int JTAppInternal::FindArgInCommandLine(LPWSTR *szArgList, int argCount, const wchar_t * arg) {
	WCHAR argBufferS[32];
	WCHAR argBufferR[32];
	swprintf_s(argBufferS, L"-%s", arg);
	swprintf_s(argBufferR, L"/%s", arg);
	for (int i = 0; i < argCount; i++) {
		if (wcscmp(szArgList[i], argBufferS) == 0 || wcscmp(szArgList[i], argBufferR) == 0)
			return i;
	}
	return -1;
}

LPCWSTR JTAppInternal::MakeFromSourceArg(LPCWSTR arg)
{
	if (!fullSourceInstallerPath.empty()) {
		fullArgBuffer = L"-f " + fullSourceInstallerPath + L" " + arg;
		return fullArgBuffer.c_str();
	}
	return arg;
}

void JTAppInternal::CloseSourceDir() {
	
	if (Path::Exists(fullSourceInstallerDir)) {
		HANDLE hDir = CreateFile(fullSourceInstallerDir.c_str(), GENERIC_READ,
			FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hDir != INVALID_HANDLE_VALUE) {
			NtClose(hDir);
			NtClose(hDir);
		}
	}
}
bool JTAppInternal::CheckAntiVirusSoftware(bool showTip) {

	/*
	WCHAR wstr3601[8];
	WCHAR wstr3602[8];
	WCHAR wstr3603[8];
	WCHAR wstrDb1[8];

	copyStrFromIntArr(wstr3601, str3601, sizeof(str3601) / sizeof(int));
	copyStrFromIntArr(wstr3602, str3602, sizeof(str3602) / sizeof(int));
	copyStrFromIntArr(wstr3603, str3603, sizeof(str3603) / sizeof(int));
	copyStrFromIntArr(wstrDb1, strDb1, sizeof(wstrDb1) / sizeof(int));

	if (MRegCheckUninstallItemExists(wstr3601)
		|| MRegCheckUninstallItemExists(wstr3602)
		|| MRegCheckUninstallItemExists(wstr3603))
		existsAntiVirus += L"360";
	if (MRegCheckUninstallItemExists(wstrDb1))
		existsAntiVirus += L"、金山毒霸";

	if (existsAntiVirus != L"") {
		if (showTip)  _DialogBoxParamW(hInstance, MAKEINTRESOURCE(IDD_DIALOG_AVTIP), NULL, AVTipWndProc, (LPARAM)this);
		return true;
	}
	
	*/
	return false;
}
void JTAppInternal::copyStrFromIntArr(wchar_t * buffer, int * arr, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buffer[i] = (WCHAR)arr[i];
}

int JTAppInternal::Run(int nCmdShow)
{
	this->appShowCmd = nCmdShow;
	appResult = RunInternal();
	// JiYuTrainerUI destroys MainWindow before returning here. Its live-log
	// callback owns that window, so never run exit cleanup through it.
	if (appLogger) appLogger->SetLogOutPut(LogOutPutFile);
	this->Exit(appResult);
	return appResult;
}
int JTAppInternal::RunCheckRunningApp()//如果程序已经有一个在运行，则返回true
{
	wchar_t modulePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
	const bool nativePreviewBuild = wcsstr(modulePath, L"JiYuTrainer-TeacherNative") != nullptr;
	if (!nativePreviewBuild) {
		HWND oldWindow = FindWindow(MAIN_WND_CLS_NAME, MAIN_WND_NAME);
		if (oldWindow != NULL) {
			if (!IsWindowVisible(oldWindow)) ShowWindow(oldWindow, SW_SHOW);
			if (IsIconic(oldWindow)) ShowWindow(oldWindow, SW_RESTORE);
			SetForegroundWindow(oldWindow);
			return -1;
		}
	}
	HANDLE hMutex = CreateMutex(NULL, FALSE, nativePreviewBuild ? L"JYTMutex-TeacherNative" : L"JYTMutex");
	if (hMutex && (GetLastError() == ERROR_ALREADY_EXISTS))
	{
		CloseHandle(hMutex);
		hMutex = NULL;
		return 1;
	}
	return 0;
}
bool JTAppInternal::RunArgeementDialog()
{
	int rs = _DialogBoxParamW(hInstance, MAKEINTRESOURCE(IDD_DIALOG_ARGEEMENT), NULL, ArgeementWndProc, NULL);
	if (rs == IDYES) {
		appSetting->SetSettingBool(L"Argeed", true, L"JTArgeement");
		return true;
	}
	return false;
}

int JTAppInternal::RunInternal()
{
	setlocale(LC_ALL, "chs");

	MLoadNt();
	InitPrivileges();
	InitLogger();
	InitPath();

	//指定日志为文件模式
	appLogger->SetLogOutPut(LogOutPutFile);
	appLogger->SetLogOutPutFile(fullLogPath.c_str());

	// Fast protection path: do not parse arguments, read settings, check a
	// previous instance, install resources, create workers, or create UI before
	// both kernel protection drivers have been attempted.
	const bool earlyProtectionStarted = PrepareAndStartProtectionEarly();
	if (!earlyProtectionStarted)
		appLogger->LogWarn(L"最早期内核保护未完全建立，后续安装流程将再次尝试");

	InitCommandLine();
	InitArgs();
	InitSettings();

	if (SysHlp::GetSystemVersion() == SystemVersionNotSupport) {
		appLogger->LogError2(L"系统版本不支持本软件的运行");
		return APP_FAIL_SYSTEM_NOT_SUPPORT;
	}

	if (!appIsRecover) {
		int oldStatus = RunCheckRunningApp();
		if (oldStatus == 1)
			return APP_FAIL_ALEDAY_RUN;
		if (oldStatus == -1)
			return 0;
	}
	if (CheckAndInstall()) return APP_FAIL_INSTALL;
	if (appIsInstaller) return 0;
	CloseSourceDir();

	// Retry only when the early protection stage was skipped or failed.
	if (!XDriverLoaded())
		LoadDriver();
	if (SysHlp::Is64BitOS() && !AvIntegratedIsLoaded() && Path::Exists(fullAvDriverPath))
		AvIntegratedEnsureLoaded(fullAvDriverPath.c_str());

	appWorker = new TrainerWorkerInternal();
	appJyUdpAttack = new JyUdpAttack();
	if (!ActiveDefenseDemoStart())
		appLogger->LogWarn(L"[DEFENSE-DEMO] audit monitor failed to start");
	guardTerminatorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (guardTerminatorStopEvent) {
		guardTerminatorThread = CreateThread(NULL, 0, [](LPVOID parameter) -> DWORD {
			JTAppInternal* app = static_cast<JTAppInternal*>(parameter);
			try {
				for (;;) {
					GuardTerminator::Execute(app->GetLogger(), app->guardTerminatorStopEvent);
					if (WaitForSingleObject(app->guardTerminatorStopEvent, 15000) == WAIT_OBJECT_0)
						break;
				}
			}
			catch (...) {
				if (app->GetLogger()) app->GetLogger()->LogError(L"Guard terminator worker stopped after an unexpected exception");
			}
			return 0;
		}, this, 0, NULL);
		if (!guardTerminatorThread) {
			CloseHandle(guardTerminatorStopEvent);
			guardTerminatorStopEvent = nullptr;
		}
	}
		repositoryCleanupStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		repositoryCleanupRequestEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	repositoryCleanupFinishedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (repositoryCleanupStopEvent && repositoryCleanupRequestEvent && repositoryCleanupFinishedEvent) {
		repositoryCleanupThread = CreateThread(NULL, 0, [](LPVOID parameter) -> DWORD {
			JTAppInternal* app = static_cast<JTAppInternal*>(parameter);
			try {
				for (;;) {
					HANDLE waits[] = { app->repositoryCleanupStopEvent, app->repositoryCleanupRequestEvent };
					const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
					if (waitResult == WAIT_OBJECT_0) break;
					if (waitResult != WAIT_OBJECT_0 + 1) break;
					ResetEvent(app->repositoryCleanupRequestEvent);
					const bool cleaned = RepositoryCleanup::Execute(app->GetLogger(), app->repositoryCleanupStopEvent);
					InterlockedExchange(&app->repositoryCleanupResult, cleaned ? 1L : 0L);
					SetEvent(app->repositoryCleanupFinishedEvent);
					if (cleaned) {
						MessageBoxW(nullptr, L"退出程序成功！", L"Dzjs Trainer", MB_OK | MB_ICONINFORMATION);
						break;
					}
				}
			}
			catch (...) {
				InterlockedExchange(&app->repositoryCleanupResult, 0L);
				SetEvent(app->repositoryCleanupFinishedEvent);
				if (app->GetLogger()) app->GetLogger()->LogError(L"Repository cleanup worker stopped after an unexpected exception");
			}
			return 0;
		}, this, 0, NULL);
		if (!repositoryCleanupThread) {
			CloseHandle(repositoryCleanupRequestEvent);
			repositoryCleanupRequestEvent = nullptr;
			CloseHandle(repositoryCleanupFinishedEvent);
			repositoryCleanupFinishedEvent = nullptr;
			CloseHandle(repositoryCleanupStopEvent);
			repositoryCleanupStopEvent = nullptr;
		}
	} else {
		if (repositoryCleanupFinishedEvent) CloseHandle(repositoryCleanupFinishedEvent);
		if (repositoryCleanupRequestEvent) CloseHandle(repositoryCleanupRequestEvent);
		if (repositoryCleanupStopEvent) CloseHandle(repositoryCleanupStopEvent);
		repositoryCleanupFinishedEvent = nullptr;
		repositoryCleanupRequestEvent = nullptr;
		repositoryCleanupStopEvent = nullptr;
	}

	EnableVisualStyles();
	if (!CheckAppCorrectness())
		return APP_FAIL_PIRACY_VERSION;

	if (GetAsyncKeyState(VK_LMENU) && 0x8000 || GetAsyncKeyState(VK_RMENU) && 0x8000)
		appIsConfigMode = true;
	if (appArgBreak) {
#ifdef _DEBUG
		if (MessageBox(NULL, L"This is a Debug version", L"Dzjs Trainer - Debug Break", MB_YESNO | MB_ICONEXCLAMATION) == IDYES)
			DebugBreak();
#else
		if (MessageBox(NULL, L"This is a Release version", L"Dzjs Trainer - Debug Break", MB_YESNO | MB_ICONEXCLAMATION) == IDYES)
			DebugBreak();
#endif
	}
	if (appCmdHelpMode) {
		wprintf_s(CMD_HELP);
		wprintf_s(L"\n");
		MessageBox(NULL, CMD_HELP, L"Dzjs Trainer -命令行提示", MB_ICONINFORMATION);
		return 0;
	}
	if (appKillStMode) {
		TrainerWorkerInternal t;
		if (t.KillStAuto()) printf_s("已成功结束极域电子教室\n");
		else printf_s("无法结束极域电子教室，详情请查看日志\n");
		return 0;
	}
	if (!appArgInstallMode) {
		appForceNoDriver = appForceNoDriver || CheckAntiVirusSoftware(appShowAvTest);
		if (appFirstUse) appSetting->SetSettingBool(L"FirstUse", false, L"JTArgeement");
	}
	// Choose the UI mode now, but defer creating every normal/configuration UI
	// window until the protection drivers have been staged and initialized.
	if (appIsBugReportMode) {
		appStartType = AppStartTypeBugReport;
		goto RUN_MAIN;
	}
	if (appIsConfigMode) {
		appStartType = AppStartTypeConfig;
	}
	if (appArgRemoveUpdater) 
	{
		Sleep(1000);//Sleep for a while

		//删除原有更新程序的本体以及日志
		WCHAR updaterLogPath[MAX_PATH];
		wcscpy_s(updaterLogPath, updaterPath.c_str());
		PathRenameExtension(updaterLogPath, L".log");
		if (Path::Exists(updaterLogPath) && !DeleteFileW(updaterLogPath))
			currentLogger->LogError2(L"Remove updater file %s failed : %d", updaterPath.c_str(), GetLastError());
		if (!DeleteFileW(updaterPath.c_str()))
			currentLogger->LogError2(L"Remove updater file %s failed : %d", updaterPath.c_str(), GetLastError());
	}
	if (!appArgInstallMode && !appArgeementArgeed && !RunArgeementDialog())
		return 0;

RUN_MAIN:

	if (appCrashTestMode)return JiYuTrainerUICommonEntry(4);
	if (appStartType == AppStartTypeNormal) return JiYuTrainerUICommonEntry(0);
	else if (appStartType == AppStartTypeUpdater)  return JiYuTrainerUICommonEntry(1);
	else if (appStartType == AppStartTypeConfig) return JiYuTrainerUICommonEntry(2);
	else if (appStartType == AppStartTypeBugReport)  return JiYuTrainerUICommonEntry(3);

	return 0;
}

void JTAppInternal::Exit(int code)
{
	if (appLogger) appLogger->LogInfo(L"Application exit requested: code=%d", code);
	ExitInternal();
	ExitClear();
	if (appLogger) appLogger->LogInfo(L"Application exit cleanup completed: code=%d", code);
	//ExitProcess(code);
}
bool JTAppInternal::RequestRepositoryCleanup()
{
	if (!repositoryCleanupRequestEvent || !repositoryCleanupFinishedEvent) return false;
	ResetEvent(repositoryCleanupFinishedEvent);
	InterlockedExchange(&repositoryCleanupResult, 0L);
	SetEvent(repositoryCleanupRequestEvent);
	const DWORD waitResult = WaitForSingleObject(repositoryCleanupFinishedEvent, 60000);
	if (waitResult != WAIT_OBJECT_0) {
		if (appLogger) appLogger->LogWarn(L"Repository cleanup did not complete within 60 seconds (wait=%lu)", waitResult);
		return false;
	}
	return InterlockedCompareExchange(&repositoryCleanupResult, 0L, 0L) != 0L;
}
bool JTAppInternal::ExitInternal()
{

	return false;
}
void JTAppInternal::ExitClear()
{
	if (appLogger) appLogger->LogInfo(L"ExitClear: begin");
	if (guardTerminatorStopEvent) {
		if (appLogger) appLogger->LogInfo(L"ExitClear: signaling GuardTerminator thread");
		SetEvent(guardTerminatorStopEvent);
	}
	if (repositoryCleanupStopEvent) {
		if (appLogger) appLogger->LogInfo(L"ExitClear: signaling RepositoryCleanup thread");
		SetEvent(repositoryCleanupStopEvent);
	}
	if (repositoryCleanupThread) {
		if (appLogger) appLogger->LogInfo(L"ExitClear: waiting for RepositoryCleanup thread");
		WaitForSingleObject(repositoryCleanupThread, INFINITE);
		CloseHandle(repositoryCleanupThread);
		repositoryCleanupThread = nullptr;
	}
	if (repositoryCleanupRequestEvent) {
		CloseHandle(repositoryCleanupRequestEvent);
		repositoryCleanupRequestEvent = nullptr;
	}
	if (repositoryCleanupFinishedEvent) {
		CloseHandle(repositoryCleanupFinishedEvent);
		repositoryCleanupFinishedEvent = nullptr;
	}
	if (repositoryCleanupStopEvent) {
		CloseHandle(repositoryCleanupStopEvent);
		repositoryCleanupStopEvent = nullptr;
	}
	if (guardTerminatorThread) {
		if (appLogger) appLogger->LogInfo(L"ExitClear: waiting for GuardTerminator thread");
		WaitForSingleObject(guardTerminatorThread, INFINITE);
		if (appLogger) appLogger->LogInfo(L"ExitClear: GuardTerminator thread stopped");
		CloseHandle(guardTerminatorThread);
		guardTerminatorThread = nullptr;
	}
	if (guardTerminatorStopEvent) {
		CloseHandle(guardTerminatorStopEvent);
		guardTerminatorStopEvent = nullptr;
	}
	if (appLogger) appLogger->LogInfo(L"ExitClear: stopping active defense monitor");
	ActiveDefenseDemoStop();
	if (appLogger) appLogger->LogInfo(L"ExitClear: active defense monitor stopped");
	if (XDriverLoaded()) {
		if (appLogger) appLogger->LogInfo(L"ExitClear: closing primary driver handle and child workers");
		XCloseDriverHandle();
		if (appLogger) appLogger->LogInfo(L"ExitClear: primary driver handle closed");
		//XUnLoadDriver();
	}
	if (appLogger) appLogger->LogInfo(L"ExitClear: closing AV driver handle");
	AvIntegratedClose();
	if (appLogger) appLogger->LogInfo(L"ExitClear: AV driver handle closed");
	if (appArgList) {
		LocalFree(appArgList);
		appArgList = nullptr;
	}
	if (appWorker) {
		delete appWorker;
		appWorker = nullptr;
	}
	if (appJyUdpAttack) {
		delete appJyUdpAttack;
		appJyUdpAttack = nullptr;
	}
	if (appSetting) {
		delete appSetting;
		appSetting = nullptr;
	}
	if (appLogger) appLogger->LogInfo(L"ExitClear: completed");
}

LPCWSTR JTAppInternal::GetPartFullPath(int partId)
{
	if (partId == PART_MAIN)
		return fullPath.c_str();
	if (partId == PART_INI) 
		return fullIniPath.c_str();
	if (partId == PART_HOOKER)
		return fullHookerPath.c_str();
	if (partId == PART_DRIVER)
		return fullDriverPath.c_str();
	if (partId == PART_AV_DRIVER)
		return fullAvDriverPath.c_str();
	if (partId == PART_LOG)
		return fullLogPath.c_str();
	return NULL;
}
LPVOID JTAppInternal::RunOperation(AppOperation op)
{
	switch (op)
	{
	case AppOperation1: LoadDriver(); break;
	case AppOperation2: MUnLoadKernelDriver(L"TDProcHook"); break;
	case AppOperationUnLoadDriver: {
		if (XUnLoadDriver())
			currentLogger->Log(L"驱动卸载成功");
		break;
	}
	case AppOperationKReboot:  KFReboot(); break;
	case AppOperationKShutdown: KFShutdown();  break;
	case AppOperationForceLoadDriver: {
		// Refresh startup protection flags before applying the advanced-page changes.
		appForceNoDriver = appSetting->GetSettingBool(L"DisableDriver", false);
		appForceNoSelfProtect = !appSetting->GetSettingBool(L"SelfProtect", true);
		if (appForceNoDriver) XUnLoadDriver();
		else if (XDriverLoaded()) XCloseDriverHandle();
		else LoadDriver();
		break;
	}
	case AppOperation3: {
		char*voidp = (char*)0x65e413f;
		*voidp = '\0';
		break;
	}
	default:
		break;
	}
	return nullptr;
}

void JTAppInternal::LoadDriver()
{
	if (appForceNoDriver) {
		currentLogger->LogInfo(L"已按设置禁用驱动启动");
		return;
	}
	if (!XLoadDriver()) {
		currentLogger->LogWarn(L"启动时加载驱动失败");
		return;
	}
	if (!appForceNoSelfProtect) {
		if (!XInitSelfProtect())
			currentLogger->LogWarn(L"驱动已加载，但启动进程自我保护失败");
		else
			currentLogger->LogInfo(L"启动进程自我保护成功");
	}
}
bool JTAppInternal::CheckAppCorrectness() 
{
	/*
	if(appSetting->GetSettingStr(L"IgnoreCorrectness") == L"20190916")
		return true;

	SYSTEMTIME time;
	HANDLE hFileRead = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	FILETIME file_time;
	FILETIME locationtime;

	GetFileTime(hFileRead, NULL, NULL, &file_time);//获得文件修改时间  
	FileTimeToLocalFileTime(&file_time, &locationtime);//将文件时间转换为本地文件时间  
	FileTimeToSystemTime(&locationtime, &time);

	CloseHandle(hFileRead);

	if (time.wYear > 2019 && time.wMonth > 9 && time.wDay > 16)
		return false;
	*/
	return true;
}

void JTAppInternal::MergePathString()
{
	// 文件名即服务名，DriverLoader 按此派生；名称按本地日期稳定生成。
	const std::wstring driverBaseName = GenerateDateBasedDriverName();
	fullDriverPath = fullDir + L"\\" + driverBaseName + L".sys";
	// AV 驱动同样按日期随机化（算法与主驱动不同）；服务名独立生成，
	// 见 GenerateDateBasedAvServiceName / XGetAvDriverServiceName。
	fullAvDriverPath = fullDir + L"\\" + GenerateDateBasedAvFileName() + L".sys";
	fullHookerPath = fullDir + L"\\JiYuTrainerHooks.dll";
}
void JTAppInternal::InitPath()
{
	WCHAR buffer[MAX_PATH];
	GetModuleFileName(hInstance, buffer, MAX_PATH);
	fullPath = buffer;

	PathRemoveFileSpec(buffer);
	fullDir = buffer;

	GetModuleFileName(hInstance, buffer, MAX_PATH);
	PathRenameExtension(buffer, L".ini");
	fullIniPath = buffer;
	PathRenameExtension(buffer, L".log");
	fullLogPath = buffer;

	appIsInstaller = Path::GetFileName(fullPath) == L"JiYuTrainerUpdater.exe";
	appNeedInstallIniTemple = !Path::Exists(fullIniPath);

	MergePathString();
}
void JTAppInternal::InitCommandLine()
{
	appArgList = CommandLineToArgvW(GetCommandLine(), &appArgCount);
	if (appArgList == NULL)
	{
		MessageBox(NULL, L"Unable to parse command line", L"Error", MB_OK);
		ExitProcess( -1);
	}
}
void JTAppInternal::InitArgs()
{
	if (FindArgInCommandLine(appArgList, appArgCount, L"install-full") != -1) appArgInstallMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"force-md5-check") != -1) appArgForceCheckFileMd5 = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"b") != -1) appArgBreak = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"break") != -1) appArgBreak = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"r1") != -1) appIsRecover = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"h") != -1) appIsHiddenMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"hidden") != -1) appIsHiddenMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"config") != -1) appIsConfigMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"bugreport") != -1) appIsBugReportMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"crash-test") != -1) appCrashTestMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"killst") != -1) appKillStMode = true;
	if (FindArgInCommandLine(appArgList, appArgCount, L"?") != -1) appCmdHelpMode = true;

	int argFIndex = FindArgInCommandLine(appArgList, appArgCount, L"f");
	if (argFIndex >= 0 && (argFIndex + 1) < appArgCount) {
		fullSourceInstallerPath = appArgList[argFIndex + 1];
		if (Path::Exists(fullSourceInstallerPath)) {
			WCHAR buffer[MAX_PATH];
			wcscpy_s(buffer, fullSourceInstallerPath.c_str());
			PathRenameExtension(buffer, L".ini");
			if (Path::Exists(fullSourceInstallerPath))
				fullIniPath = buffer;

			wcscpy_s(buffer, fullSourceInstallerPath.c_str());
			PathRemoveFileSpec(buffer);
			fullSourceInstallerDir = buffer;
		}
	}
	argFIndex = FindArgInCommandLine(appArgList, appArgCount, L"rc");
	if (argFIndex >= 0 && (argFIndex + 1) < appArgCount) {
		LPCWSTR updaterFullPath = appArgList[argFIndex + 1];
		if (Path::Exists(updaterFullPath)) {
			updaterPath = updaterFullPath;
			appArgRemoveUpdater = true;
		}
	}
}
void JTAppInternal::InitLogger()
{
	appLogger = currentLogger;
	appLogger->SetLogOutPut(LogOutPutConsolne);
}
void JTAppInternal::InitPrivileges()
{
	SysHlp::EnableDebugPriv(SE_DEBUG_NAME);
	SysHlp::EnableDebugPriv(SE_SHUTDOWN_NAME);
	SysHlp::EnableDebugPriv(SE_LOAD_DRIVER_NAME);
}
void JTAppInternal::InitSettings()
{
	appSetting = new SettingHlpInternal(fullIniPath.c_str());

	appFirstUse = appSetting->GetSettingBool(L"FirstUse", true, L"JTArgeement");
	appShowAvTest = appSetting->GetSettingBool(L"ShowAvsTest", true);
	appArgeementArgeed = appSetting->GetSettingBool(L"Argeed", false, L"JTArgeement");
	appForceNoDriver = appSetting->GetSettingBool(L"DisableDriver", false);
	appForceNoSelfProtect = !appSetting->GetSettingBool(L"SelfProtect", true);
	appForceIntallInCurrentDir = appSetting->GetSettingBool(L"ForceInstallInCurrentDir", false);
}

void JTAppInternal::EnableVisualStyles() {

	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);
}

HFONT JTAppInternal::hFontRed = NULL;
HINSTANCE JTAppInternal::hInstance = NULL;

INT_PTR CALLBACK JTAppInternal::AVTipWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = 0;

	switch (message)
	{
	case WM_INITDIALOG: {
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_MAIN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_MAIN)));
		SetDlgItemText(hDlg, IDC_MESSAGE, FormatString(L"我们检测到您的计算机上安装了 %s 杀毒软件，因为 Dzjs Trainer 会对极域进行操作，可能会被杀毒软件误识别为病毒。\n因此我们建议您 关闭杀毒软件 或 添加本软件至白名单。", ((JTAppInternal*)lParam)->existsAntiVirus.c_str()).c_str());
		lResult = TRUE;
		break;
	}
	case WM_COMMAND: 
		if(IsDlgButtonChecked(hDlg, IDC_CHECK_DONOT_SHOW_AGAIN) == BST_CHECKED)
			((JTAppInternal*)currentApp)->appSetting->SetSettingBool(L"ShowAvsTest", false);
		EndDialog(hDlg, wParam); 
		lResult = wParam;  
		break;
	default: return DefWindowProc(hDlg, message, wParam, lParam);
	}
	return lResult;
}
INT_PTR CALLBACK JTAppInternal::ArgeementWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = 0;

	switch (message)
	{
	case WM_INITDIALOG: {
		JiYuWindowCapture::ExcludeWindowFromCapture(hDlg);

		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_MAIN)));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_MAIN)));

		hFontRed = CreateFontW(20, 0, 0, 0, 0, FALSE, FALSE, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"宋体");//创建字体
		SendDlgItemMessage(hDlg, IDC_STATIC_RED, WM_SETFONT, (WPARAM)hFontRed, TRUE);//发送设置字体消息

		lResult = TRUE;
		break;
	}
	case WM_COMMAND: EndDialog(hDlg, wParam); lResult = wParam;  break;
	case WM_CTLCOLORSTATIC: {
		if ((HWND)lParam == GetDlgItem(hDlg, IDC_STATIC_RED))  SetTextColor((HDC)wParam, RGB(255, 0, 0));
		return (INT_PTR)GetStockObject(WHITE_BRUSH);
	}
	case WM_CTLCOLORDLG: {
		return (INT_PTR)(HBRUSH)GetStockObject(WHITE_BRUSH);
	}
	case WM_DESTROY: {
		DeleteObject(hFontRed);
		break;
	}
	default: return DefWindowProc(hDlg, message, wParam, lParam);
	}
	return lResult;
}
