#pragma once
//
// ReplaceCodec.h —— 视频流/截屏「替换」的**纯函数层**。
//
// 设计参考 D:\用户态杀软（R3ShieldCore）的 `*_spoof_codec.h`，取三条：
//   ① 只回答「这一帧该变成什么」与「往哪些字节填什么」，**不进任何全局状态**；
//   ② **长度一个都不改** —— 只写 [0, len)，一个字节都不多写；
//   ③ 造不出合法替代物时**返回 false**，由调用方决定回落，绝不"随便填点什么"。
//
// ★★ 为什么单独成头文件（而不是留在 DzjsTrainerHooks.cpp 里）：
//   同一份逻辑要被**四条**捕获路径共用 ——
//     · EncodeToJPEGBuffer（缩略预览，bottom-up BGR）
//     · EncodeToJPEGBufferI422（另一种色度采样）
//     · LibDeskMonitor / libTDDesk2 的 GDI 抓屏（顶向下 DIB）
//     · DispFilter 的 DispDXGIBitBlt（RGBA，原地 DWORD 填充）
//   原先每个调用点各写一份填充/搬运，参数（行序、通道序、颜色）散在四处，
//   改一处必漏三处。收进来之后"填什么"只有一处定义。
//
// ⚠️ 本文件**不做**判定策略（不读 ini、不看开关、不打日志、不碰 DC）——
//   那是调用方（DzjsTrainerHooks.cpp）的事。这里没有全局变量、没有临界区。
//
#ifndef _WINDOWS_
#include <windows.h>
#endif
#include <string.h>

namespace ReplaceCodec
{
	// ------------------------------------------------------------------
	// 一、帧替换决策
	// ------------------------------------------------------------------
	//
	// 决策输入刻意只保留三个布尔 —— 这是四条捕获路径**都**必须回答的问题。
	// 任何额外条件（尺寸、缓冲区可用性）都属于"能不能做"，由 Validate 回答，
	// 不塞进决策里：混在一起会让"为什么这一帧没被替换"无从判断。
	struct Request
	{
		bool allowMonitor = true;   // 允许教师看屏
		bool videoModify = false;   // 启用视频流替换
		bool imageReady = false;    // 替换图/替换视频帧已就绪
	};

	enum class Action
	{
		Passthrough = 0,  // 原样透传（不碰调用方缓冲）
		OverlayImage,     // 用替换图缩放到目标尺寸覆盖
		FillSolid,        // 整块填充单一值
	};

	struct Plan
	{
		Action action = Action::Passthrough;
		BYTE solidValue = 0;   // 仅 FillSolid 有效
	};

	// JPEG 预览/编码路径的决策。
	//
	// ★ 颜色口径（照搬原实现，别"统一成黑"）：
	//     · 禁止监看（!allowMonitor）而没有替换图  ⇒ **白**（0xFF）
	//     · 启用替换但允许监看（videoModify）      ⇒ **黑**（0x00）
	//   这两条对应两种不同语义：前者是"让教师看到一片空白"，后者是"把画面盖掉"。
	//   合并成一种颜色会改变用户看到的效果 —— 那是行为变更，不是重构。
	inline Plan PlanJpegFrame(const Request& r) noexcept
	{
		Plan plan;
		if (r.allowMonitor && !r.videoModify)
		{
			return plan;   // Passthrough
		}
		if (r.videoModify && r.imageReady)
		{
			plan.action = Action::OverlayImage;
			return plan;
		}
		plan.action = Action::FillSolid;
		plan.solidValue = r.allowMonitor ? 0x00 : 0xFF;
		return plan;
	}

	// I422 编码路径的决策。
	//
	// ★ 与 PlanJpegFrame 的差异是**故意的**：这条路径只做纯色填充，
	//   不支持替换图（原实现也如此）。共用 PlanJpegFrame 会让"有图时覆盖"
	//   悄悄生效在一条从未验证过的路径上。
	inline Plan PlanI422Frame(const Request& r) noexcept
	{
		Plan plan;
		if (r.allowMonitor && !r.videoModify)
		{
			return plan;   // Passthrough
		}
		plan.action = Action::FillSolid;
		plan.solidValue = 0x00;   // I422 的 Y=0 即黑（RGB 全 0 亦黑）
		return plan;
	}

	// ------------------------------------------------------------------
	// 二、尺寸/步长校验（失败方向安全：一律"不能用"）
	// ------------------------------------------------------------------
	// C++14 兼容：constexpr 在命名空间作用域自带内部链接，不写 inline（C++17 才有内联变量）。
	constexpr int kMaxDimension = 16384;

	// RGB24 紧凑缓冲（stride >= width*3）。
	inline bool ValidateRgb24(int width, int height, int stride) noexcept
	{
		if (width <= 0 || height <= 0 || stride <= 0)
		{
			return false;
		}
		if (width > kMaxDimension || height > kMaxDimension)
		{
			return false;
		}
		if (static_cast<size_t>(stride) < static_cast<size_t>(width) * 3u)
		{
			return false;
		}
		// 乘积必须能装进 size_t，否则后面算长度会回绕。
		if (static_cast<size_t>(height) > SIZE_MAX / static_cast<size_t>(stride))
		{
			return false;
		}
		return true;
	}

	// DIB 行对齐：4 字节边界（BITMAPINFOHEADER 的 24bpp 约定）。
	inline int DibStride(int width) noexcept
	{
		return (width * 3 + 3) & ~3;
	}

	// ------------------------------------------------------------------
	// 三、填充（★ 只写 [0, len)，长度一个都不改）
	// ------------------------------------------------------------------
	inline void ApplySolid(BYTE* p, size_t len, BYTE value) noexcept
	{
		if (!p || len == 0)
		{
			return;
		}
		memset(p, value, len);
	}

	// DispFilter 的 RGBA 缓冲：三色字节恒 0、alpha 强制 0xFF。
	//
	// ★ 为什么按 DWORD 而不是按字节填 0xFF000000：三种字节序（RGBA/BGRA/ARGB）
	//   下"三色为 0 + alpha 不透明"的 DWORD 值都是 0xFF000000 —— 逐 DWORD 写
	//   与布局无关。逐字节写死 B,G,R,A 的顺序就只能对上一种布局。
	inline void ApplyOpaqueBlackRgba(DWORD* px, size_t count) noexcept
	{
		if (!px || count == 0)
		{
			return;
		}
		constexpr DWORD kOpaqueBlack = 0xFF000000u;
		for (size_t i = 0; i < count; ++i)
		{
			px[i] = kOpaqueBlack;
		}
	}

	// ------------------------------------------------------------------
	// 四、缩放搬运（带行序/通道序参数）
	// ------------------------------------------------------------------
	//
	// 三处调用点要的形状各不相同：
	//   JPEG 预览：源是顶向下 RGB24，目标是**自下而上**的 BGR24 —— 要同时翻转行序 + 换通道；
	//   DIB 缓存：  源与目标都是顶向下 BGR24 —— 只换通道；
	//   未来若有用例要原样搬 —— 两个开关都关掉。
	// 与其写三份循环，不如把这两件事做成参数。
	//
	// ⚠️ 最近邻缩放（(x*sw)/dw）**不是**为了画质，而是为了与既有实现逐像素一致：
	//   换成线性插值会让"替换图边缘"出现原实现没有的中间色。
	struct BlitParams
	{
		int dstWidth = 0;
		int dstHeight = 0;
		int dstStride = 0;
		int srcWidth = 0;
		int srcHeight = 0;
		bool swapRB = true;    // 目标按 BGR 排列
		bool flipVertical = true;  // 目标行序自下而上
	};

	inline bool BlitScaled(BYTE* dst, const BYTE* src, const BlitParams& p) noexcept
	{
		if (!dst || !src)
		{
			return false;
		}
		if (p.dstWidth <= 0 || p.dstHeight <= 0 || p.srcWidth <= 0 || p.srcHeight <= 0)
		{
			return false;
		}
		if (p.dstStride < p.dstWidth * 3)
		{
			return false;
		}

		for (int y = 0; y < p.dstHeight; ++y)
		{
			// 行映射：翻转时目标第 0 行取源的最后一行（原实现的 (dh-1-y)*sh/dh）。
			int sy = p.flipVertical
				? ((p.dstHeight - 1 - y) * p.srcHeight) / p.dstHeight
				: (y * p.srcHeight) / p.dstHeight;
			if (sy >= p.srcHeight)
			{
				sy = p.srcHeight - 1;
			}
			if (sy < 0)
			{
				sy = 0;
			}
			const BYTE* srow = src + static_cast<size_t>(sy) * p.srcWidth * 3;
			BYTE* drow = dst + static_cast<size_t>(y) * p.dstStride;

			for (int x = 0; x < p.dstWidth; ++x)
			{
				int sx = (x * p.srcWidth) / p.dstWidth;
				if (sx >= p.srcWidth)
				{
					sx = p.srcWidth - 1;
				}
				if (sx < 0)
				{
					sx = 0;
				}
				const BYTE* s = srow + static_cast<size_t>(sx) * 3;
				BYTE* d = drow + static_cast<size_t>(x) * 3;
				if (p.swapRB)
				{
					d[0] = s[2];
					d[1] = s[1];
					d[2] = s[0];
				}
				else
				{
					d[0] = s[0];
					d[1] = s[1];
					d[2] = s[2];
				}
			}
		}
		return true;
	}

	// ------------------------------------------------------------------
	// 五、DIB 绘制（把 HBITMAP 之外的紧凑 DIB 缓冲铺到目标 DC）
	// ------------------------------------------------------------------
	//
	// ★ 与 R3ShieldCore 的 `PaintDib` 同一取舍：目标矩形与源图尺寸不一致时
	//   **拉伸铺满**，而不是平铺或裁切 —— "整块区域都被换成这张图"才是用户直觉。
	// ★ `COLORONCOLOR` 而非 `HALFTONE`：调用方是捕获热路径，HALFTONE 的
	//   抖动计算在 1080p 上有可测开销，而原实现用的就是 COLORONCOLOR ——
	//   换算法等于换观感，属于行为变更。
	// ★ 用完**必须恢复** stretch mode：目标 DC 是调用者的（可能是别人的内存 DC），
	//   改了不还原的症状是"某程序截图之后界面变色"，极难归因。
	inline bool PaintDib(HDC dc, int x, int y, int width, int height,
		const BYTE* bgrData, int srcWidth, int srcHeight, int srcStride) noexcept
	{
		if (!dc || !bgrData || width <= 0 || height <= 0)
		{
			return false;
		}
		if (srcWidth <= 0 || srcHeight <= 0 || srcStride < srcWidth * 3)
		{
			return false;
		}

		BITMAPINFO info;
		ZeroMemory(&info, sizeof(info));
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = srcWidth;
		info.bmiHeader.biHeight = -srcHeight;   // 负值 = 顶向下，与调用方缓冲一致
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 24;
		info.bmiHeader.biCompression = BI_RGB;

		const int oldMode = SetStretchBltMode(dc, COLORONCOLOR);
		const int scanLines = StretchDIBits(dc, x, y, width, height,
			0, 0, srcWidth, srcHeight,
			bgrData, &info, DIB_RGB_COLORS, SRCCOPY);
		if (oldMode != 0)
		{
			SetStretchBltMode(dc, oldMode);
		}
		return scanLines != 0 && scanLines != GDI_ERROR;
	}

	// 把 RGB24 紧凑缓冲转成 BGR24 的 DIB 缓冲（顶向下、行按 4 字节对齐）。
	//
	// ★ 目标缓冲由调用方提供并保证至少 `DibStride(w) * h` 字节；
	//   本函数返回实际需要的字节数，0 表示参数不可用（调用方不该分配）。
	inline size_t Rgb24ToDib(const BYTE* src, int width, int height, BYTE* dst, int dstStride) noexcept
	{
		if (!src || !dst || width <= 0 || height <= 0)
		{
			return 0;
		}
		if (dstStride < DibStride(width))
		{
			return 0;
		}
		for (int y = 0; y < height; ++y)
		{
			const BYTE* s = src + static_cast<size_t>(y) * width * 3;
			BYTE* d = dst + static_cast<size_t>(y) * dstStride;
			for (int x = 0; x < width; ++x)
			{
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				s += 3;
				d += 3;
			}
		}
		return static_cast<size_t>(dstStride) * height;
	}
}
