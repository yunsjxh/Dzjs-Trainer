#include "VideoWall.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

const wchar_t kStripClass[] = L"CaptureProbeVideoStrip";
const wchar_t kPaneClass[] = L"CaptureProbeVideoPane";

HWND g_parent = nullptr;
HWND g_strip = nullptr;
HWND g_pane[PaneCount] = {};
bool g_ok[PaneCount] = {};
bool g_classReady = false;
bool g_visible = false;
unsigned g_frame = 0;

Image32 g_scene;
HDC g_sceneDc = nullptr;
HBITMAP g_sceneBmp = nullptr;
void* g_sceneBits = nullptr;
BITMAPINFO g_bi = {};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain1* g_swap = nullptr;
ID3D11Texture2D* g_tex = nullptr;

wchar_t g_initNote[192] = {};

// ---- 画面 --------------------------------------------------------------
void FillBox(Image32& img, int x0, int y0, int x1, int y1,
	unsigned char r, unsigned char g, unsigned char b)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > img.width) x1 = img.width;
	if (y1 > img.height) y1 = img.height;
	for (int y = y0; y < y1; ++y)
	{
		uint8_t* row = img.Row(y);
		for (int x = x0; x < x1; ++x)
		{
			uint8_t* p = row + (size_t)x * 4;
			p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
		}
	}
}

// 七段数码管，位序 a b c d e f g。帧号全程序化画，不依赖字体。
const unsigned char kSegMask[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

void DrawDigit(Image32& img, int x, int y, int w, int h, int t, int digit)
{
	if (digit < 0 || digit > 9) return;
	const unsigned char m = kSegMask[digit];
	const int mid = y + h / 2;
	auto seg = [&](int a, int b, int c, int d) { FillBox(img, a, b, c, d, 255, 255, 255); };
	if (m & 0x01) seg(x + t, y, x + w - t, y + t);                        // a
	if (m & 0x02) seg(x + w - t, y + t, x + w, mid);                      // b
	if (m & 0x04) seg(x + w - t, mid, x + w, y + h - t);                  // c
	if (m & 0x08) seg(x + t, y + h - t, x + w - t, y + h);                // d
	if (m & 0x10) seg(x, mid, x + t, y + h - t);                          // e
	if (m & 0x20) seg(x, y + t, x + t, mid);                              // f
	if (m & 0x40) seg(x + t, mid - t / 2, x + w - t, mid + (t + 1) / 2);  // g
}

// 分层窗和"系统排除"那块必须是顶层窗：SetWindowDisplayAffinity 只认
// 顶层窗口，挂在子窗口上直接返回 FALSE（试过，白忙一场）。
bool IsTopLevelPane(int idx)
{
	return idx == PaneLayered || idx == PaneExcluded;
}

int ScanBarWidth() { return kVideoPaneW / 32; }   // 5px

int ScanBarX(unsigned frame)
{
	return (int)((frame % kVideoSweep) * (unsigned)(kVideoPaneW - ScanBarWidth()) / (kVideoSweep - 1));
}

void RenderFrame(unsigned frame, Image32& out)
{
	if (out.width != kVideoPaneW || out.height != kVideoPaneH)
		out.Alloc(kVideoPaneW, kVideoPaneH);

	// 上半部六条高饱和彩条。白色只留给扫描条，否则下面找条时会把背景算进来。
	static const unsigned char kBarColor[6][3] =
	{
		{ 255, 212,   0 }, {   0, 200, 200 }, {  32, 192,  32 },
		{ 200,  32, 200 }, { 208,  32,  32 }, {  32,  48, 208 },
	};
	const int stripH = kVideoPaneH * 64 / 100;
	for (int i = 0; i < 6; ++i)
	{
		FillBox(out, i * kVideoPaneW / 6, 0, (i + 1) * kVideoPaneW / 6, stripH,
			kBarColor[i][0], kBarColor[i][1], kBarColor[i][2]);
	}

	FillBox(out, 0, stripH, kVideoPaneW, kVideoPaneH, 14, 16, 20);

	const unsigned shown = frame % 1000u;
	int cx = 8;
	for (int shift = 2; shift >= 0; --shift)
	{
		const int div = (shift == 2) ? 100 : (shift == 1) ? 10 : 1;
		DrawDigit(out, cx, stripH + 4, 12, 20, 3, (int)((shown / div) % 10));
		cx += 15;
	}

	// 底部细进度条，肉眼一秒就能看出在动。
	FillBox(out, 8, kVideoPaneH - 6, kVideoPaneW - 8, kVideoPaneH - 3, 38, 42, 50);
	const int filled = (int)((frame % kVideoSweep) * (unsigned)(kVideoPaneW - 16) / (kVideoSweep - 1));
	FillBox(out, 8, kVideoPaneH - 6, 8 + filled, kVideoPaneH - 3, 60, 220, 110);

	// 扫描条最后画，压在所有内容上面。
	const int x0 = ScanBarX(frame);
	FillBox(out, x0, 0, x0 + ScanBarWidth(), kVideoPaneH, 255, 255, 255);
}

double LocateBar(const Image32& img)
{
	if (!img.Valid() || img.width < 16) return -1.0;

	std::vector<int> counts((size_t)img.width, 0);
	int bestCol = -1;
	int bestCount = 0;
	for (int x = 0; x < img.width; ++x)
	{
		int c = 0;
		for (int y = 0; y < img.height; ++y)
		{
			const uint8_t* p = img.Row(y) + (size_t)x * 4;
			if (p[0] >= 235 && p[1] >= 235 && p[2] >= 235) ++c;
		}
		counts[(size_t)x] = c;
		if (c > bestCount) { bestCount = c; bestCol = x; }
	}

	// 扫描条竖贯整幅，帧号的白色笔画只有二十像素高。用高度比例把两者分开。
	if (bestCol < 0 || bestCount < img.height * 45 / 100) return -1.0;

	int l = bestCol;
	int r = bestCol;
	const int keep = bestCount * 3 / 4;
	while (l - 1 >= 0 && counts[(size_t)(l - 1)] >= keep) --l;
	while (r + 1 < img.width && counts[(size_t)(r + 1)] >= keep) ++r;

	// 整片纯白（PrintWindow 抓 D3D 子窗口时会吐这个）每一列都"很白"，
	// 会被当成一条从左边贯到右边的扫描条，中心刚好落在画面中间。
	// 扫描条只占画面宽度的几十分之一，宽过 1/8 的一定不是它。
	if (r - l + 1 > img.width / 8) return -1.0;

	return ((double)(l + r) * 0.5 + 0.5) / (double)img.width;
}

double BarAt(unsigned frame)
{
	return (ScanBarX(frame) + ScanBarWidth() * 0.5) / (double)kVideoPaneW;
}

bool BarMatches(double observed, unsigned frame)
{
	if (observed < 0.0) return false;
	// ±4 帧。DWM 合成、远程桌面转发都会让屏幕内容比帧号慢一两帧，
	// 容差太紧会把真画面误判成对不上，太松又分不出别的白条。
	const double tol = 4.0 * (double)(kVideoPaneW - ScanBarWidth()) / (double)(kVideoSweep - 1) / (double)kVideoPaneW;
	return fabs(observed - BarAt(frame)) <= tol;
}

// ---- 四块靶子的窗口 ----------------------------------------------------
void BlitScene(HDC dc)
{
	if (!dc || !g_sceneBits) return;
	SetDIBitsToDevice(dc, 0, 0, kVideoPaneW, kVideoPaneH,
		0, 0, 0, kVideoPaneH, g_sceneBits, &g_bi, DIB_RGB_COLORS);
}

LRESULT CALLBACK PaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
	{
		PAINTSTRUCT ps = {};
		HDC dc = BeginPaint(hwnd, &ps);
		const int idx = (int)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		if (idx == PaneGdi || idx == PaneExcluded) BlitScene(dc);
		EndPaint(hwnd, &ps);
		return 0;
	}

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// 条带底板：纯黑。抓屏抓不到某一路时，那块区域露出来的就是它 ——
// 用黑色当"这里什么都没有"的底，判定才好写。
LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
	{
		// 没有背景刷，DefWindowProc 不会替我们擦；自己填黑，
		// 这样"抓不到某一路"时露出来的就是纯黑。
		PAINTSTRUCT ps = {};
		HDC dc = BeginPaint(hwnd, &ps);
		RECT rc = {};
		GetClientRect(hwnd, &rc);
		HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
		FillRect(dc, &rc, b);

		// 格与格之间留一条深灰。截图里某一路是黑的话，没有这条线就分不清
		// 是"那一格被排除了"还是"整条都黑"。
		HBRUSH gap = CreateSolidBrush(RGB(0x22, 0x26, 0x2E));
		for (int i = 1; i < PaneCount; ++i)
		{
			RECT g = { i * kVideoPaneW + (i - 1) * kVideoGap, 0,
				i * kVideoPaneW + i * kVideoGap, kVideoPaneH };
			FillRect(dc, &g, gap);
		}
		DeleteObject(gap);

		DeleteObject(b);
		EndPaint(hwnd, &ps);
		return 0;
	}

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureClasses()
{
	if (g_classReady) return true;

	HINSTANCE inst = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = PaneProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = kPaneClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	wc.lpfnWndProc = StripProc;
	wc.lpszClassName = kStripClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	g_classReady = true;
	return true;
}

bool EnsureGdiBuffers()
{
	if (g_sceneBits) return true;

	g_bi = {};
	g_bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	g_bi.bmiHeader.biWidth = kVideoPaneW;
	g_bi.bmiHeader.biHeight = -kVideoPaneH;
	g_bi.bmiHeader.biPlanes = 1;
	g_bi.bmiHeader.biBitCount = 32;
	g_bi.bmiHeader.biCompression = BI_RGB;

	g_sceneDc = CreateCompatibleDC(nullptr);
	g_sceneBmp = CreateDIBSection(g_sceneDc, &g_bi, DIB_RGB_COLORS, &g_sceneBits, nullptr, 0);
	if (!g_sceneDc || !g_sceneBmp)
	{
		if (g_sceneBmp) { DeleteObject(g_sceneBmp); g_sceneBmp = nullptr; }
		if (g_sceneDc) { DeleteDC(g_sceneDc); g_sceneDc = nullptr; }
		g_sceneBits = nullptr;
		return false;
	}
	SelectObject(g_sceneDc, g_sceneBmp);
	return true;
}

// ---- D3D11 ------------------------------------------------------------
void ReleaseD3D()
{
	if (g_swap) { g_swap->Release(); g_swap = nullptr; }
	if (g_tex) { g_tex->Release(); g_tex = nullptr; }
	if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
	if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

bool InitD3D(HWND hwnd)
{
	D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;

	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, _countof(want), D3D11_SDK_VERSION,
		&g_dev, &got, &g_ctx);
	if (FAILED(hr))
	{
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, _countof(want), D3D11_SDK_VERSION,
			&g_dev, &got, &g_ctx);
	}
	if (FAILED(hr))
	{
		_snwprintf_s(g_initNote, _countof(g_initNote), _TRUNCATE, L"D3D11 建不起来 0x%08X", (unsigned)hr);
		ReleaseD3D();
		return false;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = kVideoPaneW;
	td.Height = kVideoPaneH;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	hr = g_dev->CreateTexture2D(&td, nullptr, &g_tex);
	if (FAILED(hr))
	{
		_snwprintf_s(g_initNote, _countof(g_initNote), _TRUNCATE, L"D3D 纹理建不起来 0x%08X", (unsigned)hr);
		ReleaseD3D();
		return false;
	}

	IDXGIDevice* dxgiDevice = nullptr;
	hr = g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
	if (FAILED(hr)) { ReleaseD3D(); return false; }

	IDXGIAdapter* adapter = nullptr;
	hr = dxgiDevice->GetAdapter(&adapter);
	dxgiDevice->Release();
	if (FAILED(hr)) { ReleaseD3D(); return false; }

	IDXGIFactory2* factory = nullptr;
	hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
	adapter->Release();
	if (FAILED(hr)) { ReleaseD3D(); return false; }

	// 翻转模型：播放器、浏览器、Electron 用的都是这条合成路径。
	// 位块传输模型在 DWM 下早就不是默认选择了。
	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = kVideoPaneW;
	sd.Height = kVideoPaneH;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	hr = factory->CreateSwapChainForHwnd(g_dev, hwnd, &sd, nullptr, nullptr, &g_swap);
	factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
	factory->Release();
	if (FAILED(hr))
	{
		_snwprintf_s(g_initNote, _countof(g_initNote), _TRUNCATE, L"D3D 交换链建不起来 0x%08X", (unsigned)hr);
		ReleaseD3D();
		return false;
	}
	return true;
}

void PresentD3D()
{
	if (!g_swap || !g_ctx || !g_tex) return;
	g_ctx->UpdateSubresource(g_tex, 0, nullptr, g_scene.pixels.data(), (UINT)g_scene.stride, 0);

	ID3D11Texture2D* back = nullptr;
	if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
	{
		g_ctx->CopyResource(back, g_tex);
		back->Release();
	}
	g_swap->Present(0, 0);
}

void UpdateLayered()
{
	if (!g_ok[PaneLayered] || !g_sceneBits) return;

	SIZE size = { kVideoPaneW, kVideoPaneH };
	POINT src = { 0, 0 };
	BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	UpdateLayeredWindow(g_pane[PaneLayered], nullptr, nullptr, &size,
		g_sceneDc, &src, 0, &blend, ULW_ALPHA);
}

void DrawGdiPane(int idx)
{
	if (!g_ok[idx]) return;
	HDC dc = GetDC(g_pane[idx]);
	if (!dc) return;
	BlitScene(dc);
	ReleaseDC(g_pane[idx], dc);
}

void Tick()
{
	if (!g_strip || !g_visible) return;

	++g_frame;
	RenderFrame(g_frame, g_scene);
	if (g_sceneBits)
		memcpy(g_sceneBits, g_scene.pixels.data(), (size_t)kVideoPaneW * (size_t)kVideoPaneH * 4);

	DrawGdiPane(PaneGdi);
	DrawGdiPane(PaneExcluded);
	UpdateLayered();
	PresentD3D();
}

RECT PaneScreenRect(int idx)
{
	RECT r = {};
	if (g_pane[idx]) GetWindowRect(g_pane[idx], &r);
	return r;
}

} // namespace

// ---- 对外 --------------------------------------------------------------

int VideoWallStripWidth()
{
	return PaneCount * kVideoPaneW + (PaneCount - 1) * kVideoGap;
}

int VideoWallStripHeight()
{
	return kVideoPaneH;
}

const wchar_t* VideoWallLegend()
{
	return L"视频靶 30fps · G 直绘 / D D3D11 / L 分层 / X 系统排除";
}

const wchar_t* VideoWallMarkLegend()
{
	return L"✔ 本帧   ✕ 看不见   ? 非本帧   - 未覆盖";
}

bool VideoWallCreate(HWND parent)
{
	if (g_strip) return true;
	if (!parent) return false;
	if (!EnsureClasses() || !EnsureGdiBuffers()) return false;

	g_parent = parent;
	g_scene.Alloc(kVideoPaneW, kVideoPaneH);
	RenderFrame(0, g_scene);
	memcpy(g_sceneBits, g_scene.pixels.data(), (size_t)kVideoPaneW * (size_t)kVideoPaneH * 4);

	g_strip = CreateWindowExW(0, kStripClass, L"",
		WS_CHILD | WS_CLIPCHILDREN, 0, 0, VideoWallStripWidth(), kVideoPaneH,
		parent, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!g_strip) return false;

	HINSTANCE inst = GetModuleHandleW(nullptr);
	for (int i = 0; i < PaneCount; ++i)
	{
		const int x = i * (kVideoPaneW + kVideoGap);
		if (IsTopLevelPane(i))
		{
			// 分层窗是 overlay 那类实现的真实形态：独立顶层窗（owner = 主窗），
			// 不在主窗的窗口 DC 里，PrintWindow 抓主窗时自然没有它。
			DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
			if (i == PaneLayered) ex |= WS_EX_LAYERED;
			g_pane[i] = CreateWindowExW(ex, kPaneClass, L"", WS_POPUP,
				0, 0, kVideoPaneW, kVideoPaneH, parent, nullptr, inst, nullptr);
		}
		else
		{
			g_pane[i] = CreateWindowExW(0, kPaneClass, L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, x, 0, kVideoPaneW, kVideoPaneH,
				g_strip, nullptr, inst, nullptr);
		}

		if (!g_pane[i]) continue;
		SetWindowLongPtrW(g_pane[i], GWLP_USERDATA, (LONG_PTR)i);
		g_ok[i] = true;
	}

	if (g_ok[PaneD3d] && !InitD3D(g_pane[PaneD3d]))
	{
		// D3D 起不来就让那块停在黑底上，判定会如实报"抓不到内容"。
		ShowWindow(g_pane[PaneD3d], SW_HIDE);
		g_ok[PaneD3d] = false;
	}

	if (g_ok[PaneExcluded])
	{
		// 让系统自己把这块排除在抓屏之外。这是应用层能做到的最硬的一招：
		// 谁来抓都抓不到，跟装了什么钩子无关。
		DWORD got = 0;
		bool ok = SetWindowDisplayAffinity(g_pane[PaneExcluded], WDA_EXCLUDEFROMCAPTURE) != FALSE;
		if (!ok && SetWindowDisplayAffinity(g_pane[PaneExcluded], WDA_MONITOR))
		{
			ok = true;
			wcsncat_s(g_initNote, _countof(g_initNote),
				L"；系统不支持 EXCLUDEFROMCAPTURE，降级 WDA_MONITOR", _TRUNCATE);
		}

		// 设完再问一遍，别信返回值。
		if (!GetWindowDisplayAffinity(g_pane[PaneExcluded], &got) || got == WDA_NONE)
		{
			wchar_t tail[96] = {};
			_snwprintf_s(tail, _countof(tail), _TRUNCATE,
				L"；X 那一路没设上系统排除（错误 %lu）", GetLastError());
			wcsncat_s(g_initNote, _countof(g_initNote), tail, _TRUNCATE);
		}
	}

	g_visible = true;
	return true;
}

void VideoWallDestroy()
{
	for (int i = 0; i < PaneCount; ++i)
	{
		if (g_pane[i])
		{
			DestroyWindow(g_pane[i]);
			g_pane[i] = nullptr;
		}
		g_ok[i] = false;
	}
	if (g_strip)
	{
		DestroyWindow(g_strip);
		g_strip = nullptr;
	}

	ReleaseD3D();

	if (g_sceneBmp) { DeleteObject(g_sceneBmp); g_sceneBmp = nullptr; }
	if (g_sceneDc) { DeleteDC(g_sceneDc); g_sceneDc = nullptr; }
	g_sceneBits = nullptr;
	g_parent = nullptr;
	g_visible = false;
}

bool VideoWallAlive()
{
	return g_strip != nullptr && g_visible;
}

void VideoWallSetRect(const RECT& clientRect)
{
	if (!g_strip) return;

	const int w = clientRect.right - clientRect.left;
	const int h = clientRect.bottom - clientRect.top;
	if (w < VideoWallStripWidth() || h < kVideoPaneH)
	{
		g_visible = false;
		ShowWindow(g_strip, SW_HIDE);
		for (int i = 0; i < PaneCount; ++i)
		{
			if (IsTopLevelPane(i) && g_pane[i]) ShowWindow(g_pane[i], SW_HIDE);
		}
		return;
	}

	SetWindowPos(g_strip, nullptr, clientRect.left, clientRect.top,
		VideoWallStripWidth(), kVideoPaneH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	g_visible = true;

	POINT origin = { clientRect.left, clientRect.top };
	ClientToScreen(g_parent, &origin);

	for (int i = 0; i < PaneCount; ++i)
	{
		if (!g_ok[i]) continue;
		const int x = i * (kVideoPaneW + kVideoGap);
		if (IsTopLevelPane(i))
		{
			SetWindowPos(g_pane[i], HWND_TOP, origin.x + x, origin.y,
				kVideoPaneW, kVideoPaneH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			if (i == PaneLayered) UpdateLayered();
			else
			{
				// 顶层窗没有父窗替它重画，挪完自己刷一次。
				HDC dc = GetDC(g_pane[i]);
				if (dc) { BlitScene(dc); ReleaseDC(g_pane[i], dc); }
			}
		}
		else
		{
			SetWindowPos(g_pane[i], nullptr, x, 0, kVideoPaneW, kVideoPaneH,
				SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
	}
}

RECT VideoWallClientRect()
{
	RECT r = {};
	if (g_strip) GetWindowRect(g_strip, &r);
	return r;
}

const wchar_t* VideoWallInitNote()
{
	return g_initNote;
}

void VideoWallTick()
{
	Tick();
}

int VideoWallFrame()
{
	return (int)g_frame;
}

void VideoWallPump(unsigned ms)
{
	const ULONGLONG end = GetTickCount64() + ms;
	for (;;)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (GetTickCount64() >= end) break;
		Sleep(4);
	}
}

void VideoWallAnalyze(const Image32& img, const RECT& screenRect, int frame, VideoWallReport& out)
{
	out = VideoWallReport();

	const int sw = (int)(screenRect.right - screenRect.left);
	const int sh = (int)(screenRect.bottom - screenRect.top);
	if (!img.Valid() || !g_visible || sw <= 0 || sh <= 0) return;

	out.valid = true;

	for (int i = 0; i < PaneCount; ++i)
	{
		out.marks[i] = L'-';
		if (!g_ok[i]) { out.marks[i] = L'?'; continue; }

		const RECT pr = PaneScreenRect(i);
		// 屏幕坐标 → 图像坐标。整屏是 1:1，PrintWindow 抓整窗时走比例映射。
		const int l = (int)((INT64)(pr.left - screenRect.left) * img.width / sw);
		const int t = (int)((INT64)(pr.top - screenRect.top) * img.height / sh);
		const int r = (int)((INT64)(pr.right - screenRect.left) * img.width / sw);
		const int b = (int)((INT64)(pr.bottom - screenRect.top) * img.height / sh);
		if (l < 0 || t < 0 || r > img.width || b > img.height) continue;   // 这张图没覆盖到

		++out.covered;

		const int cw = r - l;
		const int ch = b - t;
		if (cw < 16 || ch < 16) { out.marks[i] = L'?'; continue; }

		Image32 crop;
		crop.Alloc(cw, ch);
		for (int y = 0; y < ch; ++y)
			memcpy(crop.Row(y), img.Row(t + y) + (size_t)l * 4, (size_t)cw * 4);

		long long luma = 0;
		long long count = 0;
		for (int y = 0; y < ch; y += 2)
		{
			const uint8_t* row = crop.Row(y);
			for (int x = 0; x < cw; x += 2)
			{
				const uint8_t* p = row + (size_t)x * 4;
				luma += ((int)p[2] * 77 + (int)p[1] * 150 + (int)p[0] * 29) >> 8;
				++count;
			}
		}
		if (count && luma / count <= 8)
		{
			out.marks[i] = L'\u2715';    // 整块黑：这条路看不见它
			continue;
		}

		const double bar = LocateBar(crop);
		if (bar >= 0.0 && BarMatches(bar, (unsigned)frame))
		{
			out.marks[i] = L'\u2714';
			++out.live;
		}
		else
		{
			out.marks[i] = L'?';         // 有画面，但不是这一帧
		}
	}
}

void VideoWallReportText(const VideoWallReport& r, wchar_t* buf, size_t cch)
{
	if (!buf || !cch) return;
	if (!r.valid)
	{
		wcsncpy_s(buf, cch, L"视频 未覆盖", _TRUNCATE);
		return;
	}
	_snwprintf_s(buf, cch, _TRUNCATE, L"视频 G%c D%c L%c X%c · %d/%d 本帧",
		r.marks[0], r.marks[1], r.marks[2], r.marks[3], r.live, r.covered);
}
