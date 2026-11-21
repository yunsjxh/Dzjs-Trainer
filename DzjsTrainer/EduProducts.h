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
#include <string>
#include <stdlib.h>   // _countof

// 注入到第三方进程的 hooks 实例，消息中心窗口用这个标题。
// 不能复用极域学生端的 "JiYu Trainer Virus Window"：主程序是靠
// FindWindow(标题) 找极域消息中心的，同名会让 hk: 命令投到错误的进程 ——
// 极域那边收不到 reset/setmonitor，注入的进程反而当成正常输入处理。
#define EDZU_CUSTOM_HOST_WINDOW_TITLE L"Dzjs Trainer Custom Host Window"

struct EduProcessRule {
	const wchar_t* exe;        // 学生端进程名
	const wchar_t* pathHint;   // 完整路径里必须出现的一段，不区分大小写
	// pathHint 为空时的第二判据：同目录（或上一级）里应该有的文件。
	// 伽卡他卡这类国内软件装在中文目录下，路径特征靠不住，但同目录的两个
	// 进程是稳定的。两个都为空就只认进程名，留给名字足够独特的那些。
	const wchar_t* companion;
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
	// 它本身是不是学生端。是的话「结束」就是杀掉它；不是的话它只是守护，
	// 杀掉会触发 SCM 的恢复策略把它拉回来，挂起才管用。
	bool isHost;
};

struct EduProduct {
	const wchar_t* name;
	const EduProcessRule* rules;
	int ruleCount;
	const EduGuardRule* guards;
	int guardCount;
};

// 伽卡他卡：Student.exe 是主程序（窗口、锁屏都是它），Smonitor.exe 是守着它的
// 那个（服务 Smsvc）。两个装在同一个目录。
//
// 这软件在国内基本装在中文目录下（D:\咖卡他卡安装\Student\），"gakataka" 这类
// 英文特征一个都不命中，之前只登记了 Smonitor.exe + gakataka，等于整款漏掉。
// 现在两个进程互为伴生：同目录里看见另一个，就认这个。
static const EduProcessRule kGakatakaRules[] = {
	{ L"Student.exe",  L"gakataka" },
	{ L"Student.exe",  L"伽卡他卡" },
	{ L"Student.exe",  L"咖卡他卡" },
	{ L"Student.exe",  nullptr, L"Smonitor.exe" },
	{ L"Smonitor.exe", L"gakataka" },
	{ L"Smonitor.exe", L"伽卡他卡" },
	{ L"Smonitor.exe", nullptr, L"Student.exe" },
	{ L"Smonitor.exe", nullptr, nullptr },   // 这个名字本身够独特
};
static const EduGuardRule kGakatakaGuards[] = {
	{ L"Smonitor.exe", L"Smsvc", false },
};

// 红蜘蛛：学生端 REDAgent.exe，安装目录 ...\3000soft\Red Spider\。
static const EduProcessRule kRedSpiderRules[] = {
	{ L"REDAgent.exe", L"3000soft" },
	{ L"REDAgent.exe", L"red spider" },
};
// checkrs.exe 负责拉起学生端，服务名未知，按 exe 反查。
static const EduGuardRule kRedSpiderGuards[] = {
	{ L"checkrs.exe", nullptr, false },
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
	{ L"PsGhost.exe", nullptr, true },
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

// imagePath 所在目录里有没有这个文件（找不到再看一级父目录）。
// 只看文件属性，不打开文件，扫描时每个候选进程多两次系统调用，可以忽略。
inline bool EduCompanionPresent(const wchar_t* imagePath, const wchar_t* fileName)
{
	if (!imagePath || !fileName || !*fileName) return false;
	std::wstring folder(imagePath);
	size_t slash = folder.find_last_of(L"\\/");
	if (slash == std::wstring::npos || slash < 2) return false;
	folder.resize(slash);
	for (int level = 0; level < 2 && folder.size() > 2; ++level)
	{
		const std::wstring probe = folder + L"\\" + fileName;
		const DWORD attributes = GetFileAttributesW(probe.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			return true;
		slash = folder.find_last_of(L"\\/");
		if (slash == std::wstring::npos || slash < 2) break;
		folder.resize(slash);
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
			if (rule.pathHint && *rule.pathHint)
			{
				if (!EduPathContainsNoCase(fullPath, rule.pathHint)) continue;
			}
			else if (rule.companion && *rule.companion)
			{
				if (!EduCompanionPresent(fullPath, rule.companion)) continue;
			}
			return product.name;
		}
	}
	return nullptr;
}

#endif // EDU_PRODUCTS_H
