// ============================================================================
// CaptureProbe —— 截屏替换通用性靶程序
//
// 它同时扮演两个角色：
//   * 靶子：窗口右侧有一块纯品红靶标，任何截屏路径抓到的画面里都应该有它；
//           底下还有一排视频靶（GDI / D3D11 / 分层 / 系统排除），专门盯动态内容；
//   * 探针：自己用 14 条真实路径去抓屏，把抓回来的像素判成
//     "真画面 / 贴了替换图 / 整幅纯白 / 整幅纯黑 / 被遮挡"。
//
// 用法见同目录 README.md。核心判断只有一句：
//   靶标在 → 这条路径没被拦；靶标没了 → 这条路径已经被换掉了。
// 视频靶加一句：扫描条停在它该在的位置 → 这块内容真的进了这张截图。
// ============================================================================
#include "ImageIo.h"
#include "ProbeAnalyze.h"
#include "ProbeMethods.h"
#include "VideoWall.h"

#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <tlhelp32.h>
#include <vector>
#include <windowsx.h>

namespace {

// ---- 配色 --------------------------------------------------------------
const COLORREF kBg = RGB(0x0E, 0x11, 0x16);
const COLORREF kPanel = RGB(0x15, 0x1A, 0x22);
const COLORREF kPanelDeep = RGB(0x11, 0x15, 0x1C);
const COLORREF kLine = RGB(0x24, 0x2B, 0x36);
const COLORREF kText = RGB(0xE6, 0xEA, 0xF2);
const COLORREF kTextDim = RGB(0x86, 0x91, 0xA4);
const COLORREF kAccent = RGB(0x4A, 0x9E, 0xFF);
const COLORREF kTarget = RGB(0xFF, 0x00, 0xFF);

const UINT_PTR kTimerClock = 1;
const UINT_PTR kTimerBatch = 2;
const UINT_PTR kTimerVideo = 3;

struct Item
{
	ProbeResult probe;
	AnalyzeResult analysis;
	VideoWallReport video;
	wchar_t file[MAX_PATH] = {};
	bool hasFile = false;
};

struct Layout
{
	RECT toolbar;
	RECT list;
	RECT preview;
	RECT videoCaption;
	RECT videoStrip;
	RECT verdict;
	RECT targetCol;
	RECT status;
	bool hasVideo;
	int itemH;
};

struct App
{
	HWND hwnd = nullptr;
	HINSTANCE inst = nullptr;
	int dpi = 96;
	int count = 0;
	int selected = 0;
	int failedCount = 0;
	Item items[32];
	Image32 expect;
	wchar_t expectPath[MAX_PATH] = {};
	wchar_t outDir[MAX_PATH] = {};
	wchar_t moduleName[160] = {};
	bool injected = false;
	bool topmost = false;
	bool batch = false;
	bool quitAfterBatch = false;
	bool running = false;
	bool hideTarget = false;   // 调试：不画品红靶标，用来验证 REPLACED 判定
	wchar_t status[512] = {};
	RECT targetScreen = {};
	RECT btnAll = {};
	RECT btnOne = {};
	HFONT fTitle = nullptr;
	HFONT fBody = nullptr;
	HFONT fSmall = nullptr;
	HFONT fBig = nullptr;
	HFONT fMono = nullptr;
};

App g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

// ---- 小工具 ------------------------------------------------------------
void FillRectColor(HDC dc, const RECT& r, COLORREF c)
{
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

void FrameRectColor(HDC dc, const RECT& r, COLORREF c)
{
	HBRUSH b = CreateSolidBrush(c);
	FrameRect(dc, &r, b);
	DeleteObject(b);
}

void RoundedPanel(HDC dc, const RECT& r, COLORREF fill, COLORREF border)
{
	HBRUSH b = CreateSolidBrush(fill);
	HPEN p = CreatePen(PS_SOLID, 1, border);
	HGDIOBJ ob = SelectObject(dc, b);
	HGDIOBJ op = SelectObject(dc, p);
	RoundRect(dc, r.left, r.top, r.right, r.bottom, S(6), S(6));
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(b);
	DeleteObject(p);
}

void DrawTextIn(HDC dc, const wchar_t* text, RECT r, COLORREF color, HFONT font, UINT flags)
{
	HGDIOBJ old = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);
	DrawTextW(dc, text, -1, &r, flags);
	SelectObject(dc, old);
}

void DrawDot(HDC dc, int cx, int cy, int radius, COLORREF color)
{
	HBRUSH b = CreateSolidBrush(color);
	HPEN p = CreatePen(PS_SOLID, 1, color);
	HGDIOBJ ob = SelectObject(dc, b);
	HGDIOBJ op = SelectObject(dc, p);
	Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(b);
	DeleteObject(p);
}

// ---- 结论文案 ----------------------------------------------------------
const wchar_t* ItemVerdictText(const Item& it)
{
	if (it.probe.selfTest)
	{
		if (!it.probe.captured) return L"FAILED · 自检没跑起来";
		return it.probe.selfTestOk ? L"PASS · 内存 DC 未被改写" : L"误伤 · 内存 DC 被改写";
	}
	return it.analysis.Text();
}

// 视频那一列：没跑 / 没覆盖就留空，别让报告里全是"未覆盖"。
void ItemVideoText(const Item& it, wchar_t* buf, size_t cch)
{
	if (!buf || !cch) return;
	if (it.probe.selfTest || !it.video.valid)
	{
		buf[0] = 0;
		return;
	}
	VideoWallReportText(it.video, buf, cch);
}

const wchar_t* ShortVerdict(const Item& it)
{
	if (it.probe.selfTest)
	{
		if (!it.probe.captured) return L"FAIL";
		return it.probe.selfTestOk ? L"CLEAN" : L"TOUCHED";
	}
	switch (it.analysis.verdict)
	{
	case Verdict::Pass:          return L"PASS";
	case Verdict::ContentOnly:   return L"LOWBPP";
	case Verdict::ReplacedImage: return L"IMG";
	case Verdict::ReplacedOther: return L"REPL";
	case Verdict::SolidWhite:    return L"WHITE";
	case Verdict::SolidBlack:    return L"BLACK";
	case Verdict::Blocked:       return L"BLOCK";
	case Verdict::Failed:        return L"FAIL";
	default:                     return L"—";
	}
}

COLORREF ItemColor(const Item& it)
{
	if (it.probe.selfTest)
	{
		if (!it.probe.captured) return RGB(0x8A, 0x94, 0xA6);
		return it.probe.selfTestOk ? RGB(0x3D, 0xDC, 0x84) : RGB(0xFF, 0x5C, 0x5C);
	}
	return it.analysis.Color();
}

bool HasResult(const Item& it)
{
	return it.probe.captured || it.analysis.verdict != Verdict::NotRun;
}

// ---- 布局 --------------------------------------------------------------
Layout ComputeLayout(int cw, int ch)
{
	const int pad = S(16);
	const int toolbarH = S(64);
	const int statusH = S(38);
	const int listW = S(470);
	const int targetW = S(156);

	Layout l = {};
	l.toolbar = { 0, 0, cw, toolbarH };
	l.status = { 0, ch - statusH, cw, ch };
	l.list = { pad, toolbarH, pad + listW, ch - statusH };
	l.targetCol = { cw - pad - targetW, toolbarH, cw - pad, ch - statusH };

	const int previewX = l.list.right + pad;
	int previewW = l.targetCol.left - pad - previewX;
	if (previewW < S(120)) previewW = S(120);

	const int bodyH = (ch - toolbarH - statusH);
	const int gap = S(8);
	const int capH = S(18);
	const int verdictMin = S(146);

	// 视频条带是固定像素尺寸（四块 160×90），放不下就整条不显示 ——
	// 挤窄了硬塞会把预览和结论区一起压没。
	l.hasVideo = (previewW >= VideoWallStripWidth()) && (bodyH >= S(360));
	const int reserved = l.hasVideo ? (capH + VideoWallStripHeight() + gap * 2) : 0;

	int previewH = (bodyH - reserved) * 62 / 100;
	if (previewH < S(120)) previewH = S(120);
	const int previewMax = bodyH - reserved - verdictMin;
	if (previewH > previewMax) previewH = previewMax;
	if (previewH < S(90)) previewH = S(90);

	l.preview = { previewX, toolbarH, previewX + previewW, toolbarH + previewH };

	int y = l.preview.bottom + gap;
	if (l.hasVideo)
	{
		l.videoCaption = { previewX, y, previewX + previewW, y + capH };
		l.videoStrip = { previewX, y + capH, previewX + VideoWallStripWidth(), y + capH + VideoWallStripHeight() };
		y = l.videoStrip.bottom + gap;
	}
	l.verdict = { previewX, y, previewX + previewW, ch - statusH };
	l.itemH = S(36);
	return l;
}

RECT TargetClientRect(const Layout& l)
{
	const int size = S(132);
	RECT r;
	r.left = l.targetCol.left + ((l.targetCol.right - l.targetCol.left) - size) / 2;
	r.top = l.targetCol.top + S(44);
	r.right = r.left + size;
	r.bottom = r.top + size;
	return r;
}

// ---- 图像绘制 ----------------------------------------------------------
void DrawImageFit(HDC dc, const Image32& img, const RECT& box)
{
	if (!img.Valid()) return;
	const int bw = box.right - box.left;
	const int bh = box.bottom - box.top;
	if (bw <= 0 || bh <= 0) return;

	const double sx = (double)bw / (double)img.width;
	const double sy = (double)bh / (double)img.height;
	const double scale = sx < sy ? sx : sy;
	int w = (int)(img.width * scale);
	int h = (int)(img.height * scale);
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	const int x = box.left + (bw - w) / 2;
	const int y = box.top + (bh - h) / 2;

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = img.width;
	bi.bmiHeader.biHeight = -img.height;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	const int oldMode = SetStretchBltMode(dc, HALFTONE);
	SetBrushOrgEx(dc, 0, 0, nullptr);
	StretchDIBits(dc, x, y, w, h, 0, 0, img.width, img.height,
		img.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
	SetStretchBltMode(dc, oldMode);
}

void PaintTarget(HDC dc, const RECT& r)
{
	// --no-target 是给判定链路做负样本用的：靶标画成背景色，抓到的画面
	// 就"看不到靶标"，用来验证 REPLACED 分支和指纹比对是不是真的在工作。
	if (g.hideTarget)
	{
		FillRectColor(dc, r, kBg);
		return;
	}

	FillRectColor(dc, r, kTarget);
	FrameRectColor(dc, r, RGB(0xFF, 0xFF, 0xFF));

	const int cx = (r.left + r.right) / 2;
	const int cy = (r.top + r.bottom) / 2;
	const int thick = S(4);

	RECT hbar = { r.left + S(6), cy - thick / 2, r.right - S(6), cy - thick / 2 + thick };
	RECT vbar = { cx - thick / 2, r.top + S(6), cx - thick / 2 + thick, r.bottom - S(6) };
	FillRectColor(dc, hbar, RGB(0, 0, 0));
	FillRectColor(dc, vbar, RGB(0, 0, 0));
}

// ---- 输出 --------------------------------------------------------------
void BuildOutputDir()
{
	if (g.outDir[0]) return;

	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;

	_snwprintf_s(g.outDir, _countof(g.outDir), _TRUNCATE, L"%s\\probe-out", exe);
	CreateDirectoryW(g.outDir, nullptr);
}

void LoadIni()
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;

	wchar_t ini[MAX_PATH] = {};
	_snwprintf_s(ini, _countof(ini), _TRUNCATE, L"%s\\CaptureProbe.ini", exe);

	if (!g.expectPath[0])
	{
		wchar_t rel[MAX_PATH] = {};
		GetPrivateProfileStringW(L"Probe", L"ExpectImage", L"", rel, MAX_PATH, ini);
		if (rel[0])
		{
			// 相对路径按 exe 所在目录解析，免得被工作目录坑一次。
			if (rel[1] != L':' && rel[0] != L'\\')
			{
				_snwprintf_s(g.expectPath, _countof(g.expectPath), _TRUNCATE, L"%s\\%s", exe, rel);
			}
			else
			{
				wcsncpy_s(g.expectPath, _countof(g.expectPath), rel, _TRUNCATE);
			}
			ImageLoadFile(g.expectPath, g.expect);
		}
	}

	// 命令行给的那份也要在这里统一加载：之前只在 ini 分支里调了 ImageLoadFile，
	// --expect-image 传进来的路径根本没被读，指纹比对一直退化成 ReplacedOther。
	if (g.expectPath[0] && !g.expect.Valid())
		ImageLoadFile(g.expectPath, g.expect);
}

bool DetectHookModule()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE) return false;

	MODULEENTRY32W me = {};
	me.dwSize = sizeof(me);
	bool found = false;
	if (Module32FirstW(snap, &me))
	{
		do
		{
			if (wcsstr(me.szModule, L"TrainerHooks"))
			{
				wcsncpy_s(g.moduleName, _countof(g.moduleName), me.szModule, _TRUNCATE);
				found = true;
				break;
			}
		} while (Module32NextW(snap, &me));
	}
	CloseHandle(snap);
	return found;
}

void AppendReportCsv(const Item& it, int index)
{
	wchar_t path[MAX_PATH] = {};
	_snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\report.csv", g.outDir);

	const bool fresh = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
	HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;

	auto writeUtf8 = [h](const wchar_t* text)
	{
		const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (len <= 1) return;
		std::vector<char> buf((size_t)len);
		WideCharToMultiByte(CP_UTF8, 0, text, -1, buf.data(), len, nullptr, nullptr);
		DWORD written = 0;
		WriteFile(h, buf.data(), (DWORD)(len - 1), &written, nullptr);
	};
	auto newline = [h]()
	{
		DWORD written = 0;
		WriteFile(h, "\r\n", 2, &written, nullptr);
	};

	if (fresh)
	{
		const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
		DWORD written = 0;
		WriteFile(h, bom, 3, &written, nullptr);
		writeUtf8(L"time,index,method,verdict,target_ratio,expect_distance,mean_luma,width,height,ms,file,note");
		newline();
	}

	SYSTEMTIME st = {};
	GetLocalTime(&st);

	// 视频判定并进备注列：CSV 的列已经够多了，再多一列会把 Excel 撑出屏幕。
	wchar_t video[192] = {};
	ItemVideoText(it, video, _countof(video));
	wchar_t note[768] = {};
	if (video[0])
	{
		_snwprintf_s(note, _countof(note), _TRUNCATE, L"%s%s%s",
			it.probe.note, it.probe.note[0] ? L"；" : L"", video);
	}
	else
	{
		wcsncpy_s(note, _countof(note), it.probe.note, _TRUNCATE);
	}

	wchar_t line[1280] = {};
	_snwprintf_s(line, _countof(line), _TRUNCATE,
		L"%04d-%02d-%02d %02d:%02d:%02d,%d,%s,%s,%.2f,%.1f,%d,%d,%d,%lu,%s,%s",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		index + 1, MethodAt(index).name, ItemVerdictText(it),
		it.analysis.targetRatio, it.analysis.expectDistance, it.analysis.meanLuma,
		it.probe.image.width, it.probe.image.height, it.probe.elapsedMs,
		it.hasFile ? it.file : L"", note);
	writeUtf8(line);
	newline();

	CloseHandle(h);
}

void EnsureConsole()
{
	// 已经被重定向到文件/管道时不要抢回 CONOUT$，否则调用方拿不到输出。
	const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out && out != INVALID_HANDLE_VALUE)
	{
		const DWORD type = GetFileType(out);
		if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
			return;
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
	{
		FILE* dummy = nullptr;
		freopen_s(&dummy, "CONOUT$", "w", stdout);
	}
}

bool WriteUtf8File(const wchar_t* path, const std::wstring& text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
	DWORD written = 0;
	WriteFile(h, bom, 3, &written, nullptr);

	const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
		nullptr, 0, nullptr, nullptr);
	if (len > 0)
	{
		std::vector<char> buf((size_t)len);
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
			buf.data(), len, nullptr, nullptr);
		WriteFile(h, buf.data(), (DWORD)len, &written, nullptr);
	}
	CloseHandle(h);
	return true;
}

// ---- 运行 --------------------------------------------------------------
bool IsTargetObscured()
{
	if (IsIconic(g.hwnd)) return true;
	POINT pt = { (g.targetScreen.left + g.targetScreen.right) / 2,
		(g.targetScreen.top + g.targetScreen.bottom) / 2 };
	HWND hit = WindowFromPoint(pt);
	return hit != g.hwnd;
}

// 单纯 SetForegroundWindow 经常被前台锁拒掉（尤其是从终端启动的时候），
// 结果整轮都判成"靶窗被遮挡"。标准解法是临时把输入队列接到前台线程上，
// 再借用一次 TOPMOST 把窗口顶上去。
void ForceForeground(HWND hwnd)
{
	if (!hwnd) return;
	if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
	if (GetForegroundWindow() == hwnd) return;

	const HWND fg = GetForegroundWindow();
	const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
	const DWORD myThread = GetCurrentThreadId();
	bool attached = false;
	if (fgThread && fgThread != myThread)
		attached = AttachThreadInput(fgThread, myThread, TRUE) != FALSE;

	const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
	if (!g.topmost)
		SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);

	if (attached)
		AttachThreadInput(fgThread, myThread, FALSE);

	Sleep(80);
}

void UpdateTargetScreenRect()
{
	if (!g.hwnd) return;
	RECT cr = {};
	GetClientRect(g.hwnd, &cr);
	const Layout l = ComputeLayout(cr.right, cr.bottom);
	const RECT tc = TargetClientRect(l);

	POINT a = { tc.left, tc.top };
	POINT b = { tc.right, tc.bottom };
	ClientToScreen(g.hwnd, &a);
	ClientToScreen(g.hwnd, &b);
	g.targetScreen = { a.x, a.y, b.x, b.y };
}

void SyncVideoStripRect()
{
	if (!g.hwnd) return;
	RECT cr = {};
	GetClientRect(g.hwnd, &cr);
	const Layout l = ComputeLayout(cr.right, cr.bottom);

	if (l.hasVideo)
	{
		VideoWallSetRect(l.videoStrip);
	}
	else
	{
		RECT empty = {};
		VideoWallSetRect(empty);
	}
}

void RunOneInternal(int index)
{
	if (index < 0 || index >= g.count) return;

	const MethodDesc& md = MethodAt(index);
	if (md.wholeScreen)
		ForceForeground(g.hwnd);

	UpdateTargetScreenRect();

	ProbeContext ctx = {};
	ctx.hwnd = g.hwnd;
	ctx.virtualScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	ctx.virtualScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	ctx.virtualScreen.right = ctx.virtualScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	ctx.virtualScreen.bottom = ctx.virtualScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

	Item& it = g.items[index];
	it = Item();

	// 抓图前把视频靶往前推一两帧。不推的话一轮 14 项跑下来画面一动不动，
	// "抓到的是不是本帧"就没那么有说服力了。
	VideoWallPump(70);
	RunMethod(index, ctx, it.probe);

	if (it.probe.selfTest)
	{
		it.analysis.verdict = it.probe.selfTestOk ? Verdict::Pass : Verdict::ReplacedOther;
	}
	else if (!it.probe.captured)
	{
		it.analysis.verdict = Verdict::Failed;
	}
	else
	{
		AnalyzeInput in = {};
		in.image = &it.probe.image;
		in.screenRect = it.probe.screenRect;
		in.targetRect = g.targetScreen;
		in.expect = g.expect.Valid() ? &g.expect : nullptr;
		in.targetless = (index == (int)MethodId::GetDib16bpp);
		it.analysis = AnalyzeCapture(in);

		if (md.wholeScreen && it.analysis.verdict != Verdict::Pass && IsTargetObscured())
		{
			it.analysis.verdict = Verdict::Blocked;
			if (!it.probe.note[0])
				wcsncpy_s(it.probe.note, _countof(it.probe.note),
					L"靶窗不在最前，本次结果不可信", _TRUNCATE);
		}
	}

	// 视频靶：同一张抓图里每块内容有没有真的进来。
	// 自检项的图是内存里造的，跟屏幕无关，跳过。
	if (!it.probe.selfTest)
		VideoWallAnalyze(it.probe.image, it.probe.screenRect, VideoWallFrame(), it.video);

	if (it.probe.captured && it.probe.image.Valid())
	{
		SYSTEMTIME st = {};
		GetLocalTime(&st);
		_snwprintf_s(it.file, _countof(it.file), _TRUNCATE,
			L"%s\\%02d-%04d%02d%02d-%02d%02d%02d.png",
			g.outDir, index + 1, st.wYear, st.wMonth, st.wDay,
			st.wHour, st.wMinute, st.wSecond);
		it.hasFile = ImageSavePng(it.file, it.probe.image);
	}

	AppendReportCsv(it, index);
	if (it.analysis.verdict == Verdict::Failed) ++g.failedCount;

	InvalidateRect(g.hwnd, nullptr, FALSE);
	UpdateWindow(g.hwnd);
}

void RunOne(int index)
{
	if (g.running) return;
	g.running = true;
	RunOneInternal(index);
	g.running = false;
}

void RunAll()
{
	if (g.running) return;
	g.running = true;
	g.failedCount = 0;

	if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
	ForceForeground(g.hwnd);
	Sleep(120);

	for (int i = 0; i < g.count; ++i)
	{
		g.selected = i;
		RunOneInternal(i);
	}

	g.running = false;
	if (IsTargetObscured())
	{
		_snwprintf_s(g.status, _countof(g.status), _TRUNCATE,
			L"跑完 %d 项，失败 %d 项 —— 靶窗没能在最前，整屏类结果不可信", g.count, g.failedCount);
	}
	else
	{
		_snwprintf_s(g.status, _countof(g.status), _TRUNCATE,
			L"跑完 %d 项，失败 %d 项。结果在 %s", g.count, g.failedCount, g.outDir);
	}

	if (g.batch)
	{
		wchar_t line[768] = {};
		wchar_t video[192] = {};
		std::wstring text;
		text += L"序号\t方法\t判定\t靶色\t视频\t耗时\t备注\r\n";
		for (int i = 0; i < g.count; ++i)
		{
			const Item& it = g.items[i];
			ItemVideoText(it, video, _countof(video));
			_snwprintf_s(line, _countof(line), _TRUNCATE, L"%02d\t%s\t%s\t%d%%\t%s\t%lu ms\t%s\r\n",
				i + 1, MethodAt(i).name, ItemVerdictText(it),
				(int)(it.analysis.targetRatio * 100.0 + 0.5), video,
				it.probe.elapsedMs, it.probe.note);
			text += line;
		}

		wchar_t summaryPath[MAX_PATH] = {};
		_snwprintf_s(summaryPath, _countof(summaryPath), _TRUNCATE, L"%s\\summary.txt", g.outDir);
		WriteUtf8File(summaryPath, text);

		EnsureConsole();
		wprintf(L"\n%s\n报告：%s\\report.csv\n", text.c_str(), g.outDir);
		fflush(stdout);
	}

	InvalidateRect(g.hwnd, nullptr, FALSE);
	UpdateWindow(g.hwnd);
}

void CopyReportToClipboard()
{
	std::wstring text;
	wchar_t line[768] = {};
	wchar_t video[192] = {};
	for (int i = 0; i < g.count; ++i)
	{
		const Item& it = g.items[i];
		ItemVideoText(it, video, _countof(video));
		_snwprintf_s(line, _countof(line), _TRUNCATE, L"%02d\t%s\t%s\t%d%%\t%s\t%lu ms\t%s\r\n",
			i + 1, MethodAt(i).name, ItemVerdictText(it),
			(int)(it.analysis.targetRatio * 100.0 + 0.5), video,
			it.probe.elapsedMs, it.probe.note);
		text += line;
	}

	if (!OpenClipboard(g.hwnd)) return;
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (mem)
	{
		void* dst = GlobalLock(mem);
		if (dst)
		{
			memcpy(dst, text.c_str(), bytes);
			GlobalUnlock(mem);
			SetClipboardData(CF_UNICODETEXT, mem);
			mem = nullptr;
		}
		if (mem) GlobalFree(mem);
	}
	CloseClipboard();
	_snwprintf_s(g.status, _countof(g.status), _TRUNCATE, L"报告已复制到剪贴板");
	InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---- 绘制 --------------------------------------------------------------
void DrawButton(HDC dc, const RECT& r, const wchar_t* text, bool primary)
{
	RoundedPanel(dc, r, primary ? RGB(0x2A, 0x5C, 0x9E) : RGB(0x1B, 0x22, 0x2C),
		primary ? RGB(0x3E, 0x7C, 0xC8) : kLine);
	DrawTextIn(dc, text, r, primary ? RGB(0xFF, 0xFF, 0xFF) : kText, g.fBody,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawToolbar(HDC dc, const Layout& l, int cw)
{
	FillRectColor(dc, l.toolbar, kPanelDeep);
	FillRectColor(dc, { 0, l.toolbar.bottom - 1, cw, l.toolbar.bottom }, kLine);

	RECT title = { S(20), S(8), S(320), S(38) };
	DrawTextIn(dc, L"截屏探针", title, kText, g.fTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	RECT sub = { S(20), S(36), S(420), S(56) };
	DrawTextIn(dc, L"Capture Probe · 通用抓屏替换验证", sub, kTextDim, g.fSmall,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	const int btnH = S(32);
	const int btnW = S(104);
	g.btnOne = { cw - S(20) - btnW, (l.toolbar.bottom - btnH) / 2,
		cw - S(20), (l.toolbar.bottom - btnH) / 2 + btnH };
	g.btnAll = { g.btnOne.left - S(10) - btnW, g.btnOne.top, g.btnOne.left - S(10), g.btnOne.bottom };

	DrawButton(dc, g.btnAll, L"全部运行 F5", true);
	DrawButton(dc, g.btnOne, L"运行选中 ↵", false);
}

void DrawList(HDC dc, const Layout& l)
{
	RoundedPanel(dc, l.list, kPanel, kLine);

	const int left = l.list.left + S(12);
	const int right = l.list.right - S(12);
	const int headerH = S(26);
	int y = l.list.top + S(8) + headerH + S(4);

	RECT head = { left, l.list.top + S(8), right, l.list.top + S(8) + headerH };
	DrawTextIn(dc, L"抓屏路径", head, kTextDim, g.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	DrawTextIn(dc, L"结果", head, kTextDim, g.fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
	FillRectColor(dc, { left, y - S(4), right, y - S(3) }, kLine);

	const int rowH = l.itemH;
	for (int i = 0; i < g.count; ++i)
	{
		RECT row = { l.list.left + S(4), y, l.list.right - S(4), y + rowH };
		const bool sel = (i == g.selected);
		const Item& it = g.items[i];

		if (sel)
		{
			RoundedPanel(dc, row, RGB(0x1A, 0x23, 0x30), RGB(0x25, 0x31, 0x42));
			FillRectColor(dc, { row.left, row.top + S(6), row.left + S(3), row.bottom - S(6) }, kAccent);
		}

		wchar_t num[8] = {};
		_snwprintf_s(num, _countof(num), _TRUNCATE, L"%02d", i + 1);
		RECT rn = { left, row.top, left + S(26), row.bottom };
		DrawTextIn(dc, num, rn, RGB(0x5F, 0x6B, 0x7E), g.fMono, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		RECT rt = { left + S(28), row.top, right - S(150), row.bottom };
		DrawTextIn(dc, MethodAt(i).name, rt, HasResult(it) ? kText : RGB(0xB2, 0xBC, 0xCB),
			g.fBody, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

		const COLORREF vc = ItemColor(it);
		if (HasResult(it))
			DrawDot(dc, right - S(154), (row.top + row.bottom) / 2, S(3), vc);

		RECT rs = { right - S(144), row.top, right - S(56), row.bottom };
		DrawTextIn(dc, HasResult(it) ? ShortVerdict(it) : L"—", rs, vc, g.fMono,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		wchar_t ms[16] = {};
		if (it.probe.elapsedMs)
			_snwprintf_s(ms, _countof(ms), _TRUNCATE, L"%lu ms", it.probe.elapsedMs);
		RECT rm = { right - S(54), row.top, right, row.bottom };
		DrawTextIn(dc, ms, rm, kTextDim, g.fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

		y += rowH;
	}
}

void DrawPreview(HDC dc, const Layout& l)
{
	RoundedPanel(dc, l.preview, kPanel, kLine);
	RECT inner = l.preview;
	InflateRect(&inner, -S(10), -S(10));

	const Item& it = g.items[g.selected];
	if (it.probe.image.Valid())
	{
		DrawImageFit(dc, it.probe.image, inner);

		wchar_t info[128] = {};
		_snwprintf_s(info, _countof(info), _TRUNCATE, L"%d × %d", it.probe.image.width, it.probe.image.height);
		RECT tag = { inner.left + S(4), inner.top, inner.left + S(200), inner.top + S(22) };
		DrawTextIn(dc, info, tag, RGB(0x9A, 0xA6, 0xB8), g.fSmall,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
	else
	{
		RECT hint = inner;
		DrawTextIn(dc, L"按 F5 跑全部，选中一条按 Enter 单跑\r\n每条都会落一张 PNG，方便逐像素复核",
			hint, kTextDim, g.fBody, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
	}
}

void DrawVerdictBox(HDC dc, const Layout& l)
{
	RoundedPanel(dc, l.verdict, kPanelDeep, kLine);
	const Item& it = g.items[g.selected];

	RECT line1 = { l.verdict.left + S(16), l.verdict.top + S(10), l.verdict.right - S(16), l.verdict.top + S(42) };
	DrawTextIn(dc, HasResult(it) ? ItemVerdictText(it) : L"未运行",
		line1, HasResult(it) ? ItemColor(it) : kTextDim, g.fBig,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	wchar_t detail[256] = {};
	if (HasResult(it) && !it.probe.selfTest)
		it.analysis.Detail(detail, _countof(detail));
	else if (it.probe.selfTest)
		wcsncpy_s(detail, _countof(detail), it.probe.note, _TRUNCATE);

	RECT line2 = { l.verdict.left + S(16), line1.bottom - S(2), l.verdict.right - S(16), line1.bottom + S(22) };
	DrawTextIn(dc, detail, line2, kTextDim, g.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	RECT line3 = { l.verdict.left + S(16), line2.bottom, l.verdict.right - S(16), line2.bottom + S(20) };
	wchar_t videoText[192] = {};
	COLORREF videoColor = RGB(0x5C, 0x66, 0x78);
	if (!it.probe.selfTest && it.video.valid)
	{
		VideoWallReportText(it.video, videoText, _countof(videoText));
		// X 那一路本来就该看不见，别把它算成缺一路。
		const int expected = it.video.covered - (it.video.marks[PaneExcluded] == L'✕' ? 1 : 0);
		videoColor = (expected > 0 && it.video.live >= expected)
			? RGB(0x3D, 0xDC, 0x84) : RGB(0xFF, 0xB0, 0x20);
	}
	DrawTextIn(dc, videoText, line3, videoColor, g.fSmall,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	RECT line4 = { l.verdict.left + S(16), line3.bottom + S(2), l.verdict.right - S(16), l.verdict.bottom - S(8) };
	DrawTextIn(dc, it.probe.selfTest ? L"" : it.probe.note, line4, RGB(0x9A, 0xA6, 0xB8), g.fSmall,
		DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

void DrawVideoCaption(HDC dc, const Layout& l)
{
	if (!l.hasVideo) return;

	RECT cap = l.videoCaption;
	DrawTextIn(dc, VideoWallLegend(), cap, kTextDim, g.fSmall,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	RECT right = cap;
	right.left = cap.left + (cap.right - cap.left) / 2;

	const wchar_t* note = VideoWallInitNote();
	if (note && note[0])
	{
		DrawTextIn(dc, note, right, RGB(0xFF, 0xB0, 0x20), g.fSmall,
			DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
	else
	{
		DrawTextIn(dc, VideoWallMarkLegend(), right, RGB(0x5F, 0x6B, 0x7E), g.fSmall,
			DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
	}
}

void DrawTargetColumn(HDC dc, const Layout& l)
{
	RoundedPanel(dc, l.targetCol, kPanel, kLine);

	RECT cap = { l.targetCol.left, l.targetCol.top + S(12), l.targetCol.right, l.targetCol.top + S(34) };
	DrawTextIn(dc, L"靶标", cap, kTextDim, g.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	const RECT tc = TargetClientRect(l);
	PaintTarget(dc, tc);

	SYSTEMTIME st = {};
	GetLocalTime(&st);
	wchar_t clock[32] = {};
	_snwprintf_s(clock, _countof(clock), _TRUNCATE, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
	RECT cr = { l.targetCol.left, tc.bottom + S(8), l.targetCol.right, tc.bottom + S(30) };
	DrawTextIn(dc, clock, cr, kText, g.fMono, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	RECT hint = { l.targetCol.left + S(10), cr.bottom + S(4), l.targetCol.right - S(10), cr.bottom + S(88) };
	DrawTextIn(dc, L"抓到品红 = 真画面\r\n抓不到 = 已被换掉", hint, kTextDim, g.fSmall,
		DT_CENTER | DT_TOP | DT_WORDBREAK);

	RECT badge = { l.targetCol.left + S(10), l.targetCol.bottom - S(42),
		l.targetCol.right - S(10), l.targetCol.bottom - S(14) };
	const COLORREF c = g.injected ? RGB(0x3D, 0xDC, 0x84) : RGB(0xFF, 0xB0, 0x20);
	RoundedPanel(dc, badge, g.injected ? RGB(0x12, 0x28, 0x1D) : RGB(0x28, 0x20, 0x12), c);
	DrawTextIn(dc, g.injected ? L"钩子已加载" : L"钩子未加载", badge, c, g.fSmall,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawStatusBar(HDC dc, const Layout& l, int cw)
{
	FillRectColor(dc, l.status, RGB(0x0A, 0x0D, 0x12));
	FillRectColor(dc, { 0, l.status.top, cw, l.status.top + 1 }, kLine);

	RECT left = { S(20), l.status.top, cw / 2, l.status.bottom };
	DrawTextIn(dc, g.status[0] ? g.status : L"就绪", left, kTextDim, g.fSmall,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	RECT rr = { cw / 2, l.status.top, cw - S(20), l.status.bottom };
	DrawTextIn(dc, L"F5 全部   Enter 选中   F4 输出目录   F6 复制报告",
		rr, RGB(0x5F, 0x6B, 0x7E), g.fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void PaintAll(HDC dc, int cw, int ch)
{
	if (cw <= 0 || ch <= 0) return;

	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, cw, ch) : nullptr;
	if (!mem || !bmp)
	{
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		return;
	}
	HGDIOBJ old = SelectObject(mem, bmp);

	FillRectColor(mem, { 0, 0, cw, ch }, kBg);
	const Layout l = ComputeLayout(cw, ch);
	DrawToolbar(mem, l, cw);
	DrawList(mem, l);
	DrawPreview(mem, l);
	DrawVideoCaption(mem, l);
	DrawVerdictBox(mem, l);
	DrawTargetColumn(mem, l);
	DrawStatusBar(mem, l, cw);

	BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}

// ---- 窗口过程 ----------------------------------------------------------
void RecreateFonts()
{
	if (g.fTitle) DeleteObject(g.fTitle);
	if (g.fBody) DeleteObject(g.fBody);
	if (g.fSmall) DeleteObject(g.fSmall);
	if (g.fBig) DeleteObject(g.fBig);
	if (g.fMono) DeleteObject(g.fMono);

	const wchar_t* face = L"Microsoft YaHei UI";
	g.fTitle = CreateFontW(-S(19), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	g.fBody = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	g.fSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	g.fBig = CreateFontW(-S(21), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	g.fMono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
}

int HitTestListIndex(const Layout& l, POINT pt)
{
	const int top = l.list.top + S(8) + S(26) + S(4);
	if (pt.y < top) return -1;
	const int idx = (pt.y - top) / l.itemH;
	return (idx >= 0 && idx < g.count) ? idx : -1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		g.hwnd = hwnd;
		g.count = MethodCount();
		if (g.count > (int)(sizeof(g.items) / sizeof(g.items[0])))
			g.count = (int)(sizeof(g.items) / sizeof(g.items[0]));

		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32)
		{
			typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
			PFN_GetDpiForWindow fn = (PFN_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
			if (fn)
			{
				const UINT d = fn(hwnd);
				if (d >= 72) g.dpi = (int)d;
			}
		}

		g.injected = DetectHookModule();
		RecreateFonts();

		// 视频靶：四块 160×90，跟主窗口一起建，位置由布局算。
		VideoWallCreate(hwnd);
		SyncVideoStripRect();

		_snwprintf_s(g.status, _countof(g.status), _TRUNCATE, L"输出目录 %s", g.outDir);
		SetTimer(hwnd, kTimerClock, 1000, nullptr);
		SetTimer(hwnd, kTimerVideo, 33, nullptr);
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
	{
		PAINTSTRUCT ps = {};
		HDC dc = BeginPaint(hwnd, &ps);
		RECT cr = {};
		GetClientRect(hwnd, &cr);
		UpdateTargetScreenRect();
		PaintAll(dc, cr.right - cr.left, cr.bottom - cr.top);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_SIZE:
		SyncVideoStripRect();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOVE:
		// 分层那块是独立顶层窗，跟着主窗搬。
		SyncVideoStripRect();
		return 0;

	case WM_TIMER:
		if (wp == kTimerClock)
		{
			// 注入发生在窗口建好之后，g.injected 只在 WM_CREATE 取过一次 ——
			// 不补检的话，替身钩子明明生效了，角标还一直挂着"钩子未加载"。
			// 查到就停，只有没查到的时候每秒扫一次模块链表。
			if (!g.injected) DetectHookModule();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		else if (wp == kTimerBatch)
		{
			KillTimer(hwnd, kTimerBatch);
			RunAll();
			if (g.quitAfterBatch) DestroyWindow(hwnd);
		}
		else if (wp == kTimerVideo)
		{
			VideoWallTick();
		}
		return 0;

	case WM_LBUTTONDOWN:
	{
		POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		if (PtInRect(&g.btnAll, pt)) { RunAll(); return 0; }
		if (PtInRect(&g.btnOne, pt)) { RunOne(g.selected); return 0; }

		RECT cr = {};
		GetClientRect(hwnd, &cr);
		const Layout l = ComputeLayout(cr.right, cr.bottom);
		if (PtInRect(&l.list, pt))
		{
			const int idx = HitTestListIndex(l, pt);
			if (idx >= 0)
			{
				g.selected = idx;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	}

	case WM_LBUTTONDBLCLK:
	{
		POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		RECT cr = {};
		GetClientRect(hwnd, &cr);
		const Layout l = ComputeLayout(cr.right, cr.bottom);
		if (PtInRect(&l.list, pt))
		{
			const int idx = HitTestListIndex(l, pt);
			if (idx >= 0)
			{
				g.selected = idx;
				RunOne(idx);
			}
		}
		return 0;
	}

	case WM_KEYDOWN:
		switch (wp)
		{
		case VK_F5:
			RunAll();
			return 0;
		case VK_RETURN:
			RunOne(g.selected);
			return 0;
		case VK_UP:
			if (g.selected > 0) { --g.selected; InvalidateRect(hwnd, nullptr, FALSE); }
			return 0;
		case VK_DOWN:
			if (g.selected + 1 < g.count) { ++g.selected; InvalidateRect(hwnd, nullptr, FALSE); }
			return 0;
		case VK_F4:
			ShellExecuteW(hwnd, L"open", g.outDir, nullptr, nullptr, SW_SHOWNORMAL);
			return 0;
		case VK_F6:
			CopyReportToClipboard();
			return 0;
		default:
			break;
		}
		break;

	case WM_CLOSE:
		// 跑一轮的时候会把消息泵进来（视频要动），这时销毁窗口会死在栈上。
		if (g.running)
		{
			_snwprintf_s(g.status, _countof(g.status), _TRUNCATE, L"正在跑，等这一轮结束再关");
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, kTimerClock);
		KillTimer(hwnd, kTimerVideo);
		VideoWallDestroy();
		if (g.fTitle) DeleteObject(g.fTitle);
		if (g.fBody) DeleteObject(g.fBody);
		if (g.fSmall) DeleteObject(g.fSmall);
		if (g.fBig) DeleteObject(g.fBig);
		if (g.fMono) DeleteObject(g.fMono);
		PostQuitMessage(0);
		return 0;

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnableDpiAwareness()
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32)
	{
		typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
		PFN_SetCtx setCtx = (PFN_SetCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		if (setCtx && setCtx((HANDLE)-4))   // PER_MONITOR_AWARE_V2
			return;
	}
	SetProcessDPIAware();
}

int SystemDpi()
{
	HDC dc = GetDC(nullptr);
	const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc) ReleaseDC(nullptr, dc);
	return dpi >= 72 ? dpi : 96;
}

void ParseCommandLine()
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return;

	for (int i = 1; i < argc; ++i)
	{
		const wchar_t* a = argv[i];
		if (wcscmp(a, L"--all") == 0)
		{
			g.batch = true;
			g.quitAfterBatch = true;
		}
		else if (wcscmp(a, L"--keep-open") == 0)
		{
			g.quitAfterBatch = false;
		}
		else if (wcscmp(a, L"--topmost") == 0)
		{
			g.topmost = true;
		}
		else if (wcscmp(a, L"--no-target") == 0)
		{
			g.hideTarget = true;
		}
		else if (wcscmp(a, L"--out") == 0 && i + 1 < argc)
		{
			wcsncpy_s(g.outDir, _countof(g.outDir), argv[++i], _TRUNCATE);
			CreateDirectoryW(g.outDir, nullptr);
		}
		else if (wcscmp(a, L"--expect-image") == 0 && i + 1 < argc)
		{
			wcsncpy_s(g.expectPath, _countof(g.expectPath), argv[++i], _TRUNCATE);
		}
	}
	LocalFree(argv);
}

} // namespace

int APIENTRY WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
	EnableDpiAwareness();
	g.dpi = SystemDpi();
	ImageIoInit();      // LoadIni 里要读替换图，WIC 必须先就绪
	ParseCommandLine();
	LoadIni();
	BuildOutputDir();

	g.inst = inst;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = L"CaptureProbeWindow";
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);

	// 默认放宽到 1360：视频靶那条带要 658 像素才摆得下四块，
	// 加上列表 470 + 靶标列 156 + 三处留白 46，正好是 1348。
	RECT rc = { 0, 0, S(1360), S(816) };
	AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	const int maxW = GetSystemMetrics(SM_CXSCREEN) - S(40);
	const int maxH = GetSystemMetrics(SM_CYSCREEN) - S(60);
	if (w > maxW) w = maxW;
	if (h > maxH) h = maxH;

	const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
	const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

	HWND hwnd = CreateWindowExW(g.topmost ? WS_EX_TOPMOST : 0, L"CaptureProbeWindow",
		L"截屏探针 · Capture Probe", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, w, h, nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return 2;

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	if (g.batch)
		SetTimer(hwnd, kTimerBatch, 400, nullptr);

	MSG msg = {};
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	ImageIoShutdown();
	return g.failedCount > 0 ? 1 : 0;
}
