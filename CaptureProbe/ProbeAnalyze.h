#pragma once
// ============================================================================
// ProbeAnalyze —— 把一张抓回来的图判成"真画面 / 被替换 / 纯色兜底"。
//
// 判据不依赖 hook 内部状态，只看像素，所以对任何注入实现都成立：
//   1. 靶标（窗口里的品红色块）在应该在的位置上 → 真画面；
//   2. 整幅图接近单一颜色，且是白/黑 → 命中了纯色兜底分支；
//   3. 既没有靶标、也不是纯色 → 画面被换成了别的东西；
//      如果给了期望图，再算一遍 16×16 指纹距离，能进一步确认"是那张图"。
// ============================================================================
#include "ImageIo.h"
#include <windows.h>

enum class Verdict
{
	NotRun = 0,
	Pass,           // 靶标完整，说明抓到的就是真实屏幕
	ContentOnly,    // 画面有结构、靶标却读不出来（多是 16bpp）——视为未被拦
	ReplacedImage,  // 与期望替换图高度一致
	ReplacedOther,  // 靶标没了，整幅图也不是纯色 —— 被换成了别的内容
	SolidWhite,     // 整幅纯白（!allowMonitor 时的纯色兜底）
	SolidBlack,     // 整幅纯黑（videoModify 无图时的纯色兜底）
	Blocked,        // 靶窗被别的窗口挡住，本次结果不可信
	Failed,         // 压根没抓到画面
};

struct AnalyzeInput
{
	const Image32* image = nullptr;
	RECT screenRect = {};    // image 覆盖的屏幕矩形（虚拟屏坐标）
	RECT targetRect = {};    // 靶标在屏幕上的矩形
	const Image32* expect = nullptr;   // 期望的替换图，可为空
	bool targetless = false; // 位深太低、靶色不可靠时置 true，用"有内容"代替判靶
};

struct AnalyzeResult
{
	Verdict verdict = Verdict::NotRun;
	double targetRatio = 0.0;    // 靶标色占采样点的比例，1.0 = 完好
	double expectDistance = -1.0;// 与期望图的指纹距离，-1 = 没给期望图
	int meanLuma = 0;
	int minLuma = 0;
	int maxLuma = 0;
	RECT targetInImage = {};

	const wchar_t* Text() const;
	COLORREF Color() const;

	// 一行补充信息，给列表下方和 CSV 用
	void Detail(wchar_t* buf, size_t cch) const;
};

AnalyzeResult AnalyzeCapture(const AnalyzeInput& in);

const wchar_t* VerdictText(Verdict v);
COLORREF VerdictColor(Verdict v);

// 两图缩到 16×16 后的平均通道差，0 最像，255 最不像。
double FingerprintDistance(const Image32& a, const Image32& b);
