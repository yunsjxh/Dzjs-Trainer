#include "stdafx.h"
#include "Logger.h"
#include <time.h>
#include <share.h>
#include "StringHlp.h"
#include "PathHelper.h"

using namespace std;

#undef LogError2
#undef LogWarn2
#undef LogInfo2
#undef Log2

namespace
{
	std::wstring NarrowToWide(const char* text, UINT codePage, DWORD flags)
	{
		if (text == nullptr || *text == '\0') {
			return std::wstring();
		}

		const int length = MultiByteToWideChar(codePage, flags, text, -1, nullptr, 0);
		if (length <= 0) {
			return std::wstring();
		}

		std::wstring result(static_cast<size_t>(length), L'\0');
		if (MultiByteToWideChar(codePage, flags, text, -1, &result[0], length) <= 0) {
			return std::wstring();
		}
		result.resize(static_cast<size_t>(length - 1));
		return result;
	}

	std::wstring SourceTextToWide(const char* text)
	{
		std::wstring result = NarrowToWide(text, CP_UTF8, MB_ERR_INVALID_CHARS);
		if (!result.empty() || text == nullptr || *text == '\0') {
			return result;
		}
		return NarrowToWide(text, CP_ACP, 0);
	}

	std::wstring SourcePathForLog(const char* file)
	{
		const std::wstring path = SourceTextToWide(file);
		const size_t fileSeparator = path.find_last_of(L"\\/");
		if (fileSeparator == std::wstring::npos) {
			return path;
		}

		const size_t projectSeparator = path.find_last_of(L"\\/", fileSeparator - 1);
		return projectSeparator == std::wstring::npos
			? path
			: path.substr(projectSeparator + 1);
	}
}

LoggerInternal::LoggerInternal()
{
}
LoggerInternal::~LoggerInternal()
{
	CloseLogFile();
}

void LoggerInternal::Log(const wchar_t * str, ...)
{
	if (level <= LogLevelText) {
		va_list arg;
		va_start(arg, str);
		LogInternal(LogLevelText, str, arg);
		va_end(arg);
	}
}
void LoggerInternal::LogWarn(const wchar_t * str, ...)
{
	if (level <= LogLevelWarn) {
		va_list arg;
		va_start(arg, str);
		LogInternal(LogLevelWarn, str, arg);
		va_end(arg);
	}
}
void LoggerInternal::LogError(const wchar_t * str, ...)
{
	if (level <= LogLevelError) {
		va_list arg;
		va_start(arg, str);
		LogInternal(LogLevelError, str, arg);
		va_end(arg); 
	}
}
void LoggerInternal::LogInfo(const wchar_t * str, ...)
{
	if (level <= LogLevelInfo) {
		va_list arg;
		va_start(arg, str);
		LogInternal(LogLevelInfo, str, arg);
		va_end(arg);
	}
}

void LoggerInternal::Log2(const wchar_t * str, const char * file, int line, const char * functon, ...)
{
	if (level <= LogLevelText) {
		va_list arg;
		va_start(arg, functon);
		LogInternalWithCodeAndLine(LogLevelText, str, file, line, functon, arg);
		va_end(arg);
	}
}
void LoggerInternal::LogWarn2(const wchar_t * str, const char * file, int line, const char * functon, ...)
{
	if (level <= LogLevelWarn) {
		va_list arg;
		va_start(arg, functon);
		LogInternalWithCodeAndLine(LogLevelWarn, str, file, line, functon, arg);
		va_end(arg);
	}
}
void LoggerInternal::LogError2(const wchar_t * str, const char * file, int line, const char * functon, ...)
{
	if (level <= LogLevelError) {
		va_list arg;
		va_start(arg, functon);
		LogInternalWithCodeAndLine(LogLevelError, str, file, line, functon, arg);
		va_end(arg);
	}
}
void LoggerInternal::LogInfo2(const wchar_t * str, const char * file, int line, const  char * functon, ...)
{
	if (level <= LogLevelInfo) {
		va_list arg;
		va_start(arg, functon);
		LogInternalWithCodeAndLine(LogLevelInfo, str, file, line, functon, arg);
		va_end(arg);
	}
}

void LoggerInternal::SetLogLevel(LogLevel level)
{
	this->level = level;
}
void LoggerInternal::SetLogOutPut(LogOutPut output)
{
	std::lock_guard<std::mutex> guard(outputMutex);
	this->outPut = output;
}
bool LoggerInternal::ApplyLogDirectory(const std::wstring& dir, const std::wstring& stem)
{
	logDir = dir;
	logStem = stem;
	logFileDate = SYSTEMTIME{};
	logFilePath[0] = L'\0';
	return ResolveDailyFile();
}

bool LoggerInternal::ResolveDailyFile()
{
	if (logDir.empty() || logStem.empty()) return false;

	SYSTEMTIME now{};
	GetLocalTime(&now);
	if (logFilePath[0] != L'\0' &&
		now.wYear == logFileDate.wYear && now.wMonth == logFileDate.wMonth &&
		now.wDay == logFileDate.wDay) {
		return true;
	}

	// 只建一级：父目录是 exe 所在目录或 %LOCALAPPDATA%\DzjsTrainer，都已经在。
	CreateDirectoryW(logDir.c_str(), nullptr);

	WCHAR path[MAX_PATH] = {};
	swprintf_s(path, L"%s\\%s-%04d-%02d-%02d.log", logDir.c_str(), logStem.c_str(),
		now.wYear, now.wMonth, now.wDay);

	FILE* file = _wfsopen(path, L"a+", _SH_DENYNO);
	if (file == nullptr) {
		logFilePath[0] = L'\0';
		return false;
	}
	// 同一天里程序重启过几次、跨天换到新文件，都能从这一行看出来。
	fwprintf_s(file, L"\n========== DzjsTrainer session started %04d-%02d-%02d %02d:%02d:%02d ==========\n",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	fflush(file);
	fclose(file);

	wcsncpy_s(logFilePath, path, MAX_PATH);
	logFileDate = now;
	return true;
}

void LoggerInternal::SetLogOutPutFile(const wchar_t * filePath)
{
	std::lock_guard<std::mutex> guard(outputMutex);
	if (filePath == nullptr || *filePath == L'\0') return;

	const std::wstring requested(filePath);
	if (requested == logRequested) return;
	logRequested = requested;

	// 拆出目录和文件名主干。传进来的路径里的日期只是当天日期，真正写哪个
	// 文件由 ResolveDailyFile 按当前时间决定。
	std::wstring directory;
	std::wstring name = requested;
	const size_t separator = requested.find_last_of(L"\\/");
	if (separator != std::wstring::npos) {
		directory = requested.substr(0, separator);
		name = requested.substr(separator + 1);
	}
	const size_t dot = name.find_last_of(L'.');
	const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);

	if (ApplyLogDirectory(directory, stem)) return;

	// An elevated driver loader may run from Program Files, but a child or
	// recovery instance can still lack write access to that directory.
	// Keep logs durable by falling back to a per-user location.
	WCHAR localAppData[MAX_PATH] = {};
	const DWORD length = GetEnvironmentVariableW(
		L"LOCALAPPDATA", localAppData, _countof(localAppData));
	if (length == 0 || length >= _countof(localAppData)) return;

	const std::wstring appDirectory = std::wstring(localAppData) + L"\\DzjsTrainer";
	CreateDirectoryW(appDirectory.c_str(), NULL);
	if (!ApplyLogDirectory(appDirectory + L"\\logs", stem)) return;

	FILE* file = _wfsopen(logFilePath, L"a+", _SH_DENYNO);
	if (file) {
		fwprintf_s(file, L"[I] Log path fallback: %s\n", logFilePath);
		fflush(file);
		fclose(file);
	}
}
void LoggerInternal::SetLogOutPutCallback(LogCallBack callback, LPARAM lparam)
{
	std::unique_lock<std::mutex> guard(outputMutex);
	callBack = callback;
	callBackData = lparam;
	if (!callback) {
		callbackIdle.wait(guard, [this]() { return activeCallbacks == 0; });
	}
}

void LoggerInternal::ResentNotCaputureLog()
{
	if (outPut == LogOutPutCallback && callBack) {
		std::list< LOG_SLA>::iterator i;
		for (i = logPendingBuffer.begin(); i != logPendingBuffer.end(); i++)
			callBack((*i).str.c_str(), (*i).level, callBackData);
		logPendingBuffer.clear();
	}
}
void LoggerInternal::WritePendingLog(const wchar_t * str, LogLevel level)
{
	LOG_SLA sla = { std::wstring(str), level };
	logPendingBuffer.push_back(sla);
}

void LoggerInternal::LogInternalWithCodeAndLine(LogLevel level, const wchar_t * str, const char * file, int line, const char * functon, va_list arg)
{
	const std::wstring wideFile = SourcePathForLog(file);
	const std::wstring wideFunction = SourceTextToWide(functon);
	wstring format1 = FormatString(L"%s\n[In] %s (%d) : %s", str, wideFile.c_str(), line, wideFunction.c_str());
	LogInternal(level, format1.c_str(), arg);
}
void LoggerInternal::LogInternal(LogLevel level, const wchar_t * str, va_list arg)
{
	const wchar_t*levelStr = L"";
	switch (level)
	{
	case LogLevelInfo: levelStr = L"I"; break;
	case LogLevelWarn: levelStr = L"W"; break;
	case LogLevelError: levelStr = L"E"; break;
	case LogLevelText: levelStr = L"T"; break;
	}
	time_t time_log = time(NULL);
	struct tm tm_log;
	localtime_s(&tm_log, &time_log);

	wstring format1 = FormatString(L"[%02d:%02d:%02d] [%s] %s\n", tm_log.tm_hour, tm_log.tm_min, tm_log.tm_sec, levelStr, str);
	wstring out = FormatString(format1.c_str(), arg);

	LogOutput(level, out.c_str(), out.size());
}
void LoggerInternal::LogOutput(LogLevel level, const wchar_t * str, size_t len)
{
#if _DEBUG
	OutputDebugString(str);
#else 
	if (outPut == LogOutPutConsolne)
		OutputDebugString(str);
#endif
	LogCallBack callback = nullptr;
	LPARAM callbackData = 0;
	{
		std::lock_guard<std::mutex> guard(outputMutex);
		bool wroteFile = false;
		// 跨天运行时先换到当天的文件，再写这一条。
		if (!logDir.empty()) ResolveDailyFile();
		// Open with shared access and close after every record so a hung process
		// never keeps the day's log locked.
		if (logFilePath[0]) {
			FILE* writeFile = _wfsopen(logFilePath, L"a+", _SH_DENYNO);
			if (writeFile) {
				fwprintf_s(writeFile, L"%s", str);
				wroteFile = fflush(writeFile) == 0;
				fclose(writeFile);
			}
		}
		if (outPut == LogOutPutCallback && callBack) {
			callback = callBack;
			callbackData = callBackData;
			++activeCallbacks;
		}
		else if (!wroteFile) {
			WritePendingLog(str, level);
		}
	}
	if (callback) {
		callback(str, level, callbackData);
		std::lock_guard<std::mutex> guard(outputMutex);
		--activeCallbacks;
		if (activeCallbacks == 0) callbackIdle.notify_all();
	}
}

void LoggerInternal::CloseLogFile()
{
	// Log files are opened and closed for each record; no persistent handle exists.
}
