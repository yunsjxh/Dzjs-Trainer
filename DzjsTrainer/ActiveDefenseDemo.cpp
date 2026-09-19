#include "stdafx.h"
#include "ActiveDefenseDemo.h"
#include "Logger.h"
#include "PathHelper.h"

#include <TlHelp32.h>
#include <cwctype>
#include <string>
#include <vector>

extern LoggerInternal* currentLogger;

namespace {
HANDLE g_stopEvent = NULL;
HANDLE g_workerThread = NULL;

bool ContainsInsensitive(const std::wstring& value, const wchar_t* fragment)
{
	return fragment != nullptr &&
		value.find(fragment) != std::wstring::npos;
}

int ScorePath(const std::wstring& path, bool systemImage, bool protectedTarget)
{
	if (path.empty()) return 30;
	if (systemImage) {
		std::wstring lower = path;
		for (wchar_t& character : lower) character = towlower(character);
		if (ContainsInsensitive(lower, L"\\windows\\system32\\") ||
			ContainsInsensitive(lower, L"\\windows\\syswow64\\")) {
			return 0;
		}
		return 60;
	}

	std::wstring lower = path;
	for (wchar_t& character : lower) character = towlower(character);
	int score = protectedTarget ? 10 : 0;
	if (ContainsInsensitive(lower, L"\\appdata\\local\\temp\\") ||
		ContainsInsensitive(lower, L"\\windows\\temp\\") ||
		ContainsInsensitive(lower, L"\\downloads\\")) {
		score += 50;
	}
	if (ContainsInsensitive(lower, L"\\startup\\") ||
		ContainsInsensitive(lower, L"\\programdata\\")) {
		score += 30;
	}
	return score;
}

void LogProcessFinding(DWORD processId, const std::wstring& path, int score, bool baseline)
{
	if (!currentLogger || score < 30) return;
	currentLogger->LogWarn(
		L"[DEFENSE-DEMO] %s suspicious process pid=%lu score=%d path=%s",
		baseline ? L"baseline" : L"periodic",
		processId,
		score,
		path.empty() ? L"<unknown>" : path.c_str());
}

bool QueryProcessPath(HANDLE process, std::wstring& path)
{
	DWORD capacity = 32768;
	std::vector<wchar_t> buffer(capacity);
	if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &capacity)) return false;
	path.assign(buffer.data(), capacity);
	return true;
}

void ScanProcessBaseline(bool baseline)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		if (currentLogger) currentLogger->LogWarn(L"[DEFENSE-DEMO] process snapshot failed: %lu", GetLastError());
		return;
	}

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (process) {
				std::wstring path;
				if (QueryProcessPath(process, path)) {
					int score = ScorePath(path, false, false);
					LogProcessFinding(entry.th32ProcessID, path, score, baseline);
				}
				CloseHandle(process);
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
}

DWORD WINAPI DefenseWorker(LPVOID)
{
	ScanProcessBaseline(true);
	while (WaitForSingleObject(g_stopEvent, 5000) == WAIT_TIMEOUT) {
		ScanProcessBaseline(false);
	}
	return 0;
}
}

BOOL ActiveDefenseDemoScoreImage(
	ULONG processId,
	LPCWSTR imagePath,
	BOOL systemImage,
	BOOL protectedTarget)
{
	std::wstring path = imagePath ? imagePath : L"";
	int score = ScorePath(path, systemImage != FALSE, protectedTarget != FALSE);
	if (currentLogger && score >= 30) {
		currentLogger->LogWarn(
			L"[DEFENSE-DEMO] suspicious image pid=%lu score=%d system=%d target=%d path=%s",
			processId,
			score,
			systemImage ? 1 : 0,
			protectedTarget ? 1 : 0,
			path.empty() ? L"<unknown>" : path.c_str());
	}
	return score >= 60 ? TRUE : FALSE;
}

BOOL ActiveDefenseDemoStart()
{
	if (g_workerThread) return TRUE;
	g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_stopEvent) return FALSE;
	g_workerThread = CreateThread(NULL, 0, DefenseWorker, NULL, 0, NULL);
	if (!g_workerThread) {
		CloseHandle(g_stopEvent);
		g_stopEvent = NULL;
		return FALSE;
	}
	if (currentLogger) currentLogger->LogInfo(L"[DEFENSE-DEMO] audit monitor started");
	return TRUE;
}

void ActiveDefenseDemoStop()
{
	if (g_stopEvent) SetEvent(g_stopEvent);
	if (g_workerThread) {
		WaitForSingleObject(g_workerThread, INFINITE);
		CloseHandle(g_workerThread);
		g_workerThread = NULL;
	}
	if (g_stopEvent) {
		CloseHandle(g_stopEvent);
		g_stopEvent = NULL;
	}
}
