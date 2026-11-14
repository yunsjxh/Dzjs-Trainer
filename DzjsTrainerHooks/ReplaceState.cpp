#include "stdafx.h"
#include "ReplaceState.h"

namespace ReplaceState
{
	// ==================================================================
	// ExternalSurface
	// ==================================================================

	void ExternalSurface::EnsureInit() noexcept
	{
		if (InterlockedCompareExchange(&m_csInit, 1, 0) == 0)
		{
			InitializeCriticalSection(&m_cs);
		}
	}

	void ExternalSurface::Set(int width, int height, HDC dc) noexcept
	{
		EnsureInit();

		HDC old = nullptr;
		EnterCriticalSection(&m_cs);
		{
			old = m_dc;
			m_dc = dc;
			m_width = dc ? width : 0;
			m_height = dc ? height : 0;
			// 同一个 DC 重复设置：只刷新尺寸，不触发延迟删除。
			if (old == dc)
			{
				old = nullptr;
			}
		}
		LeaveCriticalSection(&m_cs);

		if (old)
		{
			// 若此刻仍有捕获线程在借用 old，绝不能立即删除：
			// 先挂起，等引用计数归零时由 Return() 收尾。
			if (InterlockedCompareExchange(&m_refs, 0, 0) == 0)
			{
				DeleteDC(old);
			}
			else
			{
				EnterCriticalSection(&m_cs);
				m_pending.push_back(old);
				LeaveCriticalSection(&m_cs);
			}
		}
	}

	HDC ExternalSurface::Borrow(int width, int height, bool sourceIsDc, bool* hasSurface) noexcept
	{
		if (hasSurface)
		{
			*hasSurface = false;
		}
		if (!m_csInit)
		{
			return nullptr;
		}

		HDC out = nullptr;
		EnterCriticalSection(&m_cs);
		if (hasSurface)
		{
			*hasSurface = (m_dc != nullptr);
		}
		if (m_dc && sourceIsDc && width == m_width && height == m_height)
		{
			out = m_dc;
			InterlockedIncrement(&m_refs);
		}
		LeaveCriticalSection(&m_cs);
		return out;
	}

	void ExternalSurface::Return() noexcept
	{
		if (!m_csInit)
		{
			return;
		}

		EnterCriticalSection(&m_cs);
		if (InterlockedDecrement(&m_refs) == 0 && !m_pending.empty())
		{
			for (size_t i = 0; i < m_pending.size(); ++i)
			{
				DeleteDC(m_pending[i]);
			}
			m_pending.clear();
		}
		LeaveCriticalSection(&m_cs);
	}

	bool ExternalSurface::HasSurface() noexcept
	{
		if (!m_csInit)
		{
			return false;
		}
		EnterCriticalSection(&m_cs);
		const bool has = m_dc != nullptr;
		LeaveCriticalSection(&m_cs);
		return has;
	}

	int ExternalSurface::Width() noexcept
	{
		if (!m_csInit)
		{
			return 0;
		}
		EnterCriticalSection(&m_cs);
		const int w = m_dc ? m_width : 0;
		LeaveCriticalSection(&m_cs);
		return w;
	}

	int ExternalSurface::Height() noexcept
	{
		if (!m_csInit)
		{
			return 0;
		}
		EnterCriticalSection(&m_cs);
		const int h = m_dc ? m_height : 0;
		LeaveCriticalSection(&m_cs);
		return h;
	}

	void ExternalSurface::Shutdown() noexcept
	{
		if (!m_csInit)
		{
			return;
		}

		EnterCriticalSection(&m_cs);
		if (m_dc)
		{
			DeleteDC(m_dc);
			m_dc = nullptr;
		}
		for (size_t i = 0; i < m_pending.size(); ++i)
		{
			DeleteDC(m_pending[i]);
		}
		m_pending.clear();
		m_width = 0;
		m_height = 0;
		LeaveCriticalSection(&m_cs);

		DeleteCriticalSection(&m_cs);
		InterlockedExchange(&m_csInit, 0);
	}

	// ==================================================================
	// FrameStore
	// ==================================================================

	void FrameStore::EnsureInit() noexcept
	{
		if (InterlockedCompareExchange(&m_csInit, 1, 0) == 0)
		{
			InitializeCriticalSection(&m_cs);
		}
	}

	void FrameStore::ReclaimLocked() noexcept
	{
		if (m_refs == 0 && !m_pending.empty())
		{
			for (size_t i = 0; i < m_pending.size(); ++i)
			{
				delete[] m_pending[i];
			}
			m_pending.clear();
		}
	}

	bool FrameStore::Publish(BYTE* rgb, BYTE* dib, int width, int height, int dibStride) noexcept
	{
		// 失败也接管所有权（见头文件里的约定）。
		if (!rgb || !dib || width <= 0 || height <= 0 || dibStride < width * 3)
		{
			delete[] rgb;
			delete[] dib;
			return false;
		}

		EnsureInit();

		EnterCriticalSection(&m_cs);
		if (m_valid)
		{
			if (m_refs == 0)
			{
				delete[] m_frame.rgb;
				delete[] m_frame.dib;
			}
			else
			{
				// 还有线程正握着上一帧：挂等到它 Release。
				if (m_frame.rgb) m_pending.push_back(m_frame.rgb);
				if (m_frame.dib) m_pending.push_back(m_frame.dib);
			}
		}
		m_frame.rgb = rgb;
		m_frame.dib = dib;
		m_frame.width = width;
		m_frame.height = height;
		m_frame.dibStride = dibStride;
		m_valid = true;
		LeaveCriticalSection(&m_cs);
		return true;
	}

	bool FrameStore::Acquire(Frame& out) noexcept
	{
		if (!m_csInit)
		{
			return false;
		}

		EnterCriticalSection(&m_cs);
		if (!m_valid)
		{
			LeaveCriticalSection(&m_cs);
			return false;
		}
		InterlockedIncrement(&m_refs);
		out = m_frame;
		LeaveCriticalSection(&m_cs);
		return true;
	}

	void FrameStore::Release() noexcept
	{
		if (!m_csInit)
		{
			return;
		}

		EnterCriticalSection(&m_cs);
		InterlockedDecrement(&m_refs);
		ReclaimLocked();
		LeaveCriticalSection(&m_cs);
	}

	void FrameStore::Clear() noexcept
	{
		if (!m_csInit)
		{
			return;
		}

		EnterCriticalSection(&m_cs);
		if (m_valid)
		{
			if (m_refs == 0)
			{
				delete[] m_frame.rgb;
				delete[] m_frame.dib;
			}
			else
			{
				if (m_frame.rgb) m_pending.push_back(m_frame.rgb);
				if (m_frame.dib) m_pending.push_back(m_frame.dib);
			}
			m_valid = false;
			m_frame = Frame{};
		}
		ReclaimLocked();
		LeaveCriticalSection(&m_cs);
	}

	bool FrameStore::Valid() noexcept
	{
		if (!m_csInit)
		{
			return false;
		}
		EnterCriticalSection(&m_cs);
		const bool valid = m_valid;
		LeaveCriticalSection(&m_cs);
		return valid;
	}

	void FrameStore::Shutdown() noexcept
	{
		if (!m_csInit)
		{
			return;
		}

		EnterCriticalSection(&m_cs);
		// 卸载时已保证没有借用者（调用点在停用全部 detour 之后）。
		// 即便如此也走同一套顺序：当前帧 → pending，避免"少销毁一类"。
		delete[] m_frame.rgb;
		delete[] m_frame.dib;
		m_frame = Frame{};
		m_valid = false;
		for (size_t i = 0; i < m_pending.size(); ++i)
		{
			delete[] m_pending[i];
		}
		m_pending.clear();
		LeaveCriticalSection(&m_cs);

		DeleteCriticalSection(&m_cs);
		InterlockedExchange(&m_csInit, 0);
	}
}
