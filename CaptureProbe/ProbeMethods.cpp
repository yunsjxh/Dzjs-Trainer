#include "ProbeMethods.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const MethodDesc kMethods[] =
{
	{ L"BitBlt 整屏",            L"GetDC(NULL) → BitBlt(SRCCOPY)",            true,  false },
	{ L"BitBlt + CAPTUREBLT",    L"GetDC(NULL) → BitBlt(SRCCOPY|CAPTUREBLT)",  true,  false },
	{ L"StretchBlt 整屏",        L"GetDC(NULL) → StretchBlt(1:1)",             true,  false },
	{ L"GetDIBits 全高",         L"GetDIBits(hdcScreen, 32bpp, 全部行)",       true,  false },
	{ L"GetDIBits 上半屏",       L"GetDIBits(hdcScreen, 32bpp, 只请求一半行)",  true,  false },
	{ L"GetDIBits 内存 DC",      L"GetDIBits(hdcMem, 24bpp)  ← 对照组",        true,  false },
	{ L"GetDIBits 16bpp",        L"GetDIBits(hdcScreen, 16bpp 565)",           true,  false },
	{ L"GetWindowDC(桌面)",      L"GetDesktopWindow → GetWindowDC → BitBlt",   true,  false },
	{ L"CreateDCW(DISPLAY)",     L"CreateDCW(L\"DISPLAY\") → BitBlt",          true,  false },
	{ L"PrintWindow",            L"PrintWindow(hwnd, 0)",                      false, false },
	{ L"PrintWindow FULL",       L"PrintWindow(hwnd, PW_RENDERFULLCONTENT)",   false, false },
	{ L"DXGI 桌面复制",          L"IDXGIOutputDuplication::AcquireNextFrame",  false, false },
	{ L"自检 · 内存 BitBlt",     L"内存 DC → 内存 DC，不该被改写",             false, true  },
	{ L"自检 · 内存 Stretch",    L"内存 DC → 内存 DC，不该被改写",             false, true  },
};

const size_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);

// 自检块的边长。256 足够看出涂改，也不会把预览撑爆。
const int kSelfTestSize = 256;

void SetNote(ProbeResult& r, const wchar_t* fmt, ...)
{
	// 已有内容就接在后面，方便"桌面被替换 + BitBlt 失败"这种复合原因。
	const size_t used = wcslen(r.note);
	wchar_t* tail = r.note + used;
	size_t cap = _countof(r.note) - used;
	if (cap <= 1) return;
	if (used)
	{
		tail[0] = L'；';
		++tail;
		--cap;
		if (cap <= 1) return;
	}
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(tail, cap, _TRUNCATE, fmt, ap);
	va_end(ap);
}

BITMAPINFO MakeBi(int w, int h, WORD bitCount = 32)
{
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   // 顶向下，与 Image32 一致
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = bitCount;
	bi.bmiHeader.biCompression = BI_RGB;
	return bi;
}

bool CreateDib32(int w, int h, HBITMAP& bmp, void*& bits)
{
	const BITMAPINFO bi = MakeBi(w, h, 32);
	bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	return bmp != nullptr && bits != nullptr;
}

// 把 DDB 读回成 Image32。readDc 必须是**内存 DC** —— 传屏幕 DC 会被抓屏钩子
// 拦下来，那样测的就不是 BitBlt 这一环了。
bool ReadBack(HDC readDc, HBITMAP bmp, int w, int h, Image32& out)
{
	BITMAPINFO bi = MakeBi(w, h, 32);
	out.Alloc(w, h);
	const int lines = GetDIBits(readDc, bmp, 0, (UINT)h, out.pixels.data(), &bi, DIB_RGB_COLORS);
	if (lines != h)
	{
		out.Clear();
		return false;
	}
	return true;
}

void CopyFromBits(const void* bits, int w, int h, Image32& out)
{
	out.Alloc(w, h);
	memcpy(out.pixels.data(), bits, (size_t)w * (size_t)h * 4);
}

// 16bpp 的 BI_RGB 到底是 565 还是 555，各机器不一定一样（实测本机是 555，
// 而文档和大多数资料按 565 写）。判据很简单：555 里 bit15 恒为 0，
// 565 里它是 G 的最高位，正常画面必然会出现 1。先采样再看比例。
void Expand16(const uint8_t* src, int w, int h, int srcStride, Image32& out)
{
	int highBitSet = 0;
	int sampled = 0;
	for (int y = 0; y < h; y += 8)
	{
		const uint8_t* s = src + (size_t)y * (size_t)srcStride;
		for (int x = 0; x < w; x += 8)
		{
			const unsigned v = (unsigned)s[x * 2] | ((unsigned)s[x * 2 + 1] << 8);
			if (v & 0x8000u) ++highBitSet;
			++sampled;
		}
	}
	const bool is565 = sampled > 0 && (highBitSet * 100 > sampled);

	out.Alloc(w, h);
	for (int y = 0; y < h; ++y)
	{
		const uint8_t* s = src + (size_t)y * (size_t)srcStride;
		uint8_t* d = out.Row(y);
		for (int x = 0; x < w; ++x)
		{
			const unsigned v = (unsigned)s[x * 2] | ((unsigned)s[x * 2 + 1] << 8);
			unsigned rr, gg, bb;
			if (is565)
			{
				rr = (v >> 11) & 0x1F;
				gg = (v >> 5) & 0x3F;
				bb = v & 0x1F;
				d[x * 4 + 1] = (uint8_t)((gg << 2) | (gg >> 4));
			}
			else
			{
				rr = (v >> 10) & 0x1F;
				gg = (v >> 5) & 0x1F;
				bb = v & 0x1F;
				d[x * 4 + 1] = (uint8_t)((gg << 3) | (gg >> 2));
			}
			d[x * 4 + 0] = (uint8_t)((bb << 3) | (bb >> 2));
			d[x * 4 + 2] = (uint8_t)((rr << 3) | (rr >> 2));
			d[x * 4 + 3] = 255;
		}
	}
}

// 16bpp 必须显式给 565 掩码。只写 biBitCount=16 + BI_RGB 的话，GetDIBits
// 有可能按老的 5-5-5 布局转换，解出来整幅偏绿 —— 这是实测踩出来的。
struct BitmapInfo565
{
	BITMAPINFOHEADER header;
	DWORD masks[3];
};

bool GetDib16(HDC readDc, HBITMAP bmp, int w, int h, int startRow, int lines,
	std::vector<uint8_t>& buffer, int& stride)
{
	BitmapInfo565 bi = {};
	bi.header.biSize = sizeof(BITMAPINFOHEADER);
	bi.header.biWidth = w;
	bi.header.biHeight = -h;
	bi.header.biPlanes = 1;
	bi.header.biBitCount = 16;
	bi.header.biCompression = BI_BITFIELDS;
	bi.masks[0] = 0xF800;   // R
	bi.masks[1] = 0x07E0;   // G
	bi.masks[2] = 0x001F;   // B

	stride = (int)((((DWORD)w * 16) + 31) / 32 * 4);
	buffer.assign((size_t)stride * (size_t)lines, 0);
	return GetDIBits(readDc, bmp, (UINT)startRow, (UINT)lines, buffer.data(),
		reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS) > 0;
}

void Expand24(const uint8_t* src, int w, int h, int srcStride, bool bottomUp, Image32& out)
{
	out.Alloc(w, h);
	for (int y = 0; y < h; ++y)
	{
		const int srcRow = bottomUp ? (h - 1 - y) : y;
		const uint8_t* s = src + (size_t)srcRow * (size_t)srcStride;
		uint8_t* d = out.Row(y);
		for (int x = 0; x < w; ++x)
		{
			d[x * 4 + 0] = s[x * 3 + 0];
			d[x * 4 + 1] = s[x * 3 + 1];
			d[x * 4 + 2] = s[x * 3 + 2];
			d[x * 4 + 3] = 255;
		}
	}
}

// ---- 1 / 2：最经典的整屏抓取 ------------------------------------------------
void CaptureWithBitBlt(const ProbeContext& ctx, ProbeResult& r, DWORD rop)
{
	const RECT& vs = ctx.virtualScreen;
	const int w = (int)(vs.right - vs.left);
	const int h = (int)(vs.bottom - vs.top);
	if (w <= 0 || h <= 0)
	{
		SetNote(r, L"虚拟屏尺寸异常 %dx%d", w, h);
		return;
	}

	HDC screen = GetDC(nullptr);
	if (!screen)
	{
		SetNote(r, L"GetDC(NULL) 返回 NULL");
		return;
	}

	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(screen, w, h) : nullptr;
	if (!mem || !bmp)
	{
		SetNote(r, L"CreateCompatibleDC/Bitmap 失败");
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		ReleaseDC(nullptr, screen);
		return;
	}

	HGDIOBJ old = SelectObject(mem, bmp);
	const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, (int)vs.left, (int)vs.top, rop);
	if (!ok)
	{
		SetNote(r, L"BitBlt 失败，GetLastError=%lu", GetLastError());
	}
	else if (!ReadBack(mem, bmp, w, h, r.image))
	{
		SetNote(r, L"GetDIBits 读回失败");
	}
	else
	{
		r.screenRect = vs;
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

// ---- 3：StretchBlt ---------------------------------------------------------
void CaptureWithStretchBlt(const ProbeContext& ctx, ProbeResult& r)
{
	const RECT& vs = ctx.virtualScreen;
	const int w = (int)(vs.right - vs.left);
	const int h = (int)(vs.bottom - vs.top);
	if (w <= 0 || h <= 0)
	{
		SetNote(r, L"虚拟屏尺寸异常 %dx%d", w, h);
		return;
	}

	HDC screen = GetDC(nullptr);
	if (!screen)
	{
		SetNote(r, L"GetDC(NULL) 返回 NULL");
		return;
	}

	HDC mem = CreateCompatibleDC(screen);
	HBITMAP dib = nullptr;
	void* bits = nullptr;
	if (!mem || !CreateDib32(w, h, dib, bits))
	{
		SetNote(r, L"准备目标 DIB 失败");
		if (mem) DeleteDC(mem);
		ReleaseDC(nullptr, screen);
		return;
	}

	HGDIOBJ old = SelectObject(mem, dib);
	SetStretchBltMode(mem, COLORONCOLOR);
	const BOOL ok = StretchBlt(mem, 0, 0, w, h, screen, (int)vs.left, (int)vs.top, w, h, SRCCOPY);
	if (!ok)
	{
		SetNote(r, L"StretchBlt 失败，GetLastError=%lu", GetLastError());
	}
	else
	{
		CopyFromBits(bits, w, h, r.image);
		r.screenRect = vs;
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

// ---- 4 / 5 / 6 / 7：GetDIBits 的四种写法 -----------------------------------
// useScreenDc 决定命中不命中钩子：钩子的判据是 WindowFromDC(hdc) == 桌面窗口。
//
// startRow 的语义要当心：源是 DDB，存储顺序是 bottom-up，GetDIBits 的 start
// 从**存储起点**算，所以 start=0 拿到的是屏幕**底部**的行。要上半屏就得
// start = h/2。这个方向和"目标 DIB 写成 top-down"的直觉相反，实测踩过。
void CaptureViaGetDib(const ProbeContext& ctx, ProbeResult& r,
	bool useScreenDc, WORD bitCount, int startRow, int lines)
{
	const RECT& vs = ctx.virtualScreen;
	const int w = (int)(vs.right - vs.left);
	const int h = (int)(vs.bottom - vs.top);
	if (w <= 0 || h <= 0)
	{
		SetNote(r, L"虚拟屏尺寸异常 %dx%d", w, h);
		return;
	}
	if (lines <= 0 || startRow < 0 || startRow + lines > h)
	{
		SetNote(r, L"请求行范围异常 start=%d lines=%d h=%d", startRow, lines, h);
		return;
	}

	HDC screen = GetDC(nullptr);
	if (!screen)
	{
		SetNote(r, L"GetDC(NULL) 返回 NULL");
		return;
	}

	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(screen, w, h) : nullptr;
	if (!mem || !bmp)
	{
		SetNote(r, L"CreateCompatibleDC/Bitmap 失败");
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		ReleaseDC(nullptr, screen);
		return;
	}

	HGDIOBJ old = SelectObject(mem, bmp);
	if (!BitBlt(mem, 0, 0, w, h, screen, (int)vs.left, (int)vs.top, SRCCOPY))
	{
		SetNote(r, L"准备工作位图失败（BitBlt），GetLastError=%lu", GetLastError());
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		ReleaseDC(nullptr, screen);
		return;
	}

	// 这里就是被测点：屏幕 DC 会被钩子拦，内存 DC 不会。
	HDC readDc = useScreenDc ? screen : mem;

	// biHeight 必须是**整张位图**的高度，cLines 才是本次请求的行数。
	std::vector<uint8_t> buffer;
	int stride = 0;
	int got = 0;
	if (bitCount == 16)
	{
		got = GetDib16(readDc, bmp, w, h, startRow, lines, buffer, stride) ? lines : 0;
	}
	else
	{
		BITMAPINFO bi = MakeBi(w, h, bitCount);
		stride = (int)((((DWORD)w * bitCount) + 31) / 32 * 4);
		buffer.assign((size_t)stride * (size_t)lines, 0);
		got = GetDIBits(readDc, bmp, (UINT)startRow, (UINT)lines, buffer.data(), &bi, DIB_RGB_COLORS);
	}

	if (got <= 0)
	{
		SetNote(r, L"GetDIBits 返回 %d，GetLastError=%lu", got, GetLastError());
	}
	else
	{
		if (bitCount == 32)
			CopyFromBits(buffer.data(), w, got, r.image);
		else if (bitCount == 24)
			Expand24(buffer.data(), w, got, stride, false, r.image);
		else
			Expand16(buffer.data(), w, got, stride, r.image);

		r.screenRect = { vs.left, vs.top, vs.right, vs.top + got };
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
}

// ---- 8：GetDesktopWindow + GetWindowDC -------------------------------------
// allowMonitor=false 时钩子会把桌面窗口换成假的，这条路径就是用来暴露它的。
void CaptureViaDesktopWindow(const ProbeContext& ctx, ProbeResult& r)
{
	const RECT& vs = ctx.virtualScreen;
	const int vw = (int)(vs.right - vs.left);
	const int vh = (int)(vs.bottom - vs.top);

	HWND desk = GetDesktopWindow();
	if (!desk)
	{
		SetNote(r, L"GetDesktopWindow 返回 NULL");
		return;
	}

	RECT dr = {};
	GetWindowRect(desk, &dr);
	const int dw = (int)(dr.right - dr.left);
	const int dh = (int)(dr.bottom - dr.top);
	if (dw != vw || dh != vh)
		SetNote(r, L"桌面窗口尺寸 %dx%d（虚拟屏 %dx%d），句柄疑似被替换", dw, dh, vw, vh);

	if (vw <= 0 || vh <= 0)
	{
		SetNote(r, L"虚拟屏尺寸异常");
		return;
	}

	HDC dc = GetWindowDC(desk);
	if (!dc)
	{
		SetNote(r, L"GetWindowDC 返回 NULL");
		return;
	}

	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, vw, vh) : nullptr;
	if (!mem || !bmp)
	{
		SetNote(r, L"CreateCompatibleDC/Bitmap 失败");
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		ReleaseDC(desk, dc);
		return;
	}

	HGDIOBJ old = SelectObject(mem, bmp);
	const BOOL ok = BitBlt(mem, 0, 0, vw, vh, dc, (int)vs.left, (int)vs.top, SRCCOPY);
	if (!ok)
	{
		SetNote(r, L"BitBlt 失败，GetLastError=%lu", GetLastError());
	}
	else if (!ReadBack(mem, bmp, vw, vh, r.image))
	{
		SetNote(r, L"GetDIBits 读回失败");
	}
	else
	{
		r.screenRect = vs;
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(desk, dc);
}

// ---- 9：CreateDCW(L"DISPLAY") ----------------------------------------------
// allowMonitor=false 时钩子直接返回 NULL，抓屏软件会以为显卡坏了。
void CaptureViaCreateDc(const ProbeContext& ctx, ProbeResult& r)
{
	const RECT& vs = ctx.virtualScreen;
	const int w = (int)(vs.right - vs.left);
	const int h = (int)(vs.bottom - vs.top);
	if (w <= 0 || h <= 0)
	{
		SetNote(r, L"虚拟屏尺寸异常");
		return;
	}

	HDC dc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
	if (!dc)
	{
		SetNote(r, L"CreateDCW(L\"DISPLAY\") 返回 NULL，GetLastError=%lu", GetLastError());
		return;
	}

	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
	if (!mem || !bmp)
	{
		SetNote(r, L"CreateCompatibleDC/Bitmap 失败");
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		DeleteDC(dc);
		return;
	}

	HGDIOBJ old = SelectObject(mem, bmp);
	const BOOL ok = BitBlt(mem, 0, 0, w, h, dc, (int)vs.left, (int)vs.top, SRCCOPY);
	if (!ok)
	{
		SetNote(r, L"BitBlt 失败，GetLastError=%lu", GetLastError());
	}
	else if (!ReadBack(mem, bmp, w, h, r.image))
	{
		SetNote(r, L"GetDIBits 读回失败");
	}
	else
	{
		r.screenRect = vs;
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	DeleteDC(dc);
}

// ---- 10 / 11：PrintWindow ---------------------------------------------------
// 注意：钩子对 PrintWindow **不检查目标窗口**，任何窗口被 PrintWindow 都会
// 被贴图。这条在真实软件里可能是误伤（有些程序拿它做窗口缩略图缓存）。
void CaptureViaPrintWindow(const ProbeContext& ctx, ProbeResult& r, UINT flags)
{
	RECT wr = {};
	if (!GetWindowRect(ctx.hwnd, &wr))
	{
		SetNote(r, L"GetWindowRect 失败");
		return;
	}

	const int w = (int)(wr.right - wr.left);
	const int h = (int)(wr.bottom - wr.top);
	if (w <= 0 || h <= 0)
	{
		SetNote(r, L"窗口尺寸异常 %dx%d", w, h);
		return;
	}

	HDC mem = CreateCompatibleDC(nullptr);
	HBITMAP dib = nullptr;
	void* bits = nullptr;
	if (!mem || !CreateDib32(w, h, dib, bits))
	{
		SetNote(r, L"准备目标 DIB 失败");
		if (mem) DeleteDC(mem);
		return;
	}

	HGDIOBJ old = SelectObject(mem, dib);
	const BOOL ok = PrintWindow(ctx.hwnd, mem, flags);
	if (!ok)
	{
		SetNote(r, L"PrintWindow 失败，GetLastError=%lu", GetLastError());
	}
	else
	{
		CopyFromBits(bits, w, h, r.image);
		r.screenRect = wr;
		r.captured = true;
	}

	SelectObject(mem, old);
	DeleteObject(dib);
	DeleteDC(mem);
}

// ---- 12：DXGI Desktop Duplication ------------------------------------------
// 这条路完全不经过 GDI，是所有 GDI 层钩子都够不着的地方。
template <typename T>
struct ComPtr
{
	T* p = nullptr;

	ComPtr() = default;
	ComPtr(const ComPtr&) = delete;
	ComPtr& operator=(const ComPtr&) = delete;
	~ComPtr() { Reset(); }

	T** Put() { Reset(); return &p; }
	void** PutVoid() { Reset(); return reinterpret_cast<void**>(&p); }
	T* Get() const { return p; }
	T* operator->() const { return p; }
	bool Valid() const { return p != nullptr; }
	void Reset() { if (p) { p->Release(); p = nullptr; } }
};

// 逐个输出试。带虚拟显示器（ToDesk、向日葵、Parsec）的机器上，输出 #0
// 经常是那块虚拟屏，DuplicateOutput 能成功但每次都是全零帧；真正的物理屏
// 排在后面。只试 #0 会得出"这台机器不支持 DXGI"的错误结论。
enum class OutputTry { End, Failed, Ok };

OutputTry DxgiTryOutput(ID3D11Device* device, ID3D11DeviceContext* context,
	IDXGIAdapter* adapter, UINT index, Image32& image, RECT& rect,
	wchar_t* why, size_t cch)
{
	ComPtr<IDXGIOutput> output;
	HRESULT hr = adapter->EnumOutputs(index, output.Put());
	if (hr == DXGI_ERROR_NOT_FOUND) return OutputTry::End;
	if (FAILED(hr))
	{
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u EnumOutputs 0x%08X", index, (unsigned)hr);
		return OutputTry::Failed;
	}

	DXGI_OUTPUT_DESC odesc = {};
	output->GetDesc(&odesc);

	ComPtr<IDXGIOutput1> output1;
	hr = output->QueryInterface(__uuidof(IDXGIOutput1), output1.PutVoid());
	if (FAILED(hr))
	{
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u 无 IDXGIOutput1", index);
		return OutputTry::Failed;
	}

	ComPtr<IDXGIOutputDuplication> dupl;
	hr = output1->DuplicateOutput(device, dupl.Put());
	if (FAILED(hr))
	{
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u DuplicateOutput 0x%08X", index, (unsigned)hr);
		return OutputTry::Failed;
	}

	DXGI_OUTDUPL_FRAME_INFO info = {};
	ComPtr<IDXGIResource> resource;
	bool acquired = false;
	for (int attempt = 0; attempt < 3 && !acquired; ++attempt)
	{
		hr = dupl->AcquireNextFrame(attempt == 0 ? 1500 : 800, &info, resource.Put());
		if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
		if (FAILED(hr)) break;
		acquired = true;
	}
	if (!acquired)
	{
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u AcquireNextFrame 0x%08X", index, (unsigned)hr);
		return OutputTry::Failed;
	}

	ComPtr<ID3D11Texture2D> tex;
	hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), tex.PutVoid());
	if (FAILED(hr) || !tex.Valid())
	{
		dupl->ReleaseFrame();
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u 取纹理失败", index);
		return OutputTry::Failed;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);

	D3D11_TEXTURE2D_DESC sdesc = desc;
	sdesc.Usage = D3D11_USAGE_STAGING;
	sdesc.BindFlags = 0;
	sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sdesc.MiscFlags = 0;
	sdesc.MipLevels = 1;
	sdesc.ArraySize = 1;

	ComPtr<ID3D11Texture2D> staging;
	hr = device->CreateTexture2D(&sdesc, nullptr, staging.Put());
	if (FAILED(hr))
	{
		dupl->ReleaseFrame();
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u 建 staging 失败", index);
		return OutputTry::Failed;
	}

	context->CopyResource(staging.Get(), tex.Get());
	dupl->ReleaseFrame();

	D3D11_MAPPED_SUBRESOURCE map = {};
	hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
	if (FAILED(hr))
	{
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u Map 失败", index);
		return OutputTry::Failed;
	}

	image.Alloc((int)desc.Width, (int)desc.Height);
	for (UINT y = 0; y < desc.Height; ++y)
	{
		memcpy(image.Row((int)y),
			(const uint8_t*)map.pData + (size_t)y * (size_t)map.RowPitch,
			(size_t)desc.Width * 4);
	}
	context->Unmap(staging.Get(), 0);
	rect = odesc.DesktopCoordinates;

	bool allZero = true;
	for (int y = 0; y < image.height && allZero; y += 16)
	{
		const uint32_t* row = reinterpret_cast<const uint32_t*>(image.Row(y));
		for (int x = 0; x < image.width; x += 16)
		{
			if (row[x] & 0x00FFFFFFu) { allZero = false; break; }
		}
	}
	if (allZero)
	{
		image.Clear();
		_snwprintf_s(why, cch, _TRUNCATE, L"#%u 全零帧（虚拟屏）", index);
		return OutputTry::Failed;
	}

	return OutputTry::Ok;
}

void CaptureViaDxgi(ProbeResult& r)
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
		device.Put(), &level, context.Put());
	if (FAILED(hr))
	{
		SetNote(r, L"D3D11CreateDevice 失败 0x%08X", (unsigned)hr);
		return;
	}

	ComPtr<IDXGIDevice> dxgiDevice;
	hr = device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.PutVoid());
	if (FAILED(hr))
	{
		SetNote(r, L"取 IDXGIDevice 失败 0x%08X", (unsigned)hr);
		return;
	}

	ComPtr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetAdapter(adapter.Put());
	if (FAILED(hr))
	{
		SetNote(r, L"取 IDXGIAdapter 失败 0x%08X", (unsigned)hr);
		return;
	}

	wchar_t collected[512] = {};
	for (UINT i = 0; i < 16; ++i)
	{
		wchar_t why[160] = {};
		Image32 image;
		RECT rect = {};
		const OutputTry result = DxgiTryOutput(device.Get(), context.Get(), adapter.Get(),
			i, image, rect, why, _countof(why));

		if (result == OutputTry::End) break;
		if (result == OutputTry::Ok)
		{
			r.image = image;
			r.screenRect = rect;
			r.captured = true;
			if (i > 0)
				SetNote(r, L"输出 #%u 抓到画面（排在前面的输出拿不到帧）", i);
			return;
		}

		_snwprintf_s(collected, _countof(collected), _TRUNCATE, L"%s%s%s",
			collected, collected[0] ? L"；" : L"", why);
	}

	if (collected[0])
		SetNote(r, L"所有输出都不可用：%s", collected);
	else
		SetNote(r, L"没有可用的 DXGI 输出");
}

// ---- 13 / 14：误伤自检 ------------------------------------------------------
// 源和目标都是内存 DC。抓屏钩子如果在这里动手，说明它的"屏幕 DC"判据坏了，
// 真实软件里就会把图标、字体、离屏渲染一起涂掉。
void RunSelfTestMemoryDc(ProbeResult& r, bool useStretch)
{
	r.selfTest = true;

	HBITMAP srcBmp = nullptr;
	void* srcBits = nullptr;
	HBITMAP dstBmp = nullptr;
	void* dstBits = nullptr;
	if (!CreateDib32(kSelfTestSize, kSelfTestSize, srcBmp, srcBits)
		|| !CreateDib32(kSelfTestSize, kSelfTestSize, dstBmp, dstBits))
	{
		SetNote(r, L"CreateDIBSection 失败");
		if (srcBmp) DeleteObject(srcBmp);
		if (dstBmp) DeleteObject(dstBmp);
		return;
	}

	// 源：纯绿。目标：纯黑，跑完只该变成绿。
	uint8_t* s = (uint8_t*)srcBits;
	for (int i = 0; i < kSelfTestSize * kSelfTestSize; ++i)
	{
		s[i * 4 + 0] = 0;
		s[i * 4 + 1] = 255;
		s[i * 4 + 2] = 0;
		s[i * 4 + 3] = 255;
	}
	memset(dstBits, 0, (size_t)kSelfTestSize * kSelfTestSize * 4);

	HDC srcDc = CreateCompatibleDC(nullptr);
	HDC dstDc = CreateCompatibleDC(nullptr);
	HGDIOBJ oldSrc = SelectObject(srcDc, srcBmp);
	HGDIOBJ oldDst = SelectObject(dstDc, dstBmp);

	BOOL ok;
	if (useStretch)
	{
		SetStretchBltMode(dstDc, COLORONCOLOR);
		ok = StretchBlt(dstDc, 0, 0, kSelfTestSize, kSelfTestSize,
			srcDc, 0, 0, kSelfTestSize, kSelfTestSize, SRCCOPY);
	}
	else
	{
		ok = BitBlt(dstDc, 0, 0, kSelfTestSize, kSelfTestSize, srcDc, 0, 0, SRCCOPY);
	}

	const uint8_t* d = (const uint8_t*)dstBits;
	int green = 0;
	int total = 0;
	for (int y = 0; y < kSelfTestSize; y += 8)
	{
		for (int x = 0; x < kSelfTestSize; x += 8)
		{
			const uint8_t* p = d + ((size_t)y * kSelfTestSize + x) * 4;
			++total;
			if (p[0] < 64 && p[1] > 200 && p[2] < 64) ++green;
		}
	}

	r.selfTestOk = (ok != FALSE) && total > 0 && green == total;
	if (!ok)
		SetNote(r, L"拷贝调用失败，GetLastError=%lu", GetLastError());
	else if (green != total)
		SetNote(r, L"内存 DC 被改写：%d/%d 采样点不再是纯绿", total - green, total);
	else
		SetNote(r, L"内存 DC 未被触碰");

	// 把结果留一份给预览：绿块 = 没被改写。
	r.image.Alloc(kSelfTestSize, kSelfTestSize);
	memcpy(r.image.pixels.data(), dstBits, (size_t)kSelfTestSize * kSelfTestSize * 4);
	r.screenRect = { 0, 0, kSelfTestSize, kSelfTestSize };
	r.captured = true;

	SelectObject(srcDc, oldSrc);
	SelectObject(dstDc, oldDst);
	DeleteDC(srcDc);
	DeleteDC(dstDc);
	DeleteObject(srcBmp);
	DeleteObject(dstBmp);
}

} // namespace

int MethodCount()
{
	return (int)kMethodCount;
}

const MethodDesc& MethodAt(int index)
{
	if (index < 0 || index >= (int)kMethodCount) index = 0;
	return kMethods[index];
}

void RunMethod(int index, const ProbeContext& ctx, ProbeResult& out)
{
	out = ProbeResult();
	const ULONGLONG started = GetTickCount64();
	const int vh = (int)(ctx.virtualScreen.bottom - ctx.virtualScreen.top);

	switch ((MethodId)index)
	{
	case MethodId::BitBltScreen:
		CaptureWithBitBlt(ctx, out, SRCCOPY);
		break;
	case MethodId::BitBltCaptureBlt:
		CaptureWithBitBlt(ctx, out, SRCCOPY | CAPTUREBLT);
		break;
	case MethodId::StretchBltScreen:
		CaptureWithStretchBlt(ctx, out);
		break;
	case MethodId::GetDibScreenFull:
		CaptureViaGetDib(ctx, out, true, 32, 0, vh);
		break;
	case MethodId::GetDibScreenPartial:
		CaptureViaGetDib(ctx, out, true, 32, vh / 2, vh / 2);
		break;
	case MethodId::GetDibMemDc:
		CaptureViaGetDib(ctx, out, false, 24, 0, vh);
		break;
	case MethodId::GetDib16bpp:
		CaptureViaGetDib(ctx, out, true, 16, 0, vh);
		break;
	case MethodId::DesktopWindowDc:
		CaptureViaDesktopWindow(ctx, out);
		break;
	case MethodId::CreateDcDisplay:
		CaptureViaCreateDc(ctx, out);
		break;
	case MethodId::PrintWindowPlain:
		CaptureViaPrintWindow(ctx, out, 0);
		break;
	case MethodId::PrintWindowFull:
		CaptureViaPrintWindow(ctx, out, PW_RENDERFULLCONTENT);
		break;
	case MethodId::DxgiDuplication:
		CaptureViaDxgi(out);
		break;
	case MethodId::SelfTestMemBitBlt:
		RunSelfTestMemoryDc(out, false);
		break;
	case MethodId::SelfTestMemStretch:
		RunSelfTestMemoryDc(out, true);
		break;
	default:
		SetNote(out, L"未知的方法序号 %d", index);
		break;
	}

	// DIB 读回、DXGI 复制出来的图不一定带有效 alpha。统一置不透明，
	// 否则 PNG 存出来是透明的，看图的人会当成白屏。
	if (out.image.Valid())
	{
		uint8_t* p = out.image.pixels.data();
		const size_t n = (size_t)out.image.width * (size_t)out.image.height;
		for (size_t i = 0; i < n; ++i)
			p[i * 4 + 3] = 255;
	}

	out.elapsedMs = (DWORD)(GetTickCount64() - started);
}
