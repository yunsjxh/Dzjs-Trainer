#include "stdafx.h"
#include "BuildStamp.h"
#include <cstdio>

namespace {

// "YYYY-MM-DD HH:MM" 16 个字符 + 结尾 0，留几位余量。
constexpr int kStampChars = 20;

struct Stamp {
	char utf8[kStampChars];
	wchar_t wide[kStampChars];
};

// __DATE__ 形如 "Nov 15 2026"，日不足两位时带前导空格（"Nov  5 2026"）；
// __TIME__ 形如 "16:21:07"。两者拼出 "2026-11-15 16:21"。
void FromCompileDate(char out[kStampChars])
{
	static const char* const kMonths[12] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	const char* raw = __DATE__;
	int month = 0;
	for (int i = 0; i < 12; ++i) {
		if (raw[0] == kMonths[i][0] && raw[1] == kMonths[i][1] && raw[2] == kMonths[i][2]) {
			month = i + 1;
			break;
		}
	}
	const int day = (raw[4] == ' ' ? 0 : (raw[4] - '0') * 10) + (raw[5] - '0');
	const int year = (raw[7] - '0') * 1000 + (raw[8] - '0') * 100 +
		(raw[9] - '0') * 10 + (raw[10] - '0');
	const int hour = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
	const int minute = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');
	sprintf_s(out, kStampChars, "%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
}

// PE 头里的 TimeDateStamp 是链接时刻（自 1970 起的秒数，不是 FILETIME）。
// 直接按偏移读，不套 IMAGE_NT_HEADERS —— 那样得为 32/64 位各写一份。
bool FromLinkerStamp(char out[kStampChars])
{
	HMODULE module = GetModuleHandleW(nullptr);
	if (!module) return false;
	const unsigned char* base = reinterpret_cast<const unsigned char*>(module);
	if (!base || base[0] != 'M' || base[1] != 'Z') return false;

	const DWORD lfanew = *reinterpret_cast<const DWORD*>(base + 0x3C);
	if (lfanew < 0x40 || lfanew > 0x1000) return false;
	// NT 头：Signature(4) + IMAGE_FILE_HEADER{ Machine(2) NumberOfSections(2) TimeDateStamp(4) }
	if (*reinterpret_cast<const DWORD*>(base + lfanew) != 0x00004550UL) return false;
	const DWORD stamp = *reinterpret_cast<const DWORD*>(base + lfanew + 8);
	if (stamp == 0) return false;

	const ULONGLONG ticks = static_cast<ULONGLONG>(stamp) * 10000000ULL + 116444736000000000ULL;
	FILETIME utc{};
	utc.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
	utc.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
	FILETIME local{};
	SYSTEMTIME parts{};
	if (!FileTimeToLocalFileTime(&utc, &local)) return false;
	if (!FileTimeToSystemTime(&local, &parts)) return false;
	sprintf_s(out, kStampChars, "%04u-%02u-%02u %02u:%02u",
		parts.wYear, parts.wMonth, parts.wDay, parts.wHour, parts.wMinute);
	return true;
}

Stamp MakeStamp()
{
	Stamp stamp{};
	if (!FromLinkerStamp(stamp.utf8)) FromCompileDate(stamp.utf8);
	MultiByteToWideChar(CP_UTF8, 0, stamp.utf8, -1, stamp.wide, kStampChars);
	return stamp;
}

const Stamp& Current()
{
	// 进程内只算一次：日志和关于页每次重绘都会问，没必要重复解析 PE 头。
	static const Stamp stamp = MakeStamp();
	return stamp;
}

}  // namespace

const char* JTBuildDateUtf8() { return Current().utf8; }
const wchar_t* JTBuildDateWide() { return Current().wide; }
