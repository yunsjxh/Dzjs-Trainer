#pragma once

#include "stdafx.h"

#define MAIN_WND_CLS_NAME L"DzjsTrainerNativeWindow"
#define MAIN_WND_NAME L"Dzjs Trainer \u00b7 \u8bbe\u5907\u9632\u62a4\u4e2d\u5fc3"

#ifdef DZJSTRAINERUI_EXPORTS

#include <ShellAPI.h>
#include <objidl.h>
#include <gdiplus.h>
#include <array>
#include <atomic>
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
	// 探测开始时刻（GetTickCount64）。结果日志里要记总耗时 —— 「点了没反应」
	// 这类描述只有配上毫秒数才能分清是慢在进程内握手、子进程启动，还是读文件。
	unsigned long long startedAt = 0;
	// 走的是子进程回退路径。主程序进程里的 TLS 是坏的，正常情况下都是 true。
	bool viaSubprocess = false;
	// 连"有没有网络"这一关都没过，没发出任何请求。
	bool offline = false;
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
	// 顺序即侧栏顺序，也即 Page 的整数值（导航用 static_cast<int>(page) 反查）。
	// 「使用帮助」页已删除，位置由「进程注入」接替 —— Injection 放在第 6 位，
	// 后面的 Network/Diagnostics/Logs/About 序号保持不变，少动就少出错。
	enum class Page { Overview, Protection, Replacement, Antivirus, Advanced, Injection, Network, Diagnostics, Logs, About };

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

	// 「防护策略」页右上角那块驱动状态。
	// XDriverLoaded() 只说明本进程有没有打开设备句柄 —— 驱动被别的实例加载时
	// 它是 FALSE，据此写「没装」会误导。所以顺带问一次 SCM，把「服务在跑但本
	// 程序没连上」和「服务根本没起来」分开显示。
	struct DriverStatusInfo {
		bool handleOpen = false;
		bool serviceExists = false;
		bool serviceRunning = false;
		std::wstring serviceName;
		// 0 表示还没采样过。Paint 在 hover 动画里一秒会被叫几十次，
		// 缓存 1.5 秒，加载/卸载之后调用 InvalidateDriverStatus() 直接作废。
		unsigned long long sampledAt = 0;
	};

	HWND _hWnd = nullptr;
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::Bitmap* backBuffer = nullptr;
	int backBufferWidth = 0;
	int backBufferHeight = 0;
	// 后备缓冲改成 DIB 段：GDI+ 直接画进 DIB，最后用 BitBlt 推到窗口 DC。
	// 实测 BitBlt 0.07 ms vs Graphics::DrawImage 3.20 ms（1120x760）。
	HBITMAP backBufferDib = nullptr;
	HDC backBufferDc = nullptr;
	HGDIOBJ backBufferPreviousBitmap = nullptr;
	void* backBufferBits = nullptr;
	bool EnsureBackBuffer(int width, int height);
	void ReleaseBackBuffer();
	void LogPaintDuration(double milliseconds);
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
	// 高级配置页的输入框底色，必须和自绘的圆角输入框填充一致，
	// 否则原生控件的方块背景会从圆角里露出来。
	HBRUSH advancedFieldBrush = nullptr;
	// 进程选择器的搜索框是原生 EDIT，底色同样要和自绘的圆角框对齐。
	HBRUSH processPickerFieldBrush = nullptr;
	HWND processPickerSearchEdit = nullptr;
	WNDPROC processPickerSearchEditProc = nullptr;
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
	std::wstring avScanStatus = L"\u5c31\u7eea";
	std::wstring avSignaturePath;
	std::wstring avSignaturePattern;
	ULONG avSignatureId = 0;
	DWORD avSignatureSampleCount = 0;
	DWORD avSignatureExactBytes = 0;
	std::array<HWND, 3> advancedControls{};
	// 快捷键框改成自绘：原生 HOTKEY 控件会自己画一圈直角黑边，和圆角卡片冲突。
	// 值沿用 HOTKEY 控件的编码（HIBYTE = 修饰键位，LOBYTE = 虚拟键码），
	// 这样 SysHlp::HotKeyCtlToKeyCode 不用改。
	int hotkeyFakeFullValue = 1606;
	int hotkeyShowHideValue = 1604;
	int hotkeyCaptureIndex = -1;
	std::array<RECT, 2> advancedHotkeyRect{};
	// Custom process injection page
	std::array<RECT, 3> customInjectRects{};
	std::wstring customInjectStatus = L"\u5c31\u7eea";
	std::wstring customInjectList;

	// 进程选择弹窗：和更新弹窗共用一套窗口内自绘模态卡片，不再弹原生对话框。
	// 原生 STATIC/EDIT/BUTTON 是直角灰底 + 系统标题栏，和这套圆角玻璃界面两套皮。
	struct ProcessPickerEntry {
		DWORD pid = 0;
		std::wstring name;
		std::wstring path;
		bool is64 = false;
		bool bitnessKnown = false;   // 打不开进程句柄时位数未知，别硬标成 32 位
		bool self = false;           // 训练器自身：唯一不给注入的目标
		bool critical = false;       // 系统关键进程：只做提示，不拦
		bool injected = false;
	};
	enum class ProcessPickerMode { None, Inject, Unload };
	struct ProcessPickerState {
		ProcessPickerMode mode = ProcessPickerMode::None;
		float appear = 0.0f;
		std::wstring filter;
		std::vector<ProcessPickerEntry> entries;
		std::vector<int> visible;   // 过滤后的下标，指向 entries
		// 多选：与 entries 平行的勾选位。选中状态跟着进程走，不跟着行号走，
		// 所以过滤、滚动、刷新都不会把别人的勾错位。
		std::vector<unsigned char> checked;
		int cursor = -1;            // 焦点行，指向 visible
		int anchor = -1;            // Shift 连选的起点，指向 visible
		int scroll = 0;
		int rowCount = 0;
		int hoverRow = -1;
		std::wstring status;
	};
	ProcessPickerState processPicker;
	RECT processPickerCardRect{};
	RECT processPickerListRect{};
	RECT processPickerSearchRect{};
	RECT processPickerPrimaryRect{};
	RECT processPickerSecondaryRect{};
	RECT processPickerRefreshRect{};
	RECT processPickerSelectAllRect{};
	std::array<RECT, 14> processPickerRowRects{};
	bool ProcessPickerVisible() const { return processPicker.mode != ProcessPickerMode::None; }
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
	int pressTarget = -1;
	// 交互元素比早期多得多（导航 10 + 页面按钮 + 诊断列表行），留够槽位。
	// 原来 80 个槽，restoreTeacherView(81) 和 superTopMost(80) 已经越界。
	std::array<float, 128> hoverProgress{};
	// 开关滑块的插值位置（0=关，1=开）。防护策略 7 个、高级配置 6 个，
	// 两页分槽（0-7 / 8-15），否则翻页时滑块会串味。
	std::array<float, 24> toggleMotion{};
	// 一帧的真实时长，让滑块动画按时间推进而不是按帧数（否则重绘频率一变速度就变）。
	double frameDelta = 1.0 / 60.0;
	ULONGLONG lastPaintTick = 0;
	bool toggleAnimating = false;
	// 键盘焦点。用 Tab 在可点区域之间移动，Enter/Space 触发，Esc 取消。
	int focusTarget = -1;
	enum class ButtonStyle { Primary, Tonal, Ghost, Danger };
	void PaintButton(Gdiplus::Graphics& g, const Gdiplus::RectF& rect, LPCWSTR label,
		int hoverId, ButtonStyle style, float radius = 10.0f, int icon = -1, bool enabled = true);
	double pageMotion = 1.0;
	// 侧栏选中指示器的浮点位置（单位＝条目序号）。翻页时在两条之间滑动，
	// 直接跳到目标位置会让整个界面显得生硬。
	float navIndicator = 0.0f;
	double pulse = 0.0;
	// 动画定时器按需启停：没有动画时不再每 16 ms 醒一次。
	bool animationTimerRunning = false;
	ULONGLONG lastAnimationTick = 0;
	bool darkMode = false;
	// 没手动点过主题按钮就跟随系统的浅色/深色设置；点过之后本次运行内以手动为准。
	bool themeFollowsSystem = true;
	void ApplyTheme(bool dark, bool fromSystem);
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
	// 状态文案在守护线程回报之前就会显示在概览页，默认值不能留英文。
	std::wstring statusTitle = L"\u6b63\u5728\u521d\u59cb\u5316";
	std::wstring statusDetail = L"\u6b63\u5728\u8fde\u63a5\u9632\u62a4\u670d\u52a1";
	std::wstring studentPath = L"\u672a\u68c0\u6d4b\u5230";
	std::wstring toastText;
	ULONGLONG toastUntil = 0;
	ULONGLONG toastStart = 0;

	bool setAllowAllRunOp = true;
	bool setAllowGbTop = false;
	bool setAllowMonitor = true;
	// 2026-11-14 起「默认只记录不拦截」：下面这些"允许"类开关默认放行，
	// "阻止"类开关默认关闭。用户打开某一项，注入的钩子才会真的动手。
	bool setAllowControl = true;
	bool setProhibitKillProcess = false;
	bool setProhibitCloseWindow = false;
	bool setBandAllRunOp = false;
	// 新增的两个拦截项：窗口操作 / 极域驱动访问。
	bool setBlockWindowOps = false;
	bool setBlockDriverAccess = false;
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
	DriverStatusInfo driverStatus;
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
	void PaintNetwork(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintDiagnostics(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintAbout(Gdiplus::Graphics& g, int width, int height, int offsetY);
	void PaintNavigation(Gdiplus::Graphics& g, int height);
	void PaintHeader(Gdiplus::Graphics& g, int width);
	void PaintToast(Gdiplus::Graphics& g, int width, int height);
	void EnsureAnimationTimer();
	int HitTargetAt(POINT point);
	// 焦点目标与 hover id 共用同一套编号，方便复用命中判断。
	bool TargetRect(int target, RECT* rect) const;
	std::vector<int> FocusableTargets() const;
	void MoveFocus(bool backwards);
	bool ActivateFocus();
	float AnimateToggle(int index, bool on);
	// 主色按钮的 hover / 按下态统一走这里，省得每处自己拼颜色。
	void PaintPrimaryButton(Gdiplus::Graphics& g, const Gdiplus::RectF& rect, LPCWSTR label,
		int hoverId, float radius = 20.0f);
	void UpdateHover(POINT point);
	void HandleClick(POINT point);
	void CheckForUpdate(bool byUser);
	// 内置更新：下载 + 校验 + 替换都在本进程里做，走独立线程，进度用消息回报。
	static DWORD WINAPI UpdateDownloadThreadProc(LPVOID parameter);
	void OnUpdateDownloadState(WPARAM wParam, LPARAM lParam);
	// 子进程下载时回传的进度（total 在 wParam、done 在 lParam）。
	void OnChildDownloadProgress(WPARAM wParam, LPARAM lParam);
	// 探测结果回到 UI 线程后处理（比对版本、弹确认框、必要时启动更新器）。
	void FinishUpdateProbe(const UpdateProbeContext& probe);
	// 更新的确认 / 进度 / 失败都用窗口内的自绘卡片，不再弹 MessageBox。
	// 系统消息框是直角白底，和这套圆角界面完全不搭，而且进度条画不上去。
	enum class UpdateDialogMode { None, Confirm, Download, Done, Failed };
	struct UpdateDialogState {
		UpdateDialogMode mode = UpdateDialogMode::None;
		std::wstring url;
		std::wstring version;
		std::wstring currentVersion;
		std::wstring notes;
		std::wstring sha256;
		std::wstring status;
		int percent = -1;
		unsigned long long done = 0;
		unsigned long long total = 0;
		float appear = 0.0f;    // 入场：0→1
		float progress = 0.0f;  // 进度条插值位置（0..1）
		float sweep = 0.0f;     // 总长度未知时的走动相位
	};
	UpdateDialogState updateDialog;
	RECT updateDialogPrimaryRect{};
	RECT updateDialogSecondaryRect{};
	bool UpdateDialogVisible() const { return updateDialog.mode != UpdateDialogMode::None; }
	bool ModalDialogVisible() const { return UpdateDialogVisible() || ProcessPickerVisible(); }
	// 一次探测还没回来时不再起第二次。两次并行探测会各弹一次结果，谁先到不确定；
	// 用户看到的就是「点了没动静，再点一下突然说已是最新版本」。
	std::atomic<bool> updateProbeInFlight{ false };
	void PaintUpdateDialog(Gdiplus::Graphics& g, int width, int height);
	void ShowUpdateConfirm(const UpdateProbeContext& probe, const std::wstring& currentVersion);
	void StartUpdateDownload();
	void CloseUpdateDialog();
	void FailUpdateDialog(const std::wstring& reason);
	bool HandleUpdateDialogClick(POINT point);
	int HitUpdateDialogAt(POINT point) const;
	// 原生子窗口（教师端宿主、输入框）由系统单独绘制，会盖在自绘弹窗上面，
	// 弹窗期间要先把它们藏起来，关掉后再按页面状态恢复。
	void ApplyDialogOcclusion();
	void RequestSuperTopmost();
	void SelectPage(Page target);
	void PaintInjection(Gdiplus::Graphics& g, int width, int height, int offsetY);
	// 自绘进程选择器
	void PaintProcessPicker(Gdiplus::Graphics& g, int width, int height);
	void OpenProcessPicker(bool unload);
	void CloseProcessPicker();
	void RefreshProcessPicker();
	void RebuildProcessPickerVisible();
	int HitProcessPickerAt(POINT point) const;
	bool HandleProcessPickerClick(POINT point);
	bool HandleProcessPickerKey(UINT key, WPARAM wParam);
	void UpdateProcessPickerHover(POINT point);
	void CommitProcessPicker();
	// 搜索框：自绘的那个框接不到输入法，所以叠一个真 EDIT 上去。
	void EnsureProcessPickerSearchEdit();
	void PositionProcessPickerSearchEdit();
	void SyncProcessPickerFilterFromEdit();
	static LRESULT CALLBACK ProcessPickerSearchProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	void StartAvProcessScan();
	void StopAvProcessScan();
	void FinishAvProcessScan();
	void ImportAvExeSignature();
	void InputAvSignature();
	void CopyAvSignature();
	void UnloadAvDriver();
	// 驱动状态框的数据源，带 1.5 秒缓存；点完加载/卸载调 Invalidate 立刻重采。
	const DriverStatusInfo& CurrentDriverStatus();
	void InvalidateDriverStatus() { driverStatus.sampledAt = 0; }
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
	void SetHotkeyValue(int index, int value);
	void HandleHotkeyKey(UINT virtualKey);
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
