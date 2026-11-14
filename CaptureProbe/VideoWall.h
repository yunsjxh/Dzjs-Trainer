#pragma once
// ============================================================================
// VideoWall —— 视频靶墙：一排四块 160×90 的小画面，按 30fps 同步播放。
//
// 静态品红靶标只能回答"一张截图有没有被换"。视频是另一码事：同一段内容，
// 播放器可以走 GDI 直绘、走 D3D 交换链、走分层窗口，甚至让系统直接把它排除
// 在抓屏之外。抓屏方能不能看见它，取决于"呈现方式 × 抓屏方式"这个矩阵，
// 跟谁装了钩子无关。这四块并排摆着，就是给这个矩阵当靶子用的。
//
//   G  GDI 直绘        SetDIBitsToDevice，MCI/DrawDib、老播放器、Swing 一类
//   D  D3D11 交换链    DXGI 翻转模型，播放器/浏览器/Electron 一类
//   L  分层窗口        WS_EX_LAYERED + UpdateLayeredWindow，overlay 一类
//   X  系统排除抓屏    SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
//
// 判据只看抓回来的像素：画面里有一条竖贯全高的白色扫描条，位置由帧号唯一
// 决定。抓到一帧就知道它是哪一帧；对不上，就说明这块内容压根没进这张截图。
// ============================================================================
#include "ImageIo.h"
#include <windows.h>

const int kVideoPaneW = 160;
const int kVideoPaneH = 90;
const int kVideoGap = 6;
const unsigned kVideoSweep = 64;   // 扫描条扫满一趟用多少帧（约 2.1 秒）

enum VideoPaneIndex
{
	PaneGdi = 0,
	PaneD3d,
	PaneLayered,
	PaneExcluded,
	PaneCount
};

int VideoWallStripWidth();
int VideoWallStripHeight();
const wchar_t* VideoWallLegend();
const wchar_t* VideoWallMarkLegend();

bool VideoWallCreate(HWND parent);
void VideoWallDestroy();
bool VideoWallAlive();
void VideoWallSetRect(const RECT& clientRect);   // 父窗口客户区坐标
RECT VideoWallClientRect();
const wchar_t* VideoWallInitNote();              // 某几路没起来的原因，空串表示正常

void VideoWallTick();          // 主窗口 33ms 定时器驱动，同一帧推给四块
int VideoWallFrame();
void VideoWallPump(unsigned ms);

struct VideoWallReport
{
	bool valid = false;        // 本次抓图里能不能看到视频区
	int covered = 0;           // 有几路的区域落在抓图范围内
	int live = 0;              // 有几路抓到的确实是这一帧
	wchar_t marks[8] = L"----";// 逐路结果，顺序 G D L X
	wchar_t note[192] = {};
};

// frame 是抓图那一刻的帧号；判"是不是本帧"用它，容许显示链路的几帧延迟。
void VideoWallAnalyze(const Image32& img, const RECT& screenRect, int frame, VideoWallReport& out);

// "视频 G✔ D✔ L✕ X? · 2/4 本帧"
void VideoWallReportText(const VideoWallReport& r, wchar_t* buf, size_t cch);
