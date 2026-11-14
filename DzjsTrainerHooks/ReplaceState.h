#pragma once
//
// ReplaceState.h —— 替换画面的**共享状态**（原来三套并行，现在两个对象）。
//
// ======================================================================
// 原来是什么样
// ======================================================================
//   g_HdcMem + g_HdcMemCs + g_HdcMemRefs + g_HdcMemPending          （外部替换 DC）
//   g_ImgData + g_ImgDibData + g_ImgW/H + g_ImgRefs + g_ImgPending   （图片/视频帧）
//   g_VideoThread + g_VideoStopEvent + g_VideoFrameActive            （解码线程）
//
//   前两套各自一份临界区、一份引用计数、一份"延迟释放"逻辑，**逐字重复**，
//   差别只在释放手段是 DeleteDC 还是 delete[]。重复的真实成本是：
//   修了其中一套的竞态，另一套照旧 —— 而两套都在同一条捕获热路径上被读。
//   第三套（解码线程）产出的帧最终发布进第二套，本身不持有像素，留在原处。
//
// ======================================================================
// 现在：两个对象，各自的边界
// ======================================================================
//   · ExternalSurface —— 一次只有一个 DC，**所有权在调用方**
//     （VSetVideoStreamModify 传进来的）。这里只在"换新/停用"时负责把旧 DC
//     安全地延迟删除。
//   · FrameStore     —— 一次只有一个帧缓冲，**所有权在本模块**
//     （Publish 之后由 store 负责 delete[]）。
//
// 两者都保留"引用计数归零才真正释放"的语义：捕获热路径可能正握着指针，
// 立刻释放会让另一线程读到已释放内存。
//
// ⚠️ 临界区靠显式 EnsureInit() 懒初始化，与原来一致 ——
//   避免在 DllMain 之前引入新的初始化顺序依赖。
//
#ifndef _WINDOWS_
#include <windows.h>
#endif
#include <vector>

namespace ReplaceState
{
	// ------------------------------------------------------------------
	// 外部替换画面（HDC 的所有权在调用方）
	// ------------------------------------------------------------------
	class ExternalSurface
	{
	public:
		// 换新画面（或传 nullptr 停用）。
		//
		// 旧 DC 的处理：
		//   · 当前没有线程在借用   ⇒ 立即 DeleteDC
		//   · 有线程正在借用       ⇒ 挂进 pending，等最后一个 Return() 时删除
		// ★ 宽度/高度存在这里而不是由调用方另存：判定"这块 BitBlt 是不是在抓
		//   我们替换的那张画面"必须与 DC 在同一把锁里完成，否则会出现
		//   "判定时是旧尺寸、使用时已经是新 DC"的错配。
		void Set(int width, int height, HDC dc) noexcept;

		// 借用：尺寸匹配且源确实是窗口/屏幕 DC 时返回 DC 并把引用计数 +1；否则返回 nullptr。
		// `hasSurface` 输出"当前有没有外部替换画面"—— 它必须与 Borrow 在同一把锁里取，
		// 否则调用方会拿到"借不到 + 不知道该不该刷黑"的矛盾组合。
		// 返回非空时**必须**成对调用 Return()。
		HDC Borrow(int width, int height, bool sourceIsDc, bool* hasSurface) noexcept;
		void Return() noexcept;

		// 只读查看（不加引用，仅用于"有没有外部画面"这类判定）。
		bool HasSurface() noexcept;
		int Width() noexcept;
		int Height() noexcept;

		void Shutdown() noexcept;

	private:
		void EnsureInit() noexcept;

		HDC m_dc = nullptr;
		int m_width = 0;
		int m_height = 0;
		volatile LONG m_refs = 0;
		CRITICAL_SECTION m_cs = {};
		volatile LONG m_csInit = 0;
		std::vector<HDC> m_pending;   // 借用中的旧 DC（正常至多 1 个）
	};

	// ------------------------------------------------------------------
	// 帧缓冲（所有权在本模块）
	// ------------------------------------------------------------------
	struct Frame
	{
		BYTE* rgb = nullptr;      // RGB24（R,G,B），紧凑，stride = width*3
		BYTE* dib = nullptr;      // BGR24，顶向下，stride = dibStride
		int width = 0;
		int height = 0;
		int dibStride = 0;
	};

	class FrameStore
	{
	public:
		// 接管所有权。参数不可用时**释放并返回 false**。
		// ★ "失败也接管所有权"是刻意约定：调用点不必写两条 delete 分支，
		//   少一处就少一处泄漏。
		bool Publish(BYTE* rgb, BYTE* dib, int width, int height, int dibStride) noexcept;

		// 借用当前帧（引用计数 +1）。返回 false 时出参不变。
		bool Acquire(Frame& out) noexcept;
		void Release() noexcept;

		// 丢弃当前帧（引用归零则立即释放，否则挂等到 Release）。
		void Clear() noexcept;

		bool Valid() noexcept;
		void Shutdown() noexcept;

	private:
		void EnsureInit() noexcept;
		void ReclaimLocked() noexcept;   // 调用方持锁

		Frame m_frame;
		bool m_valid = false;
		volatile LONG m_refs = 0;
		CRITICAL_SECTION m_cs = {};
		volatile LONG m_csInit = 0;
		std::vector<BYTE*> m_pending;   // 借用期间不能立刻释放的旧缓冲
	};
}
