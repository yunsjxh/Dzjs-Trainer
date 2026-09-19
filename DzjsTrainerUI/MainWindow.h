#pragma once

#include "stdafx.h"

#define MAIN_WND_CLS_NAME L"DzjsTrainerNativeWindow"
#define MAIN_WND_NAME L"Dzjs Trainer Main Window"

#ifdef DZJSTRAINERUI_EXPORTS

#include <ShellAPI.h>
#include <objidl.h>
#include <gdiplus.h>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../DzjsTrainer/TrainerWorker.h"
#include "../DzjsTrainer/Logger.h"
#include "../DzjsTrainer/AvIntegrated.h"
#include "../DzjsTrainerUpdater/DzjsTrainerUpdater.h"
// 更新探测的上下文。探测必须离开 UI 线程 —— 见 CheckForUpdate 里的说明。
struct UpdateProbeContext {
	HWND window = nullptr;
	bool byUser = false;
	bool ok = false;
	JUpdaterManifestInfo manifest = {};
};

class MainWindow : public TrainerWorkerCallback
{
public:
	MainWindow();
	~MainWindow();

	HWND get_hwnd() const { return _hWnd; }
	bool isValid() const { return _hWnd != nullptr; }
	int RunLoop();
	void Close();

	void OnUpdateStudentMainInfo(bool running, LPCWSTR fullPath, DWORD pid, bool byuser) override;
	void OnUpdateState(TrainerStatus status, LPCWSTR textMain, LPCWSTR textMore) override;
	void OnResolveBlackScreenWindow() override;
	void OnBeforeSendStartConf() override;
	HWND GetMainHWND() override { return _hWnd; }
	void OnSimpleMessageCallback(LPCWSTR text) override;
	void OnAllowGbTop() override;
	void OnShowHelp() override;

private:
	struct TeacherBackend;
	enum class Page { Overview, Protection, Replacement, Antivirus, Advanced, Help, Network, Diagnostics, Logs, About };

	struct LogEntry {
		std::wstring text;
		LogLevel level;
	};

	struct DiagnosticDriver {
		std::wstring name;
		std::wstring path;
		std::wstring rating;
		std::wstring signer;
		std::wstring service;
		std::wstring evidence;
		bool highRisk = false;
		bool signedDriver = false;
		bool needsReview = false;
		bool ownedByJiYu = false;
		bool canUnload = false;
		std::wstring serviceName;
	};

	enum class DiagnosticsFilter { All, HighRisk, Signed, Review };

	HWND _hWnd = nullptr;
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::Bitmap* backBuffer = nullptr;
	int backBufferWidth = 0;
	int backBufferHeight = 0;
	NOTIFYICONDATAW nid{};
	bool trayIconRegistered = false;
	HMENU hMenuTray = nullptr;
	int WM_TASKBARCREATED = 0;
	int hotkeyShowHide = 0;
	int hotkeySwFull = 0;
	HHOOK keyboardInputHook = nullptr;
	HHOOK mouseInputHook = nullptr;
	HANDLE inputHookThread = nullptr;
	HANDLE inputHookReadyEvent = nullptr;
	DWORD inputHookThreadId = 0;
	DWORD inputHookInstallError = ERROR_SUCCESS;

	Logger* currentLogger = nullptr;
	TrainerWorker* currentWorker = nullptr;
	HFONT controlFont = nullptr;
	HBRUSH controlSurfaceBrush = nullptr;
	std::array<HWND, 15> networkControls{};
	HWND teacherHostWindow = nullptr;
	HWND teacherLaunchButton = nullptr;
	HANDLE teacherProcess = nullptr;
	DWORD teacherProcessId = 0;
	HWND teacherChildWindow = nullptr;
	bool teacherLaunchPending = false;
	ULONGLONG teacherLaunchDeadline = 0;
	HWND teacherRefreshButton = nullptr;
	HWND teacherViewButton = nullptr;
	HWND teacherChatInput = nullptr;
	HWND teacherChatButton = nullptr;
	HWND teacherBlackButton = nullptr;
	HWND teacherUnlockButton = nullptr;
	HWND teacherShutdownButton = nullptr;
	HWND teacherRebootButton = nullptr;
	HWND teacherStudentList = nullptr;
	std::unique_ptr<TeacherBackend> teacherBackend;
	std::vector<std::string> teacherDisplayedIps;
	HANDLE avScanThread = nullptr;
	HANDLE avScanCancelEvent = nullptr;
	volatile LONG avScanRunning = 0;
	AV_PROCESS_SCAN_SUMMARY avScanSummary{};
	std::wstring avScanStatus = L"Ready";
	std::wstring avSignaturePath;
	std::wstring avSignaturePattern;
	ULONG avSignatureId = 0;
	DWORD avSignatureSampleCount = 0;
	DWORD avSignatureExactBytes = 0;
	std::array<HWND, 3> advancedControls{};
	int advancedTab = 0;
	int advancedKillMode = 1;
	int advancedInjectMode = 0;
	bool advDisableDriver = false;
	bool advSelfProtect = true;
	bool advAutoForceKill = false;
	bool advStrictWindow = false;
	bool advHideOutput = true;
	bool advHideTaskbar = false;
	bool advController = true;
	bool advAlwaysUpdate = false;
	bool advForceCurrentDir = false;
	bool advDisableWatchdog = false;
	bool advInjectMaster = false;
	bool advInject64 = false;
	mutable std::mutex stateMutex;
	std::vector<LogEntry> logs;

	Page page = Page::Overview;
	int hoverTarget = -1;
	std::array<float, 80> hoverProgress{};
	double pageMotion = 1.0;
	double pulse = 0.0;
	bool darkMode = false;
	bool firstShow = true;
	bool isUserCancel = false;
	bool closeButtonClickPending = false;
	bool mouseTracking = false;
	bool hideTipShowed = false;
	bool setTopMost = false;
	bool topMostUiAccessBand = false;
	bool studentRunning = false;
	bool currentControlled = false;
	DWORD studentPid = 0;
	TrainerStatus currentStatus = TrainerStatusNotFound;
	std::wstring statusTitle = L"Initializing";
	std::wstring statusDetail = L"Connecting to protection service";
	std::wstring studentPath = L"Not detected";
	std::wstring toastText;
	ULONGLONG toastUntil = 0;

	bool setAllowAllRunOp = true;
	bool setAllowGbTop = false;
	bool setAllowMonitor = true;
	bool setAllowControl = false;
	bool setProhibitKillProcess = true;
	bool setProhibitCloseWindow = true;
	bool setBandAllRunOp = false;
	// Information-stream protection is deliberately session-only.  It is reset
	// when the program exits and is never written to the normal settings file.
	bool temporaryVideoProtection = false;
	std::wstring temporaryVideoMode = L"image";
	std::wstring temporaryVideoImagePath;
	std::wstring temporaryVideoFilePath;
	bool temporaryVideoLoop = true;
	std::unique_ptr<Gdiplus::Image> temporaryVideoPreview;
	bool setDoNotShowTrayIcon = false;
	int autoShutSec = 0;

	std::array<RECT, 10> navRects{};
	std::array<RECT, 5> actionRects{};
	std::array<RECT, 9> toggleRects{};
	RECT temporaryVideoEnableRect{};
	RECT temporaryVideoImageModeRect{};
	RECT temporaryVideoFileModeRect{};
	RECT temporaryVideoImagePickerRect{};
	RECT temporaryVideoFilePickerRect{};
	RECT temporaryVideoLoopRect{};
	RECT temporaryVideoPreviewRect{};
	// 一键恢复：同时解除「信息流保护」和「允许教师查看屏幕」两个遮挡来源。
	RECT restoreTeacherViewRect{};
	RECT themeRect{};
	RECT superTopMostRect{};
	RECT exportLogRect{};
	RECT saveSettingsRect{};
	RECT advancedSettingsRect{};
	RECT driverLoadRect{};
	RECT driverUnloadRect{};
	RECT avScanRect{};
	RECT avImportRect{};
	RECT avInputRect{};
	RECT avCopyRect{};
	RECT avUnloadRect{};
	RECT avCancelRect{};
	std::array<RECT, 2> advancedTabRects{};
	std::array<RECT, 12> advancedToggleRects{};
	std::array<RECT, 3> advancedKillRects{};
	std::array<RECT, 2> advancedInjectRects{};
	RECT advancedSaveRect{};
	RECT networkProjectRect{};
	RECT aboutUpdateRect{};
	RECT aboutExitRect{};
	HWND aboutExitButton = nullptr;
	RECT teacherLaunchRect{};
	RECT diagnosticsScanRect{};
	RECT diagnosticsListRect{};
	RECT diagnosticsHideOwnRect{};
	RECT diagnosticsUnloadRect{};
	std::array<RECT, 4> diagnosticsFilterRects{};
	std::array<RECT, 8> diagnosticsDriverRects{};
	std::vector<DiagnosticDriver> diagnosticsDrivers;
	std::vector<size_t> diagnosticsVisibleDrivers;
	std::vector<std::wstring> diagnosticsClassFilters;
	DiagnosticsFilter diagnosticsFilter = DiagnosticsFilter::All;
	int diagnosticsSelectedDriver = -1;
	int diagnosticsListOffset = 0;
	int diagnosticsVisibleRowCount = 0;
	int diagnosticsAdapterTotal = 0;
	int diagnosticsAdapterUp = 0;
	int diagnosticsUsbTotal = 0;
	int diagnosticsUsbIssueCount = 0;
	int diagnosticsProcessCandidates = 0;
	bool diagnosticsRunning = false;
	bool diagnosticsHideOwnDrivers = false;

	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	static LRESULT CALLBACK KeyboardInputProc(int, WPARAM, LPARAM);
	static LRESULT CALLBACK MouseInputProc(int, WPARAM, LPARAM);
	static DWORD WINAPI InputHookThreadProc(LPVOID);
	static DWORD WINAPI AvScanThreadProc(LPVOID);
	static bool IsProtectedInputTarget(HWND, DWORD*);
	static void NotifyInjectedInput(bool, DWORD);
	static MainWindow* FromWindow(HWND hwnd);
	bool RegisterWindowClass();
	bool Initialize();
	void Paint();
	void PaintOverview(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintProtection(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintAntivirus(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintLogs(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintAdvanced(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintHelp(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintNetwork(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintDiagnostics(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintAbout(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintNavigation(Gdiplus::Graphics& g, int height);
	void PaintHeader(Gdiplus::Graphics& g, int width);
	void PaintToast(Gdiplus::Graphics& g, int width);
	void UpdateHover(POINT point);
	void HandleClick(POINT point);
	void CheckForUpdate(bool byUser);
	// 探测结果回到 UI 线程后处理（比对版本、弹确认框、必要时启动更新器）。
	void FinishUpdateProbe(const UpdateProbeContext& probe);
	void RequestSuperTopmost();
	void SelectPage(Page target);
	void StartAvProcessScan();
	void StopAvProcessScan();
	void FinishAvProcessScan();
	void ImportAvExeSignature();
	void InputAvSignature();
	void CopyAvSignature();
	void UnloadAvDriver();
	void LayoutEmbeddedWindows();
	void CreateNetworkControls();
	void LayoutNetworkControls();
	void LaunchEmbeddedTeacherGui();
	void StopEmbeddedTeacherGui();
	void PollEmbeddedTeacherGui();
	bool AttachEmbeddedTeacherGui();
	void LayoutEmbeddedTeacherGui();
	void StartTeacherService();
	void StopTeacherService();
	void PollTeacherService();
	void LayoutTeacherControls();
	void HandleTeacherCommand(int command);
	void CreateAdvancedControls();
	void LayoutAdvancedControls();
	void LoadAdvancedSettings();
	void SaveAdvancedSettings();
	void HandleNetworkCommand(int command);
	void RunDeviceDiagnostics();
	void UnloadSelectedDiagnosticDriver();
	void AppendNetworkResult(LPCWSTR text);
	void ShowPowerMenu();
	void ShowHelp();
	void ExportLogs();
	void ApplyTemporaryVideoProtection();
	bool ChooseTemporaryVideoMedia(bool image);
	void LoadTemporaryVideoPreview();
	void ShowFastTip(LPCWSTR text);
	void LoadSettings();
	void SaveSettings();
	void SaveSettingsOnQuit();
	void OnFirstShow();
	void OnWmCommand(WPARAM wParam);
	void OnWmTimer(WPARAM wParam);
	void OnWmHotKey(WPARAM wParam);
	void OnWmUser(WPARAM wParam, LPARAM lParam);
	void OnWmDestroy();
	bool InstallInputOriginHooks();
	void RemoveInputOriginHooks();
	void CreateTrayIcon();
	void EnsureTrayIcon();
	void ShowTrayBalloon(LPCWSTR title, LPCWSTR text);
	static void LogCallBack(const wchar_t* str, LogLevel level, LPARAM lParam);
	void WriteLogItem(const wchar_t* str, LogLevel level);
};

#endif
