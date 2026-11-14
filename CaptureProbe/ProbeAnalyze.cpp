#include "ProbeAnalyze.h"
#include <math.h>
#include <stdio.h>

namespace {

// 靶标是 255,0,255 的纯品红。放宽到"红蓝都还很亮、绿被压死"，
// 保证 PNG 有损压缩、色域转换、缩放采样之后仍然认得出。
inline bool IsTargetColor(const uint8_t* bgra)
{
	return bgra[2] >= 190 && bgra[0] >= 190 && bgra[1] <= 95;
}

inline int Luma(const uint8_t* bgra)
{
	return (bgra[2] * 77 + bgra[1] * 150 + bgra[0] * 29) >> 8;
}

void ScanStats(const Image32& img, int& outMin, int& outMax, int& outMean)
{
	const int stepX = img.width > 160 ? img.width / 160 : 1;
	const int stepY = img.height > 160 ? img.height / 160 : 1;
	int mn = 255, mx = 0;
	long long sum = 0;
	long long count = 0;
	for (int y = 0; y < img.height; y += stepY)
	{
		const uint8_t* row = img.Row(y);
		for (int x = 0; x < img.width; x += stepX)
		{
			const int l = Luma(row + (size_t)x * 4);
			if (l < mn) mn = l;
			if (l > mx) mx = l;
			sum += l;
			++count;
		}
	}
	outMin = count ? mn : 0;
	outMax = count ? mx : 0;
	outMean = count ? (int)(sum / count) : 0;
}

// 16×16 网格平均色。比对"是不是同一张图"足够，也不怕尺寸不一样。
void Fingerprint16(const Image32& img, float* out)
{
	for (int gy = 0; gy < 16; ++gy)
	{
		const int y0 = (int)((INT64)gy * img.height / 16);
		int y1 = (int)((INT64)(gy + 1) * img.height / 16);
		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > img.height) y1 = img.height;

		for (int gx = 0; gx < 16; ++gx)
		{
			const int x0 = (int)((INT64)gx * img.width / 16);
			int x1 = (int)((INT64)(gx + 1) * img.width / 16);
			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > img.width) x1 = img.width;

			const int stepX = (x1 - x0) > 8 ? (x1 - x0) / 8 : 1;
			const int stepY = (y1 - y0) > 8 ? (y1 - y0) / 8 : 1;

			long long sb = 0, sg = 0, sr = 0;
			long long n = 0;
			for (int y = y0; y < y1; y += stepY)
			{
				const uint8_t* row = img.Row(y);
				for (int x = x0; x < x1; x += stepX)
				{
					const uint8_t* p = row + (size_t)x * 4;
					sb += p[0]; sg += p[1]; sr += p[2];
					++n;
				}
			}
			if (!n) n = 1;
			const size_t base = ((size_t)gy * 16 + gx) * 3;
			out[base + 0] = (float)((double)sb / (double)n);
			out[base + 1] = (float)((double)sg / (double)n);
			out[base + 2] = (float)((double)sr / (double)n);
		}
	}
}

} // namespace

const wchar_t* VerdictText(Verdict v)
{
	switch (v)
	{
	case Verdict::Pass:          return L"PASS · 靶标完整";
	case Verdict::ContentOnly:   return L"PASS · 有画面（位深不支持判靶）";
	case Verdict::ReplacedImage: return L"REPLACED · 命中替换图";
	case Verdict::ReplacedOther: return L"REPLACED · 画面已被换掉";
	case Verdict::SolidWhite:    return L"SOLID · 整幅纯白";
	case Verdict::SolidBlack:    return L"SOLID · 整幅纯黑";
	case Verdict::Blocked:       return L"BLOCKED · 靶窗被遮挡";
	case Verdict::Failed:        return L"FAILED · 没抓到画面";
	default:                     return L"未运行";
	}
}

COLORREF VerdictColor(Verdict v)
{
	switch (v)
	{
	case Verdict::Pass:          return RGB(0x3D, 0xDC, 0x84);
	case Verdict::ContentOnly:   return RGB(0x6F, 0xD0, 0x9A);
	case Verdict::ReplacedImage: return RGB(0xFF, 0x5C, 0x5C);
	case Verdict::ReplacedOther: return RGB(0xFF, 0x7A, 0x45);
	case Verdict::SolidWhite:    return RGB(0xFF, 0xB0, 0x20);
	case Verdict::SolidBlack:    return RGB(0xFF, 0xB0, 0x20);
	case Verdict::Blocked:       return RGB(0x8A, 0x94, 0xA6);
	case Verdict::Failed:        return RGB(0x8A, 0x94, 0xA6);
	default:                     return RGB(0x5C, 0x66, 0x78);
	}
}

const wchar_t* AnalyzeResult::Text() const
{
	return VerdictText(verdict);
}

COLORREF AnalyzeResult::Color() const
{
	return VerdictColor(verdict);
}

void AnalyzeResult::Detail(wchar_t* buf, size_t cch) const
{
	if (!buf || !cch) return;
	if (expectDistance >= 0.0)
	{
		_snwprintf_s(buf, cch, _TRUNCATE,
			L"靶色 %d%% · 亮度 %d..%d · 指纹距离 %.1f",
			(int)(targetRatio * 100.0 + 0.5), minLuma, maxLuma, expectDistance);
	}
	else
	{
		_snwprintf_s(buf, cch, _TRUNCATE,
			L"靶色 %d%% · 亮度 %d..%d",
			(int)(targetRatio * 100.0 + 0.5), minLuma, maxLuma);
	}
}

double FingerprintDistance(const Image32& a, const Image32& b)
{
	if (!a.Valid() || !b.Valid()) return -1.0;

	float fa[16 * 16 * 3] = {};
	float fb[16 * 16 * 3] = {};
	Fingerprint16(a, fa);
	Fingerprint16(b, fb);

	double sum = 0.0;
	for (int i = 0; i < 16 * 16 * 3; ++i)
		sum += fabs((double)fa[i] - (double)fb[i]);
	return sum / (16.0 * 16.0 * 3.0);
}

AnalyzeResult AnalyzeCapture(const AnalyzeInput& in)
{
	AnalyzeResult r;
	const Image32* img = in.image;
	if (!img || !img->Valid())
	{
		r.verdict = Verdict::Failed;
		return r;
	}

	const int sw = (int)(in.screenRect.right - in.screenRect.left);
	const int sh = (int)(in.screenRect.bottom - in.screenRect.top);
	if (sw <= 0 || sh <= 0)
	{
		r.verdict = Verdict::Failed;
		return r;
	}

	// 屏幕坐标 → 图像坐标。整屏捕获是 1:1，DXGI 缩放、PrintWindow 抓客户区
	// 这类情况会走比例映射，靶标位置一样算得对。
	const int tl = (int)((INT64)(in.targetRect.left - in.screenRect.left) * img->width / sw);
	const int tt = (int)((INT64)(in.targetRect.top - in.screenRect.top) * img->height / sh);
	const int tr = (int)((INT64)(in.targetRect.right - in.screenRect.left) * img->width / sw);
	const int tb = (int)((INT64)(in.targetRect.bottom - in.screenRect.top) * img->height / sh);
	r.targetInImage = { tl, tt, tr, tb };

	const int tw = tr - tl;
	const int th = tb - tt;
	if (tw > 8 && th > 8)
	{
		// 8×8 而不是 7×7：奇数网格的中心点正好落在靶标的黑十字上，
		// 实测只能拿到 73%，看着像"没抓全"；偶数网格的采样点全部避开十字。
		const int cols = 8;
		const int rows = 8;
		int hit = 0;
		int total = 0;
		for (int gy = 0; gy < rows; ++gy)
		{
			const int py = tt + (int)((INT64)th * (2 * gy + 1) / (2 * rows));
			if (py < 0 || py >= img->height) continue;
			const uint8_t* row = img->Row(py);
			for (int gx = 0; gx < cols; ++gx)
			{
				const int px = tl + (int)((INT64)tw * (2 * gx + 1) / (2 * cols));
				if (px < 0 || px >= img->width) continue;
				++total;
				if (IsTargetColor(row + (size_t)px * 4)) ++hit;
			}
		}
		r.targetRatio = total ? (double)hit / (double)total : 0.0;
	}

	if (r.targetRatio >= 0.55)
	{
		r.verdict = Verdict::Pass;
		ScanStats(*img, r.minLuma, r.maxLuma, r.meanLuma);
		return r;
	}

	ScanStats(*img, r.minLuma, r.maxLuma, r.meanLuma);

	// 纯色兜底：整幅图通道差极小才算数，一张很亮的真实画面不该被误判成"纯白"。
	const bool flat = (r.maxLuma - r.minLuma) <= 6;
	if (flat && r.meanLuma >= 246)
	{
		r.verdict = Verdict::SolidWhite;
		return r;
	}
	if (flat && r.meanLuma <= 9)
	{
		r.verdict = Verdict::SolidBlack;
		return r;
	}

	if (in.expect && in.expect->Valid())
	{
		r.expectDistance = FingerprintDistance(*img, *in.expect);
		if (r.expectDistance >= 0.0 && r.expectDistance <= 16.0)
		{
			r.verdict = Verdict::ReplacedImage;
			return r;
		}
	}

	// 16bpp 这类位深，靶标的品红会被色深量化吃掉，不能因为"没看到品红"
	// 就判成替换。钩子对不能贴图的位深只会整块填纯色，所以只要画面还有
	// 结构，就说明没被动过。
	if (in.targetless && (r.maxLuma - r.minLuma) > 24)
	{
		r.verdict = Verdict::ContentOnly;
		return r;
	}

	r.verdict = Verdict::ReplacedOther;
	return r;
}
