#include "stdafx.h"
#include "GuardTerminator.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace GuardTerminator {
namespace {

constexpr UINT kTerminateExitCode = 0xD2A5;
constexpr ULONG kProcessBreakOnTermination = 29;
constexpr DWORD kRespawnTimeoutMs = 60'000;
constexpr DWORD kPollIntervalMs = 200;
constexpr DWORD kTerminateWaitMs = 5'000;
constexpr DWORD kServiceStopTimeoutMs = 10'000;

constexpr const wchar_t* kKnownGuardProcess = L"jfglzsn.exe";
constexpr const wchar_t* kCriticalProcess = L"zmserv.exe";
constexpr const wchar_t* kServiceName = L"zmserv";

// Periodic remediation is silent on successful/no-op paths; errors remain
// visible through Logger::LogError.
template <typename... Args>
inline void GuardLogSilent(Args&&...) noexcept {}

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((LONG)(Status)) >= 0)
#endif

// ---- RAII wrappers ----

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) { Reset(o.Release()); }
        return *this;
    }
    HANDLE Get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() noexcept { HANDLE v = h_; h_ = nullptr; return v; }
    void Reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
private:
    HANDLE h_ = nullptr;
};

class UniqueSC {
public:
    UniqueSC() noexcept = default;
    explicit UniqueSC(SC_HANDLE h) noexcept : h_(h) {}
    ~UniqueSC() { Reset(); }
    UniqueSC(const UniqueSC&) = delete;
    UniqueSC& operator=(const UniqueSC&) = delete;
    SC_HANDLE Get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
    void Reset(SC_HANDLE h = nullptr) noexcept {
        if (h_) CloseServiceHandle(h_);
        h_ = h;
    }
private:
    SC_HANDLE h_ = nullptr;
};

// ---- NtProcessApi ----

class NtProcessApi {
    using NtQIP = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using NtSIP = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
public:
    bool Load(Logger* log) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
        if (!ntdll) { log->LogError(L"GuardTerminator: 无法加载 ntdll.dll"); return false; }
        query_ = reinterpret_cast<NtQIP>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
        set_ = reinterpret_cast<NtSIP>(GetProcAddress(ntdll, "NtSetInformationProcess"));
        if (!query_ || !set_) { log->LogError(L"GuardTerminator: 无法解析 NT API"); return false; }
        return true;
    }
    LONG QueryBreakOnTermination(HANDLE p, ULONG& v) const {
        ULONG len = 0;
        return query_(p, kProcessBreakOnTermination, &v, sizeof(v), &len);
    }
    LONG SetBreakOnTermination(HANDLE p, ULONG v) const {
        return set_(p, kProcessBreakOnTermination, &v, sizeof(v));
    }
private:
    NtQIP query_ = nullptr;
    NtSIP set_ = nullptr;
};

// ---- Data types ----

struct PInfo {
    DWORD pid = 0, ppid = 0;
    std::wstring name;
};

struct TTarget {
    DWORD pid = 0;
    std::wstring name;
    UniqueHandle handle;
};

// ---- Helpers ----

std::wstring FmtPid(DWORD v) { return std::to_wstring(v); }
bool EqIC(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool IsRandomGuardian(const std::wstring& name) {
    if (name.size() != 9 || name.substr(5) != L".exe") return false;
    for (int i = 0; i < 5; ++i) if (name[i] < L'a' || name[i] > L'z') return false;
    return true;
}

std::wstring Label(const PInfo& p) {
    return p.name + L"(PID=" + FmtPid(p.pid) + L")";
}

bool Snapshot(std::vector<PInfo>& out, Logger* log) {
    out.clear();
    UniqueHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) { log->LogError(L"GuardTerminator: 创建进程快照失败"); return false; }
    PROCESSENTRY32W e{ sizeof(e) };
    if (!Process32FirstW(snap.Get(), &e)) return false;
    DWORD self = GetCurrentProcessId();
    do {
        if (e.th32ProcessID == self || e.th32ProcessID == 0) continue;
        out.push_back({ e.th32ProcessID, e.th32ParentProcessID, e.szExeFile });
    } while (Process32NextW(snap.Get(), &e));
    return true;
}

std::vector<PInfo> FindByName(const std::vector<PInfo>& procs, const std::wstring& name) {
    std::vector<PInfo> r;
    for (auto& p : procs) if (EqIC(p.name, name)) r.push_back(p);
    return r;
}

std::unique_ptr<PInfo> FindByPid(const std::vector<PInfo>& procs, DWORD pid) {
    auto it = std::find_if(procs.begin(), procs.end(), [pid](const PInfo& p) { return p.pid == pid; });
    if (it == procs.end()) return nullptr;
    return std::make_unique<PInfo>(*it);
}

bool TerminateAndWait(const PInfo& p, Logger* log) {
    UniqueHandle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid));
    if (!h) {
        DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) { GuardLogSilent(L"GuardTerminator: %s 已不存在", Label(p).c_str()); return true; }
        log->LogError(L"GuardTerminator: 打开 %s 失败 err=%d", Label(p).c_str(), e);
        return false;
    }
    if (!TerminateProcess(h.Get(), kTerminateExitCode)) {
        if (WaitForSingleObject(h.Get(), 0) == WAIT_OBJECT_0) { GuardLogSilent(L"GuardTerminator: %s 已退出", Label(p).c_str()); return true; }
        log->LogError(L"GuardTerminator: 终止 %s 失败 err=%d", Label(p).c_str(), GetLastError());
        return false;
    }
    if (WaitForSingleObject(h.Get(), kTerminateWaitMs) == WAIT_OBJECT_0) {
        GuardLogSilent(L"GuardTerminator: 已结束 %s", Label(p).c_str());
        return true;
    }
    log->LogError(L"GuardTerminator: 等待 %s 退出超时", Label(p).c_str());
    return false;
}

std::unique_ptr<PInfo> WaitRespawn(const std::set<DWORD>& ignore, Logger* log,
                                   HANDLE stopEvent, bool* cancelled) {
	if (cancelled) *cancelled = false;
    auto start = std::chrono::steady_clock::now();
    auto next = start + std::chrono::seconds(5);
    auto timeout = std::chrono::milliseconds(kRespawnTimeoutMs);
    while (std::chrono::steady_clock::now() - start < timeout) {
		if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
			if (cancelled) *cancelled = true;
			return nullptr;
		}
        std::vector<PInfo> procs;
        if (Snapshot(procs, log)) {
            auto protectors = FindByName(procs, kKnownGuardProcess);
            for (auto& p : protectors) {
                if (ignore.find(p.pid) == ignore.end()) return std::make_unique<PInfo>(p);
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            auto remain = (timeout - (now - start)).count() / 1000;
            GuardLogSilent(L"GuardTerminator: 等待 %s 重新拉起，剩余 %lld 秒", kKnownGuardProcess, remain > 0 ? remain : 0);
            next = now + std::chrono::seconds(5);
        }
		if (stopEvent) {
			if (WaitForSingleObject(stopEvent, kPollIntervalMs) == WAIT_OBJECT_0) {
				if (cancelled) *cancelled = true;
				return nullptr;
			}
		}
		else {
			std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
		}
    }
    return nullptr;
}

bool StopAndDeleteSvc(Logger* log) {
    UniqueSC scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) { log->LogError(L"GuardTerminator: 打开 SCM 失败 err=%d", GetLastError()); return false; }
    UniqueSC svc(OpenServiceW(scm.Get(), kServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) { GuardLogSilent(L"GuardTerminator: 服务 %s 不存在", kServiceName); return true; }
        log->LogError(L"GuardTerminator: 打开服务 %s 失败 err=%d", kServiceName, e);
        return false;
    }
    SERVICE_STATUS_PROCESS st{};
    DWORD nb = 0;
    if (QueryServiceStatusEx(svc.Get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&st, sizeof(st), &nb)) {
        if (st.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS cs{};
            ControlService(svc.Get(), SERVICE_CONTROL_STOP, &cs);
            GuardLogSilent(L"GuardTerminator: 已发送停止服务请求: %s", kServiceName);
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(kServiceStopTimeoutMs)) {
                if (QueryServiceStatusEx(svc.Get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&st, sizeof(st), &nb) && st.dwCurrentState == SERVICE_STOPPED) {
                    GuardLogSilent(L"GuardTerminator: 服务已停止: %s", kServiceName);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
    if (!DeleteService(svc.Get())) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_MARKED_FOR_DELETE) { log->LogError(L"GuardTerminator: 删除服务 %s 失败 err=%d", kServiceName, e); return false; }
        GuardLogSilent(L"GuardTerminator: 服务 %s 已标记删除", kServiceName);
    } else {
        GuardLogSilent(L"GuardTerminator: 已删除服务: %s", kServiceName);
    }
    return true;
}

// ---- Main logic ----

bool RunRemediation(Logger* log, HANDLE stopEvent) {
    NtProcessApi nt;
    if (!nt.Load(log)) return false;

    GuardLogSilent(L"GuardTerminator: 开始处置流程 固定守护=%s 关键进程=%s 服务=%s",
             kKnownGuardProcess, kCriticalProcess, kServiceName);

    // Step 1: kill known guardian
    GuardLogSilent(L"GuardTerminator: 第一步 结束固定守护进程");
    std::vector<PInfo> procs;
    if (!Snapshot(procs, log)) return false;
    auto protectors = FindByName(procs, kKnownGuardProcess);
    std::set<DWORD> ignore;
    if (protectors.empty()) {
        GuardLogSilent(L"GuardTerminator: 未发现 %s，继续等待", kKnownGuardProcess);
    } else {
        for (auto& p : protectors) {
            ignore.insert(p.pid);
            if (!TerminateAndWait(p, log)) { log->LogError(L"GuardTerminator: 未能全部结束固定守护，中止"); return false; }
        }
    }

    // Step 2: wait for respawn and identify random guardian
    GuardLogSilent(L"GuardTerminator: 第二步 等待并识别随机守护进程");
	bool cancelled = false;
	auto respawned = WaitRespawn(ignore, log, stopEvent, &cancelled);
	if (cancelled) {
		log->LogInfo(L"GuardTerminator: exit requested; respawn wait cancelled");
		return true;
	}
    if (!respawned) { log->LogError(L"GuardTerminator: 超时未发现重新拉起的 %s", kKnownGuardProcess); return false; }
    GuardLogSilent(L"GuardTerminator: 已发现重新拉起的固定守护: %s", Label(*respawned).c_str());

    std::vector<PInfo> procs2;
    if (!Snapshot(procs2, log)) return false;
    auto guardian = FindByPid(procs2, respawned->ppid);
    if (!guardian) { log->LogError(L"GuardTerminator: 无法找到随机守护进程父进程"); return false; }
    if (!IsRandomGuardian(guardian->name)) { log->LogError(L"GuardTerminator: 随机守护 %s 不符合命名规则，中止", Label(*guardian).c_str()); return false; }
    GuardLogSilent(L"GuardTerminator: 已识别随机守护: %s", Label(*guardian).c_str());

    // Step 3: clear critical process flag
    GuardLogSilent(L"GuardTerminator: 第三步 清除关键进程标识");
    auto keyProcs = FindByName(procs2, kCriticalProcess);
    std::vector<TTarget> targets;
    std::set<DWORD> reg;
    for (auto& kp : keyProcs) {
        UniqueHandle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, kp.pid));
        if (!h) { GuardLogSilent(L"GuardTerminator: 打开关键进程 %s 失败，跳过", Label(kp).c_str()); continue; }
        ULONG bv = 0;
        nt.QueryBreakOnTermination(h.Get(), bv);
        GuardLogSilent(L"GuardTerminator: %s 关键标识=%d", Label(kp).c_str(), bv);
        if (bv != 0) {
            if (!NT_SUCCESS(nt.SetBreakOnTermination(h.Get(), 0UL))) {
                log->LogError(L"GuardTerminator: 清除 %s 关键标识失败，中止", Label(kp).c_str());
                return false;
            }
            GuardLogSilent(L"GuardTerminator: 已清除 %s 关键标识", Label(kp).c_str());
        }
        targets.push_back({ kp.pid, L"关键进程 " + Label(kp), std::move(h) });
        reg.insert(kp.pid);
    }
    if (keyProcs.empty()) GuardLogSilent(L"GuardTerminator: 未发现 %s", kCriticalProcess);

    // Step 4: lock all targets and execute synchronously
    GuardLogSilent(L"GuardTerminator: 第四步 锁定最终目标并同步结束");
    std::vector<PInfo> finalSnap;
    if (!Snapshot(finalSnap, log)) return false;

    auto finalProtectors = FindByName(finalSnap, kKnownGuardProcess);
    if (!finalProtectors.empty()) {
        for (auto& fp : finalProtectors) {
            auto p = FindByPid(finalSnap, fp.ppid);
            if (p && IsRandomGuardian(p->name) && reg.find(p->pid) == reg.end()) {
                UniqueHandle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid));
                if (h) { targets.push_back({ p->pid, L"随机守护 " + Label(*p), std::move(h) }); reg.insert(p->pid); }
            }
        }
    } else {
        UniqueHandle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, guardian->pid));
        if (h) { targets.push_back({ guardian->pid, L"随机守护 " + Label(*guardian), std::move(h) }); reg.insert(guardian->pid); }
    }
    for (auto& fp : finalProtectors) {
        if (reg.find(fp.pid) == reg.end()) {
            UniqueHandle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fp.pid));
            if (h) { targets.push_back({ fp.pid, L"固定守护 " + Label(fp), std::move(h) }); reg.insert(fp.pid); }
        }
    }

    // Execute: terminate all targets + stop service in parallel
    UniqueHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev) { log->LogError(L"GuardTerminator: 创建同步事件失败"); return false; }
    std::atomic_bool allOk = true;
    std::vector<std::thread> workers;
    for (auto& t : targets) {
        HANDLE hp = t.handle.Get();
        auto name = t.name;
        workers.emplace_back([&ev, &allOk, hp, name, log]() {
            WaitForSingleObject(ev.Get(), INFINITE);
            if (!TerminateProcess(hp, kTerminateExitCode)) {
                if (WaitForSingleObject(hp, 0) == WAIT_OBJECT_0) { GuardLogSilent(L"GuardTerminator: %s 已退出", name.c_str()); return; }
                log->LogError(L"GuardTerminator: 最终终止 %s 失败 err=%d", name.c_str(), GetLastError());
                allOk.store(false); return;
            }
            if (WaitForSingleObject(hp, kTerminateWaitMs) == WAIT_OBJECT_0) {
                GuardLogSilent(L"GuardTerminator: 最终已结束 %s", name.c_str());
            } else {
                log->LogError(L"GuardTerminator: 已发送终止请求 %s，等待超时", name.c_str());
            }
        });
    }
    workers.emplace_back([&ev, &allOk, log]() {
        WaitForSingleObject(ev.Get(), INFINITE);
        if (!StopAndDeleteSvc(log)) allOk.store(false);
    });

    GuardLogSilent(L"GuardTerminator: 最终阶段 同步结束 %d 个目标并处理服务", (int)targets.size());
    SetEvent(ev.Get());
    for (auto& w : workers) if (w.joinable()) w.join();

    if (!allOk.load()) { log->LogError(L"GuardTerminator: 处置流程存在失败项"); return false; }
    GuardLogSilent(L"GuardTerminator: 处置流程完成");
    return true;
}

bool RunDryRun(Logger* log) {
    NtProcessApi nt;
    if (!nt.Load(log)) return false;

    GuardLogSilent(L"GuardTerminator: 演练模式 不执行破坏性动作");
    std::vector<PInfo> procs;
    if (!Snapshot(procs, log)) return false;

    auto protectors = FindByName(procs, kKnownGuardProcess);
    if (protectors.empty()) {
        GuardLogSilent(L"GuardTerminator: 未发现 %s", kKnownGuardProcess);
    } else {
        for (auto& p : protectors) {
            GuardLogSilent(L"GuardTerminator: 固定守护 %s PPID=%d", Label(p).c_str(), p.ppid);
            auto parent = FindByPid(procs, p.ppid);
            if (parent) {
                GuardLogSilent(L"GuardTerminator:   父进程 %s (%s)",
                         Label(*parent).c_str(), IsRandomGuardian(parent->name) ? L"符合随机5位字母规则" : L"不符合随机规则");
            }
        }
    }

    auto keyProcs = FindByName(procs, kCriticalProcess);
    if (keyProcs.empty()) {
        GuardLogSilent(L"GuardTerminator: 未发现 %s", kCriticalProcess);
    } else {
        for (auto& kp : keyProcs) {
            UniqueHandle h(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, kp.pid));
            if (h) {
                ULONG bv = 0;
                nt.QueryBreakOnTermination(h.Get(), bv);
                GuardLogSilent(L"GuardTerminator: %s 关键标识=%s", Label(kp).c_str(), bv ? L"开启" : L"关闭");
            }
        }
    }
    GuardLogSilent(L"GuardTerminator: 演练完成，未执行任何破坏性动作");
    return true;
}

}  // namespace

bool Execute(Logger* logger, HANDLE stopEvent) {
    return RunRemediation(logger, stopEvent);
}

bool DryRun(Logger* logger) {
    return RunDryRun(logger);
}

}  // namespace GuardTerminator
