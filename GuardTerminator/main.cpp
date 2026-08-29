#include "config.h"

#include <Windows.h>
#include <Shellapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {

constexpr WORD kColorDefault = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD kColorHeader = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD kColorInfo = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD kColorSuccess = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kColorWarning = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
constexpr WORD kColorError = FOREGROUND_RED | FOREGROUND_INTENSITY;
constexpr ULONG kProcessBreakOnTermination = 29;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((LONG)(Status)) >= 0)
#endif

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE Release() noexcept {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    void Reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class UniqueServiceHandle {
public:
    UniqueServiceHandle() noexcept = default;
    explicit UniqueServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueServiceHandle() {
        Reset();
    }

    UniqueServiceHandle(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;

    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept : handle_(other.Release()) {}

    UniqueServiceHandle& operator=(UniqueServiceHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    SC_HANDLE Release() noexcept {
        SC_HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    void Reset(SC_HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
        handle_ = handle;
    }

private:
    SC_HANDLE handle_ = nullptr;
};

class Console {
public:
    Console() {
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        is_console_ = output_ != INVALID_HANDLE_VALUE && output_ != nullptr && GetConsoleMode(output_, &mode) != FALSE;

        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (is_console_ && GetConsoleScreenBufferInfo(output_, &info) != FALSE) {
            default_color_ = info.wAttributes;
        }

        SetConsoleTitleW(L"GuardTerminator - 恶意守护进程同步结束工具");
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    void Banner() {
        std::lock_guard<std::mutex> lock(mutex_);
        SetColor(kColorHeader);
        WriteRaw(L"╔════════════════════════════════════════════════════════════════════╗\n");
        WriteRaw(L"║              GuardTerminator  恶意守护进程同步结束工具             ║\n");
        WriteRaw(L"╚════════════════════════════════════════════════════════════════════╝\n");
        RestoreColor();
    }

    void Section(const std::wstring& title) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetColor(kColorHeader);
        WriteRaw(L"\n┌─ ");
        WriteRaw(title);
        WriteRaw(L" ");
        WriteRaw(std::wstring(60 > title.size() ? 60 - title.size() : 2, L'─'));
        WriteRaw(L"\n");
        RestoreColor();
    }

    void Info(const std::wstring& message) {
        Line(L"[信息] ", message, kColorInfo);
    }

    void Success(const std::wstring& message) {
        Line(L"[成功] ", message, kColorSuccess);
    }

    void Warn(const std::wstring& message) {
        Line(L"[警告] ", message, kColorWarning);
    }

    void Error(const std::wstring& message) {
        Line(L"[错误] ", message, kColorError);
    }

    void Muted(const std::wstring& message) {
        Line(L"       ", message, default_color_);
    }

    void KeyValue(const std::wstring& key, const std::wstring& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetColor(kColorInfo);
        WriteRaw(L"  ");
        WriteRaw(key);
        WriteRaw(L"：");
        RestoreColor();
        WriteRaw(value);
        WriteRaw(L"\n");
    }

private:
    void Line(const std::wstring& prefix, const std::wstring& message, WORD color) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetColor(color);
        WriteRaw(prefix);
        WriteRaw(message);
        WriteRaw(L"\n");
        RestoreColor();
    }

    void SetColor(WORD color) {
        if (is_console_) {
            SetConsoleTextAttribute(output_, color);
        }
    }

    void RestoreColor() {
        if (is_console_) {
            SetConsoleTextAttribute(output_, default_color_);
        }
    }

    void WriteRaw(const std::wstring& text) {
        if (text.empty()) {
            return;
        }

        if (is_console_) {
            DWORD written = 0;
            WriteConsoleW(output_, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
            return;
        }

        const int needed = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.c_str(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (needed <= 0) {
            return;
        }

        std::string utf8(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.c_str(),
            static_cast<int>(text.size()),
            utf8.data(),
            needed,
            nullptr,
            nullptr);

        DWORD written = 0;
        WriteFile(output_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    HANDLE output_ = nullptr;
    bool is_console_ = false;
    WORD default_color_ = kColorDefault;
    std::mutex mutex_;
};

struct Options {
    bool dry_run = false;
    bool no_pause = false;
    bool help = false;
};

struct ProcessInfo {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    std::wstring image_name;
};

struct TerminationTarget {
    DWORD pid = 0;
    std::wstring name;
    UniqueHandle handle;
};

class NtProcessApi {
public:
    bool Load(std::wstring& error) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) {
            ntdll = LoadLibraryW(L"ntdll.dll");
        }
        if (ntdll == nullptr) {
            error = L"无法加载 ntdll.dll";
            return false;
        }

        query_ = reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
        set_ = reinterpret_cast<NtSetInformationProcessFn>(
            GetProcAddress(ntdll, "NtSetInformationProcess"));

        if (query_ == nullptr || set_ == nullptr) {
            error = L"无法解析 NtQueryInformationProcess 或 NtSetInformationProcess";
            return false;
        }

        return true;
    }

    LONG QueryBreakOnTermination(HANDLE process, ULONG& value) const {
        ULONG return_length = 0;
        return query_(process, kProcessBreakOnTermination, &value, sizeof(value), &return_length);
    }

    LONG SetBreakOnTermination(HANDLE process, ULONG value) const {
        return set_(process, kProcessBreakOnTermination, &value, sizeof(value));
    }

private:
    using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using NtSetInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);

    NtQueryInformationProcessFn query_ = nullptr;
    NtSetInformationProcessFn set_ = nullptr;
};

std::wstring ToString(DWORD value) {
    return std::to_wstring(static_cast<unsigned long long>(value));
}

std::wstring ToString(UINT value) {
    return std::to_wstring(static_cast<unsigned long long>(value));
}

std::wstring FormatWin32Error(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (size != 0 && buffer != nullptr) {
        message.assign(buffer, size);
        LocalFree(buffer);
    } else {
        message = L"未知错误";
    }

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }

    return message + L"（错误码 " + ToString(error) + L"）";
}

std::wstring FormatNtStatus(LONG status) {
    std::wstringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<unsigned long>(status);
    return stream.str();
}

std::wstring ProcessLabel(const ProcessInfo& process) {
    return process.image_name + L"（PID=" + ToString(process.pid) + L"，PPID=" + ToString(process.parent_pid) + L"）";
}

std::wstring ServiceStateToString(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED:
        return L"已停止";
    case SERVICE_START_PENDING:
        return L"正在启动";
    case SERVICE_STOP_PENDING:
        return L"正在停止";
    case SERVICE_RUNNING:
        return L"正在运行";
    case SERVICE_CONTINUE_PENDING:
        return L"继续挂起";
    case SERVICE_PAUSE_PENDING:
        return L"暂停挂起";
    case SERVICE_PAUSED:
        return L"已暂停";
    default:
        return L"未知状态 " + ToString(state);
    }
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(::towlower(ch));
    });
    return value;
}

bool IsLikelyRandomGuardianName(const std::wstring& image_name) {
    const std::wstring lower = ToLower(image_name);
    constexpr wchar_t suffix[] = L".exe";
    if (lower.size() != 9 || lower.substr(5) != suffix) {
        return false;
    }

    for (std::size_t index = 0; index < 5; ++index) {
        if (lower[index] < L'a' || lower[index] > L'z') {
            return false;
        }
    }

    return true;
}

bool ParseOptions(int argc, wchar_t* argv[], Options& options, std::wstring& error) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (EqualsIgnoreCase(arg, L"--dry-run")) {
            options.dry_run = true;
        } else if (EqualsIgnoreCase(arg, L"--no-pause")) {
            options.no_pause = true;
        } else if (EqualsIgnoreCase(arg, L"--help") || EqualsIgnoreCase(arg, L"-h") || EqualsIgnoreCase(arg, L"/?")) {
            options.help = true;
        } else {
            error = L"未知参数：" + arg;
            return false;
        }
    }

    return true;
}

void PrintHelp(Console& console) {
    console.Banner();
    console.Section(L"命令行参数");
    console.KeyValue(L"--dry-run", L"演练模式：只枚举、校验和展示，不结束进程、不停止或删除服务");
    console.KeyValue(L"--no-pause", L"退出前不等待回车，适合脚本调用");
    console.KeyValue(L"--help", L"显示本帮助");
    console.Section(L"当前配置");
    console.KeyValue(L"固定守护进程", config::kKnownGuardProcessName);
    console.KeyValue(L"后台关键进程", config::kCriticalProcessName);
    console.KeyValue(L"关联服务", config::kServiceName);
    console.KeyValue(L"重启等待时间", ToString(config::kRespawnTimeoutMs / 1000) + L" 秒");
    console.KeyValue(L"随机进程名校验", config::kRequireRandomGuardianNamePattern ? L"开启（5 位小写字母 + .exe）" : L"关闭");
}

std::wstring QuoteArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = false;
    for (const wchar_t ch : argument) {
        if (::iswspace(ch) != 0 || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        return argument;
    }

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
            continue;
        }

        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring BuildParameterString(int argc, wchar_t* argv[]) {
    std::wstring parameters;
    for (int index = 1; index < argc; ++index) {
        if (!parameters.empty()) {
            parameters.push_back(L' ');
        }
        parameters += QuoteArgument(argv[index]);
    }
    return parameters;
}

bool IsRunAsAdministrator() {
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID administrators_group = nullptr;
    const BOOL allocated = AllocateAndInitializeSid(
        &nt_authority,
        2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0,
        0,
        0,
        0,
        0,
        0,
        &administrators_group);
    if (allocated == FALSE) {
        return false;
    }

    BOOL is_member = FALSE;
    const BOOL checked = CheckTokenMembership(nullptr, administrators_group, &is_member);
    FreeSid(administrators_group);
    return checked != FALSE && is_member != FALSE;
}

bool RelaunchElevated(int argc, wchar_t* argv[], Console& console) {
    wchar_t module_path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (length == 0 || length >= std::size(module_path)) {
        console.Error(L"无法获取当前程序路径：" + FormatWin32Error(GetLastError()));
        return false;
    }

    const std::wstring parameters = BuildParameterString(argc, argv);
    SHELLEXECUTEINFOW execute_info{};
    execute_info.cbSize = sizeof(execute_info);
    execute_info.lpVerb = L"runas";
    execute_info.lpFile = module_path;
    execute_info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    execute_info.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&execute_info) == FALSE) {
        console.Error(L"请求管理员权限失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    console.Success(L"已发起 UAC 提权，新窗口将继续执行。当前实例退出。");
    return true;
}

bool EnableDebugPrivilege(Console& console) {
    UniqueHandle token;
    HANDLE raw_token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw_token) == FALSE) {
        console.Error(L"打开当前进程令牌失败：" + FormatWin32Error(GetLastError()));
        return false;
    }
    token.Reset(raw_token);

    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid) == FALSE) {
        console.Error(L"查找 SeDebugPrivilege 失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (AdjustTokenPrivileges(token.Get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr) == FALSE) {
        console.Error(L"启用 SeDebugPrivilege 失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    const DWORD last_error = GetLastError();
    if (last_error == ERROR_NOT_ALL_ASSIGNED) {
        console.Error(L"当前令牌未分配 SeDebugPrivilege，无法继续。");
        return false;
    }

    console.Success(L"已启用 SeDebugPrivilege。");
    return true;
}

bool SnapshotProcesses(std::vector<ProcessInfo>& processes, std::wstring& error) {
    processes.clear();

    UniqueHandle snapshot;
    for (int attempt = 0; attempt < 3; ++attempt) {
        snapshot.Reset(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot) {
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!snapshot) {
        error = FormatWin32Error(GetLastError());
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.Get(), &entry) == FALSE) {
        error = FormatWin32Error(GetLastError());
        return false;
    }

    const DWORD current_pid = GetCurrentProcessId();
    do {
        if (entry.th32ProcessID == current_pid) {
            continue;
        }

        ProcessInfo info{};
        info.pid = entry.th32ProcessID;
        info.parent_pid = entry.th32ParentProcessID;
        info.image_name = entry.szExeFile;
        processes.push_back(std::move(info));
    } while (Process32NextW(snapshot.Get(), &entry) != FALSE);

    return true;
}

std::vector<ProcessInfo> FindProcessesByName(const std::vector<ProcessInfo>& processes, const std::wstring& image_name) {
    std::vector<ProcessInfo> result;
    for (const ProcessInfo& process : processes) {
        if (EqualsIgnoreCase(process.image_name, image_name)) {
            result.push_back(process);
        }
    }
    return result;
}

std::optional<ProcessInfo> FindProcessByPid(const std::vector<ProcessInfo>& processes, DWORD pid) {
    const auto iterator = std::find_if(processes.begin(), processes.end(), [pid](const ProcessInfo& process) {
        return process.pid == pid;
    });
    if (iterator == processes.end()) {
        return std::nullopt;
    }
    return *iterator;
}

bool TerminateProcessAndWait(const ProcessInfo& process, Console& console) {
    UniqueHandle handle(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid));
    if (!handle) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            console.Warn(ProcessLabel(process) + L" 已不存在，视为已结束。");
            return true;
        }
        console.Error(L"打开进程失败：" + ProcessLabel(process) + L"；" + FormatWin32Error(error));
        return false;
    }

    if (TerminateProcess(handle.Get(), config::kTerminateExitCode) == FALSE) {
        const DWORD error = GetLastError();
        if (WaitForSingleObject(handle.Get(), 0) == WAIT_OBJECT_0) {
            console.Warn(ProcessLabel(process) + L" 已在终止请求前退出。");
            return true;
        }
        console.Error(L"终止进程失败：" + ProcessLabel(process) + L"；" + FormatWin32Error(error));
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(handle.Get(), config::kTerminateWaitMs);
    if (wait_result == WAIT_OBJECT_0) {
        console.Success(L"已结束 " + ProcessLabel(process));
        return true;
    }

    console.Error(L"已发送终止请求，但等待退出超时：" + ProcessLabel(process));
    return false;
}

std::optional<ProcessInfo> WaitForRespawnedProtector(const std::set<DWORD>& ignored_pids, Console& console) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config::kRespawnTimeoutMs);
    auto next_progress = start + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() - start < timeout) {
        std::vector<ProcessInfo> processes;
        std::wstring error;
        if (!SnapshotProcesses(processes, error)) {
            console.Warn(L"进程快照失败，继续等待：" + error);
        } else {
            const std::vector<ProcessInfo> protectors = FindProcessesByName(processes, config::kKnownGuardProcessName);
            for (const ProcessInfo& process : protectors) {
                if (ignored_pids.find(process.pid) == ignored_pids.end()) {
                    return process;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            const auto remaining = static_cast<long long>(config::kRespawnTimeoutMs / 1000) - elapsed;
            console.Muted(L"仍在等待 " + std::wstring(config::kKnownGuardProcessName) + L" 被重新拉起，剩余约 " +
                          std::to_wstring(remaining > 0 ? remaining : 0) + L" 秒。");
            next_progress = now + std::chrono::seconds(5);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(config::kPollIntervalMs));
    }

    return std::nullopt;
}

bool ValidateGuardianProcess(const ProcessInfo& guardian, Console& console) {
    if (!config::kRequireRandomGuardianNamePattern) {
        return true;
    }

    if (IsLikelyRandomGuardianName(guardian.image_name)) {
        return true;
    }

    console.Error(L"识别到的父进程不符合“5 位小写字母 + .exe”安全校验，拒绝继续：" + ProcessLabel(guardian));
    return false;
}

bool AddProcessTarget(
    const ProcessInfo& process,
    const std::wstring& role,
    DWORD access,
    std::vector<TerminationTarget>& targets,
    std::set<DWORD>& registered_pids,
    Console& console) {
    if (process.pid == 0 || process.pid == GetCurrentProcessId()) {
        console.Warn(L"跳过无效或当前进程目标：" + ProcessLabel(process));
        return true;
    }

    if (registered_pids.find(process.pid) != registered_pids.end()) {
        return true;
    }

    UniqueHandle handle(OpenProcess(access, FALSE, process.pid));
    if (!handle) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            console.Warn(L"目标已不存在，跳过：" + ProcessLabel(process));
            return true;
        }
        console.Error(L"打开最终目标失败：" + role + L" " + ProcessLabel(process) + L"；" + FormatWin32Error(error));
        return false;
    }

    TerminationTarget target{};
    target.pid = process.pid;
    target.name = role + L" " + ProcessLabel(process);
    target.handle = std::move(handle);
    targets.push_back(std::move(target));
    registered_pids.insert(process.pid);
    console.Success(L"已锁定最终目标：" + role + L" " + ProcessLabel(process));
    return true;
}

bool QueryBreakOnTerminationForDisplay(
    const NtProcessApi& nt_api,
    const ProcessInfo& process,
    Console& console) {
    UniqueHandle handle(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process.pid));
    if (!handle) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            console.Warn(ProcessLabel(process) + L" 已不存在，跳过关键标识查询。");
            return true;
        }
        console.Warn(L"无法打开进程查询关键标识：" + ProcessLabel(process) + L"；" + FormatWin32Error(error));
        return false;
    }

    ULONG break_on_termination = 0;
    const LONG status = nt_api.QueryBreakOnTermination(handle.Get(), break_on_termination);
    if (!NT_SUCCESS(status)) {
        console.Warn(L"查询 ProcessBreakOnTermination 失败：" + ProcessLabel(process) + L"；NTSTATUS=" + FormatNtStatus(status));
        return false;
    }

    console.KeyValue(
        L"关键标识 " + ProcessLabel(process),
        break_on_termination == 0 ? L"关闭" : L"开启");
    return true;
}

bool PrepareCriticalProcessTargets(
    const std::vector<ProcessInfo>& key_processes,
    const NtProcessApi& nt_api,
    std::vector<TerminationTarget>& targets,
    std::set<DWORD>& registered_pids,
    Console& console) {
    for (const ProcessInfo& process : key_processes) {
        if (registered_pids.find(process.pid) != registered_pids.end()) {
            continue;
        }

        UniqueHandle handle(OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            process.pid));
        if (!handle) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                console.Warn(ProcessLabel(process) + L" 已不存在，跳过关键标识处理。");
                continue;
            }
            console.Error(L"打开关键进程失败：" + ProcessLabel(process) + L"；" + FormatWin32Error(error));
            return false;
        }

        ULONG break_on_termination = 0;
        LONG status = nt_api.QueryBreakOnTermination(handle.Get(), break_on_termination);
        if (NT_SUCCESS(status)) {
            console.Info(L"当前关键标识：" + ProcessLabel(process) + L" => " +
                         (break_on_termination == 0 ? L"关闭" : L"开启"));
        } else {
            console.Warn(L"查询关键标识失败，将尝试直接写入关闭状态：" + ProcessLabel(process) +
                         L"；NTSTATUS=" + FormatNtStatus(status));
        }

        if (!NT_SUCCESS(status) || break_on_termination != 0) {
            ULONG disabled = 0;
            status = nt_api.SetBreakOnTermination(handle.Get(), disabled);
            if (!NT_SUCCESS(status)) {
                console.Error(L"清除关键进程标识失败，已中止最终终止动作：" + ProcessLabel(process) +
                              L"；NTSTATUS=" + FormatNtStatus(status));
                return false;
            }
            console.Success(L"已清除关键进程标识：" + ProcessLabel(process));
        }

        TerminationTarget target{};
        target.pid = process.pid;
        target.name = L"后台关键进程 " + ProcessLabel(process);
        target.handle = std::move(handle);
        targets.push_back(std::move(target));
        registered_pids.insert(process.pid);
    }

    return true;
}

bool QueryServiceStatus(SC_HANDLE service, SERVICE_STATUS_PROCESS& status, Console& console) {
    DWORD bytes_needed = 0;
    if (QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytes_needed) == FALSE) {
        console.Error(L"查询服务状态失败：" + FormatWin32Error(GetLastError()));
        return false;
    }
    return true;
}

bool InspectService(Console& console) {
    UniqueServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        console.Warn(L"打开服务控制管理器失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    UniqueServiceHandle service(OpenServiceW(scm.Get(), config::kServiceName, SERVICE_QUERY_STATUS));
    if (!service) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            console.Warn(std::wstring(L"服务不存在：") + config::kServiceName);
            return true;
        }
        console.Warn(std::wstring(L"打开服务失败：") + config::kServiceName + L"；" + FormatWin32Error(error));
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatus(service.Get(), status, console)) {
        return false;
    }

    console.KeyValue(std::wstring(L"服务 ") + config::kServiceName, ServiceStateToString(status.dwCurrentState));
    return true;
}

bool WaitForServiceStopped(SC_HANDLE service, Console& console) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config::kServiceStopTimeoutMs);

    while (std::chrono::steady_clock::now() - start < timeout) {
        SERVICE_STATUS_PROCESS status{};
        if (!QueryServiceStatus(service, status, console)) {
            return false;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            console.Success(std::wstring(L"服务已停止：") + config::kServiceName);
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    console.Warn(std::wstring(L"等待服务停止超时：") + config::kServiceName);
    return false;
}

bool StopAndDeleteService(Console& console) {
    UniqueServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        console.Error(L"打开服务控制管理器失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    UniqueServiceHandle service(OpenServiceW(
        scm.Get(),
        config::kServiceName,
        SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
    if (!service) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            console.Warn(std::wstring(L"服务不存在，无需删除：") + config::kServiceName);
            return true;
        }
        console.Error(std::wstring(L"打开服务失败：") + config::kServiceName + L"；" + FormatWin32Error(error));
        return false;
    }

    bool stop_ok = true;
    SERVICE_STATUS_PROCESS status{};
    if (QueryServiceStatus(service.Get(), status, console)) {
        console.Info(std::wstring(L"服务当前状态：") + config::kServiceName + L" => " + ServiceStateToString(status.dwCurrentState));
        if (status.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS control_status{};
            if (ControlService(service.Get(), SERVICE_CONTROL_STOP, &control_status) == FALSE) {
                const DWORD error = GetLastError();
                if (error == ERROR_SERVICE_NOT_ACTIVE) {
                    console.Warn(std::wstring(L"服务已经不在运行：") + config::kServiceName);
                } else {
                    console.Warn(std::wstring(L"发送停止服务请求失败：") + config::kServiceName + L"；" + FormatWin32Error(error));
                    stop_ok = false;
                }
            } else {
                console.Info(std::wstring(L"已发送停止服务请求：") + config::kServiceName);
            }

            if (!WaitForServiceStopped(service.Get(), console)) {
                stop_ok = false;
            }
        }
    } else {
        stop_ok = false;
    }

    if (DeleteService(service.Get()) == FALSE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_MARKED_FOR_DELETE) {
            console.Warn(std::wstring(L"服务已标记为删除：") + config::kServiceName);
            return stop_ok;
        }

        console.Error(std::wstring(L"删除服务失败：") + config::kServiceName + L"；" + FormatWin32Error(error));
        return false;
    }

    console.Success(std::wstring(L"已删除服务：") + config::kServiceName);
    return stop_ok;
}

bool TerminateTargetFromThread(HANDLE process, const std::wstring& name, Console& console) {
    if (TerminateProcess(process, config::kTerminateExitCode) == FALSE) {
        const DWORD error = GetLastError();
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            console.Warn(name + L" 已在最终动作前退出。");
            return true;
        }
        console.Error(L"最终终止失败：" + name + L"；" + FormatWin32Error(error));
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process, config::kTerminateWaitMs);
    if (wait_result == WAIT_OBJECT_0) {
        console.Success(L"最终已结束：" + name);
        return true;
    }

    console.Warn(L"已发送最终终止请求，但等待退出超时：" + name);
    return false;
}

bool ExecuteFinalActions(std::vector<TerminationTarget>& targets, Console& console) {
    if (targets.empty()) {
        console.Warn(L"没有可锁定的进程目标，将仅尝试停止并删除服务。");
    }

    UniqueHandle start_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!start_event) {
        console.Error(L"创建同步事件失败：" + FormatWin32Error(GetLastError()));
        return false;
    }

    std::atomic_bool all_ok = true;
    std::vector<std::thread> workers;
    workers.reserve(targets.size() + 1);

    for (const TerminationTarget& target : targets) {
        workers.emplace_back([&, process = target.handle.Get(), name = target.name]() {
            WaitForSingleObject(start_event.Get(), INFINITE);
            if (!TerminateTargetFromThread(process, name, console)) {
                all_ok.store(false);
            }
        });
    }

    workers.emplace_back([&]() {
        WaitForSingleObject(start_event.Get(), INFINITE);
        if (!StopAndDeleteService(console)) {
            all_ok.store(false);
        }
    });

    console.Info(L"最终动作线程已就绪，将同步结束目标进程并处理服务。");
    SetEvent(start_event.Get());

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    return all_ok.load();
}

int RunDryRun(const NtProcessApi& nt_api, Console& console) {
    console.Section(L"演练模式：不执行破坏性动作");

    std::vector<ProcessInfo> processes;
    std::wstring error;
    if (!SnapshotProcesses(processes, error)) {
        console.Error(L"进程枚举失败：" + error);
        return 1;
    }

    const std::vector<ProcessInfo> protectors = FindProcessesByName(processes, config::kKnownGuardProcessName);
    if (protectors.empty()) {
        console.Warn(std::wstring(L"未发现固定守护进程：") + config::kKnownGuardProcessName);
    } else {
        for (const ProcessInfo& protector : protectors) {
            console.KeyValue(L"固定守护进程", ProcessLabel(protector));
            const std::optional<ProcessInfo> parent = FindProcessByPid(processes, protector.parent_pid);
            if (parent.has_value()) {
                console.KeyValue(L"当前父进程", ProcessLabel(*parent));
                if (IsLikelyRandomGuardianName(parent->image_name)) {
                    console.Success(L"父进程符合随机 5 位小写字母命名特征。");
                } else {
                    console.Warn(L"父进程当前不符合随机 5 位小写字母命名特征；正式模式会重新触发并校验。");
                }
            } else {
                console.Warn(L"未能在当前快照中找到固定守护进程的父进程。");
            }
        }
    }

    const std::vector<ProcessInfo> key_processes = FindProcessesByName(processes, config::kCriticalProcessName);
    if (key_processes.empty()) {
        console.Warn(std::wstring(L"未发现后台关键进程：") + config::kCriticalProcessName);
    } else {
        for (const ProcessInfo& process : key_processes) {
            QueryBreakOnTerminationForDisplay(nt_api, process, console);
        }
    }

    InspectService(console);
    console.Success(L"演练完成，未结束任何进程，未停止或删除任何服务。");
    return 0;
}

bool CollectGuardianTargets(
    const std::vector<ProcessInfo>& snapshot,
    const std::vector<ProcessInfo>& protectors,
    std::vector<ProcessInfo>& guardians,
    Console& console) {
    std::set<DWORD> guardian_pids;
    for (const ProcessInfo& protector : protectors) {
        const std::optional<ProcessInfo> parent = FindProcessByPid(snapshot, protector.parent_pid);
        if (!parent.has_value()) {
            console.Error(L"未能在最终快照中找到固定守护进程的父进程：" + ProcessLabel(protector));
            return false;
        }

        if (!ValidateGuardianProcess(*parent, console)) {
            return false;
        }

        if (guardian_pids.insert(parent->pid).second) {
            guardians.push_back(*parent);
        }
    }
    return true;
}

int RunRemediation(Console& console) {
    NtProcessApi nt_api;
    std::wstring nt_error;
    if (!nt_api.Load(nt_error)) {
        console.Error(nt_error);
        return 1;
    }

    console.Section(L"配置");
    console.KeyValue(L"固定守护进程", config::kKnownGuardProcessName);
    console.KeyValue(L"后台关键进程", config::kCriticalProcessName);
    console.KeyValue(L"关联服务", config::kServiceName);
    console.KeyValue(L"等待重新拉起", ToString(config::kRespawnTimeoutMs / 1000) + L" 秒");

    console.Section(L"第一步：结束固定守护进程");
    std::vector<ProcessInfo> initial_processes;
    std::wstring error;
    if (!SnapshotProcesses(initial_processes, error)) {
        console.Error(L"进程枚举失败：" + error);
        return 1;
    }

    const std::vector<ProcessInfo> initial_protectors = FindProcessesByName(initial_processes, config::kKnownGuardProcessName);
    std::set<DWORD> ignored_protector_pids;
    if (initial_protectors.empty()) {
        console.Warn(std::wstring(L"未发现固定守护进程：") + config::kKnownGuardProcessName + L"。仍将等待新实例出现。");
    } else {
        bool all_terminated = true;
        for (const ProcessInfo& protector : initial_protectors) {
            ignored_protector_pids.insert(protector.pid);
            if (!TerminateProcessAndWait(protector, console)) {
                all_terminated = false;
            }
        }
        if (!all_terminated) {
            console.Error(L"固定守护进程未能全部结束，拒绝进入最终同步阶段。");
            return 1;
        }
    }

    console.Section(L"第二步：等待并识别随机守护进程");
    const std::optional<ProcessInfo> respawned = WaitForRespawnedProtector(ignored_protector_pids, console);
    if (!respawned.has_value()) {
        console.Error(std::wstring(L"超时：") + ToString(config::kRespawnTimeoutMs / 1000) +
                      L" 秒内未发现重新拉起的 " + config::kKnownGuardProcessName);
        return 1;
    }
    console.Success(L"已发现重新拉起的固定守护进程：" + ProcessLabel(*respawned));

    std::vector<ProcessInfo> after_respawn;
    if (!SnapshotProcesses(after_respawn, error)) {
        console.Error(L"重新枚举进程失败：" + error);
        return 1;
    }

    const std::optional<ProcessInfo> first_guardian = FindProcessByPid(after_respawn, respawned->parent_pid);
    if (!first_guardian.has_value()) {
        console.Error(L"未能找到重新拉起实例的父进程，无法确认随机守护进程。");
        return 1;
    }
    if (!ValidateGuardianProcess(*first_guardian, console)) {
        return 1;
    }
    console.Success(L"已识别随机守护进程：" + ProcessLabel(*first_guardian));

    console.Section(L"第三步：清除后台关键进程标识");
    const std::vector<ProcessInfo> key_processes = FindProcessesByName(after_respawn, config::kCriticalProcessName);

    std::vector<TerminationTarget> targets;
    std::set<DWORD> registered_pids;
    if (key_processes.empty()) {
        console.Warn(std::wstring(L"未发现后台关键进程：") + config::kCriticalProcessName);
    } else if (!PrepareCriticalProcessTargets(key_processes, nt_api, targets, registered_pids, console)) {
        return 1;
    }

    console.Section(L"第四步：锁定最终同步目标");
    std::vector<ProcessInfo> final_snapshot;
    if (!SnapshotProcesses(final_snapshot, error)) {
        console.Error(L"最终快照失败：" + error);
        return 1;
    }

    const std::vector<ProcessInfo> final_protectors = FindProcessesByName(final_snapshot, config::kKnownGuardProcessName);
    std::vector<ProcessInfo> guardians;
    if (final_protectors.empty()) {
        console.Warn(std::wstring(L"最终快照未发现固定守护进程，将尝试使用已识别随机守护进程：") + ProcessLabel(*first_guardian));
        guardians.push_back(*first_guardian);
    } else if (!CollectGuardianTargets(final_snapshot, final_protectors, guardians, console)) {
        return 1;
    }

    for (const ProcessInfo& guardian : guardians) {
        if (!AddProcessTarget(
                guardian,
                L"随机守护进程",
                PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                targets,
                registered_pids,
                console)) {
            return 1;
        }
    }

    for (const ProcessInfo& protector : final_protectors) {
        if (!AddProcessTarget(
                protector,
                L"固定守护进程",
                PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                targets,
                registered_pids,
                console)) {
            return 1;
        }
    }

    console.Section(L"最终阶段：同步结束并删除服务");
    if (!ExecuteFinalActions(targets, console)) {
        console.Error(L"最终动作存在失败项，请根据上方日志复核。");
        return 1;
    }

    console.Success(L"处置流程完成。");
    return 0;
}

void PauseIfNeeded(const Options& options, Console& console) {
    if (options.no_pause) {
        return;
    }

    console.Muted(L"按回车键退出...");
    std::wstring ignored;
    std::getline(std::wcin, ignored);
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    Console console;

    Options options;
    std::wstring parse_error;
    if (!ParseOptions(argc, argv, options, parse_error)) {
        console.Banner();
        console.Error(parse_error);
        console.Info(L"使用 --help 查看可用参数。");
        PauseIfNeeded(options, console);
        return 2;
    }

    if (options.help) {
        PrintHelp(console);
        return 0;
    }

    console.Banner();

    if (!IsRunAsAdministrator()) {
        console.Warn(L"当前不是管理员权限，正在请求 UAC 提权。");
        const bool relaunched = RelaunchElevated(argc, argv, console);
        if (!relaunched) {
            PauseIfNeeded(options, console);
            return 1;
        }
        return 0;
    }

    console.Success(L"已确认管理员权限。");
    if (!EnableDebugPrivilege(console)) {
        PauseIfNeeded(options, console);
        return 1;
    }

    NtProcessApi nt_api;
    std::wstring nt_error;
    if (!nt_api.Load(nt_error)) {
        console.Error(nt_error);
        PauseIfNeeded(options, console);
        return 1;
    }

    int exit_code = 0;
    if (options.dry_run) {
        exit_code = RunDryRun(nt_api, console);
    } else {
        exit_code = RunRemediation(console);
    }

    PauseIfNeeded(options, console);
    return exit_code;
}
