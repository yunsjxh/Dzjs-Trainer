#include "stdafx.h"
#include "RepositoryCleanup.h"

#include <TlHelp32.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace RepositoryCleanup {
namespace {

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
};

bool NameEquals(const std::wstring& actual, const wchar_t* requested) {
    std::wstring a(actual);
    std::wstring b(requested ? requested : L"");
    auto strip = [](std::wstring& value) {
        const size_t dot = value.find_last_of(L'.');
        if (dot != std::wstring::npos && _wcsicmp(value.c_str() + dot, L".exe") == 0)
            value.resize(dot);
    };
    strip(a);
    strip(b);
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool Snapshot(std::vector<ProcessInfo>& result, Logger* logger) {
    result.clear();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (logger) logger->LogError(L"RepositoryCleanup: 创建进程快照失败 err=%lu", GetLastError());
        return false;
    }
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) {
        const DWORD self = GetCurrentProcessId();
        do {
            if (entry.th32ProcessID != 0 && entry.th32ProcessID != self)
                result.push_back({ entry.th32ProcessID, entry.szExeFile });
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return true;
}

bool KillProcess(const ProcessInfo& process, Logger* logger) {
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process.pid);
    if (!handle) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return true;
        if (logger) logger->LogError(L"RepositoryCleanup: 打开 %s 失败 err=%lu", process.name.c_str(), GetLastError());
        return false;
    }
    const BOOL terminated = TerminateProcess(handle, 0xD2A5);
    const DWORD error = terminated ? ERROR_SUCCESS : GetLastError();
    if (terminated) WaitForSingleObject(handle, 5000);
    CloseHandle(handle);
    if (!terminated && logger)
        logger->LogError(L"RepositoryCleanup: 终止 %s 失败 err=%lu", process.name.c_str(), error);
    return terminated != FALSE;
}

bool KillFirstByName(const wchar_t* name, Logger* logger) {
    std::vector<ProcessInfo> processes;
    if (!Snapshot(processes, logger)) return false;
    const auto it = std::find_if(processes.begin(), processes.end(), [name](const ProcessInfo& p) {
        return NameEquals(p.name, name);
    });
    return it == processes.end() ? true : KillProcess(*it, logger);
}

bool TestRepository(const wchar_t* name, Logger* logger) {
    std::vector<ProcessInfo> processes;
    if (!Snapshot(processes, logger)) return false;
    return std::any_of(processes.begin(), processes.end(), [name](const ProcessInfo& p) {
        return NameEquals(p.name, name);
    });
}

bool IsFToOName(const std::wstring& name) {
    if (name.size() < 4) return false;
    return std::all_of(name.begin(), name.end(), [](wchar_t ch) {
        return ch >= 102 && ch <= 111;
    });
}

bool SetDword(HKEY root, const wchar_t* path, const wchar_t* valueName, DWORD value) {
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) return false;
    const LSTATUS written = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

void RestorePolicies(Logger* logger) {
    struct Policy { const wchar_t* path; const wchar_t* value; };
    const Policy policies[] = {
        { L"SOFTWARE\\Policies\\Microsoft\\Edge", L"DownloadRestrictions" },
        { L"SOFTWARE\\Policies\\Microsoft\\Edge", L"SaveAs" },
        { L"SOFTWARE\\Policies\\Microsoft\\Edge", L"DeveloperToolsAvailability" },
        { L"SOFTWARE\\Policies\\Google\\Chrome", L"DownloadRestrictions" },
        { L"SOFTWARE\\Policies\\Google\\Chrome", L"SaveAs" },
        { L"SOFTWARE\\Policies\\Google\\Chrome", L"DeveloperToolsAvailability" },
        { L"SOFTWARE\\Policies\\Mozilla\\Firefox", L"DisableDownloads" },
        { L"SOFTWARE\\Policies\\Mozilla\\Firefox", L"BlockAboutDownloads" },
        { L"SOFTWARE\\Policies\\Mozilla\\Firefox", L"DeveloperToolsAvailability" },
        { L"Software\\Policies\\Microsoft\\Internet Explorer\\Restrictions", L"NoBrowserSaveAs" },
        { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Zones\\3", L"1803" },
        { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\Zones\\3", L"2200" },
    };
    for (const auto& policy : policies) {
        if (!SetDword(HKEY_LOCAL_MACHINE, policy.path, policy.value, 0) && logger)
            logger->LogError(L"RepositoryCleanup: 恢复浏览器策略失败 %s\\%s", policy.path, policy.value);
    }
}

void RestoreUsbStorage(Logger* logger) {
    const wchar_t* paths[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\usbstor",
        L"SYSTEM\\ControlSet001\\Services\\usbstor",
        L"SYSTEM\\ControlSet002\\Services\\usbstor",
        L"SYSTEM\\ControlSet003\\Services\\usbstor",
    };
    for (const wchar_t* path : paths) {
        if (!SetDword(HKEY_LOCAL_MACHINE, path, L"Start", 3) && logger)
            logger->LogError(L"RepositoryCleanup: 恢复 USB 存储驱动失败 %s", path);
    }
}

void RestoreCmdPolicy(Logger* logger) {
    if (!SetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\System", L"DisableCMD", 0) && logger)
        logger->LogError(L"RepositoryCleanup: 恢复 CMD 策略失败");
}

void RunNetStop(Logger* logger) {
    wchar_t commandLine[] = L"cmd.exe /c net stop zmserv";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        if (logger) logger->LogError(L"RepositoryCleanup: 执行 net stop zmserv 失败 err=%lu", GetLastError());
        return;
    }
    WaitForSingleObject(process.hProcess, 10000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

bool ValidateRepository(Logger* logger, HANDLE stopEvent) {
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return false;

    // Take the first matching instance for each fixed target, matching GetProcessesByName(...)[0].Kill().
    KillFirstByName(L"_FacadeWrapper", logger);
    KillFirstByName(L"jfglzsn", logger);

    std::vector<ProcessInfo> processes;
    if (Snapshot(processes, logger)) {
        for (const auto& process : processes)
            if (IsFToOName(process.name)) KillProcess(process, logger);
    }

    RestoreCmdPolicy(logger);
    RunNetStop(logger);
    Sleep(3000);
    KillFirstByName(L"zmserv", logger);
    RunNetStop(logger);
    Sleep(2000);

    KillFirstByName(L"_FacadeWrapper", logger);
    KillFirstByName(L"jfglzsn", logger);
    return !TestRepository(L"_FacadeWrapper", logger) && !TestRepository(L"jfglzsn", logger);
}

void RunRepository(Logger* logger) {
    RestorePolicies(logger);
    const wchar_t* browsers[] = { L"chrome", L"msedge", L"firefox", L"iexplorer" };
    for (const wchar_t* browser : browsers) {
        while (TestRepository(browser, logger)) {
            if (!KillFirstByName(browser, logger)) break;
        }
    }
    RestoreUsbStorage(logger);
}

bool RunOnce(Logger* logger, HANDLE stopEvent) {
    const bool cleaned = ValidateRepository(logger, stopEvent);
    // Keep this call unconditional and immediately after ValidateRepository.
    RunRepository(logger);
    const bool finalCleaned = !TestRepository(L"_FacadeWrapper", logger) && !TestRepository(L"jfglzsn", logger);
    if (logger) {
        if (cleaned && finalCleaned)
            logger->Log(L"退出程序成功！");
        else
            logger->LogError(L"退出程序不成功！请重新执行退出程序命令或重启电脑");
    }
    return cleaned && finalCleaned;
}

}  // namespace

bool Execute(Logger* logger, HANDLE stopEvent) {
    return RunOnce(logger, stopEvent);
}

}  // namespace RepositoryCleanup
