#pragma once
// ============================================================================
// ImageIo —— 32bpp BGRA 顶向下图像缓冲，以及 WIC 的 PNG 存 / 通用格式读。
//
// 整个探针内部只用一种像素布局，省掉一堆位深分支：抓回来的 DIB、DXGI 的
// staging texture、WIC 解出来的替换图，全部先落成 Image32 再比较。
// ============================================================================
#include <windows.h>
#include <stdint.h>
#include <vector>

struct Image32 final
{
	int width = 0;
	int height = 0;
	int stride = 0;                 // 字节，恒等于 width * 4
	std::vector<uint8_t> pixels;    // BGRA，行 0 在顶部

	bool Valid() const
	{
		return width > 0 && height > 0
			&& stride >= width * 4
			&& pixels.size() >= (size_t)stride * (size_t)height;
	}

	void Clear()
	{
		width = height = stride = 0;
		pixels.clear();
	}

	void Alloc(int w, int h)
	{
		width = w > 0 ? w : 0;
		height = h > 0 ? h : 0;
		stride = width * 4;
		pixels.assign((size_t)stride * (size_t)height, 0);
	}

	uint8_t* Row(int y) { return pixels.data() + (size_t)y * (size_t)stride; }
	const uint8_t* Row(int y) const { return pixels.data() + (size_t)y * (size_t)stride; }
};

bool ImageIoInit();
void ImageIoShutdown();

// 保存为 PNG（WIC 编码器）。path 的目录必须已存在。
bool ImageSavePng(const wchar_t* path, const Image32& img);

// 读任意 WIC 认识的格式，统一转成 32bpp BGRA。
bool ImageLoadFile(const wchar_t* path, Image32& out);
