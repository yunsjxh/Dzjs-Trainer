// ============================================================================
// EduProducts.h —— 其他电子教室产品的自动适配表
//
// 极域那条线是写死的：StudentMain.exe 单进程、单 PID，注入状态和握手全绑在
// 一起。伽卡他卡 / 红蜘蛛 / 联想传奇 没有这套语义，走的是「自定义注入」那条
// 通用宿主路径（Hooks 里 g_customHost 分支，装的是同一张 34 项钩子表）。
//
// 所以这里只做一件事：认出学生端进程，把 PID 交给 InjectCustomProcess。
//
// 每条规则都要求「进程名 + 路径特征」同时命中。理由很实际：Student.exe 这种
// 名字满地都是，只看名字等于给无关进程装钩子。路径特征取安装目录里的一段，
// 不区分大小写。
// ============================================================================
#ifndef EDU_PRODUCTS_H
#define EDU_PRODUCTS_H

#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>   // _countof

// 注入到第三方进程的 hooks 实例，消息中心窗口用这个标题。
// 不能复用极域学生端的 "JiYu Trainer Virus Window"：主程序是靠
// FindWindow(标题) 找极域消息中心的，同名会让 hk: 命令投到错误的进程 ——
// 极域那边收不到 reset/setmonitor，注入的进程反而当成正常输入处理。
#define EDZU_CUSTOM_HOST_WINDOW_TITLE L"Dzjs Trainer Custom Host Window"

struct EduProcessRule {
	const wchar_t* exe;       // 学生端进程名
	const wchar_t* pathHint;  // 完整路径里必须出现的一段，不区分大小写
};

// 结束一款教室之前要先停掉的上游。
//
// 红蜘蛛的父链是 REDAgent.exe ← checkrs.exe ← services.exe：checkrs 是个服务，
// 学生端挂了它会把学生端重新拉起来。只结束 REDAgent，看起来就像「点了没反应」。
// service 写 nullptr 表示服务名不确定，按「服务的二进制路径里含这个 exe 名」反查：
// 服务名随版本和语言变，进程名反而稳定。
struct EduGuardRule {
	const wchar_t* exe;
	const wchar_t* service;
};

struct EduProduct {
	const wchar_t* name;
	const EduProcessRule* rules;
	int ruleCount;
	const EduGuardRule* guards;
	int guardCount;
};

// 伽卡他卡：学生端 Smonitor.exe（服务 Smsvc），装在 ...\gakataka\Student\ 下。
static const EduProcessRule kGakatakaRules[] = {
	{ L"Smonitor.exe", L"gakataka" },
};
static const EduGuardRule kGakatakaGuards[] = {
	{ L"Smonitor.exe", L"Smsvc" },
};

// 红蜘蛛：学生端 REDAgent.exe，安装目录 ...\3000soft\Red Spider\。
static const EduProcessRule kRedSpiderRules[] = {
	{ L"REDAgent.exe", L"3000soft" },
	{ L"REDAgent.exe", L"red spider" },
};
// checkrs.exe 负责拉起学生端，服务名未知，按 exe 反查。
static const EduGuardRule kRedSpiderGuards[] = {
	{ L"checkrs.exe", nullptr },
};

// 联想传奇：PsGhost 是守护进程，Student 才是学生端，两个都得进。
// 目录 ...\Parasaga\cyberclass\student\，cyberclass 和 parasaga 都认。
static const EduProcessRule kParasagaRules[] = {
	{ L"PsGhost.exe", L"cyberclass" },
	{ L"Student.exe", L"cyberclass" },
	{ L"PsGhost.exe", L"parasaga" },
	{ L"Student.exe", L"parasaga" },
};
static const EduGuardRule kParasagaGuards[] = {
	{ L"PsGhost.exe", nullptr },
};

static const EduProduct kEduProducts[] = {
	{ L"伽卡他卡", kGakatakaRules,  _countof(kGakatakaRules),  kGakatakaGuards,  _countof(kGakatakaGuards) },
	{ L"红蜘蛛",   kRedSpiderRules, _countof(kRedSpiderRules), kRedSpiderGuards, _countof(kRedSpiderGuards) },
	{ L"联想传奇", kParasagaRules,  _countof(kParasagaRules),  kParasagaGuards,  _countof(kParasagaGuards) },
};

static const int kEduProductCount = _countof(kEduProducts);

inline bool EduPathContainsNoCase(const wchar_t* haystack, const wchar_t* needle)
{
	if (!haystack || !needle || !*needle) return false;
	const size_t needleLen = wcslen(needle);
	for (const wchar_t* cursor = haystack; *cursor; ++cursor)
	{
		if (_wcsnicmp(cursor, needle, needleLen) == 0) return true;
	}
	return false;
}

// 只比进程名：扫描时先用它过一遍，非候选就不必去查进程路径。
// 一次全量扫描要过一百多个进程，逐个 OpenProcess 查路径比这里贵得多。
inline bool EduCompatNameCandidate(const wchar_t* exeName)
{
	if (!exeName) return false;
	for (int i = 0; i < kEduProductCount; ++i)
	{
		const EduProduct& product = kEduProducts[i];
		for (int r = 0; r < product.ruleCount; ++r)
			if (_wcsicmp(exeName, product.rules[r].exe) == 0) return true;
	}
	return false;
}

// 命中返回产品名，否则 nullptr。
inline const wchar_t* EduCompatMatch(const wchar_t* exeName, const wchar_t* fullPath)
{
	if (!exeName || !fullPath) return nullptr;
	for (int i = 0; i < kEduProductCount; ++i)
	{
		const EduProduct& product = kEduProducts[i];
		for (int r = 0; r < product.ruleCount; ++r)
		{
			const EduProcessRule& rule = product.rules[r];
			if (_wcsicmp(exeName, rule.exe) != 0) continue;
			if (!EduPathContainsNoCase(fullPath, rule.pathHint)) continue;
			return product.name;
		}
	}
	return nullptr;
}

#endif // EDU_PRODUCTS_H
