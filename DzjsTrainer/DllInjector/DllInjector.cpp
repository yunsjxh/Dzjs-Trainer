#include "DllInjector.h"

#include <TlHelp32.h>

#include <cstring>
#include <vector>

namespace dll_injector {
namespace {

// Remote thread timeout. LoadLibraryW on a large DLL can take a moment, but a
// hung target must not block the trainer's worker loop forever.
constexpr DWORD kRemoteThreadTimeoutMs = 10000;

struct RemoteThreadResult {
    bool completed = false;
    DWORD last_error = 0;
    DWORD exit_code = 0;
};

// kernel32.dll is mapped at the same base in every 32-bit process of the same
// boot (ASLR is per-boot for known DLLs), so the address of LoadLibraryW in
// this process is valid inside the target. This is the classic constraint of
// CreateRemoteThread-based injection and the reason 64-bit targets go through
// a helper executable instead.
RemoteThreadResult RunRemoteThread(HANDLE process, LPTHREAD_START_ROUTINE start, void* argument)
{
    RemoteThreadResult result;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, start, argument, 0, nullptr);
    if (!thread) {
        result.last_error = GetLastError();
        return result;
    }
    const DWORD wait = WaitForSingleObject(thread, kRemoteThreadTimeoutMs);
    if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (GetExitCodeThread(thread, &exit_code)) {
            result.completed = true;
            result.exit_code = exit_code;
        }
        else {
            result.last_error = GetLastError();
        }
    }
    else {
        result.last_error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
    }
    CloseHandle(thread);
    return result;
}

} // namespace

Result InjectDll(HANDLE process, const wchar_t* dll_path)
{
    Result result;
    if (!process || !dll_path || !dll_path[0]) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    const SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    const SIZE_T allocation = path_bytes;

    void* remote_buffer = VirtualAllocEx(
        process, nullptr, allocation, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_buffer) {
        result.error = GetLastError();
        return result;
    }

    bool ok = false;
    auto* loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loader) {
        result.error = GetLastError();
    }
    else if (!WriteProcessMemory(process, remote_buffer, dll_path, path_bytes, nullptr)) {
        result.error = GetLastError();
    }
    else {
        const RemoteThreadResult thread = RunRemoteThread(process, loader, remote_buffer);
        result.error = thread.last_error;
        result.remote_exit_code = thread.exit_code;
        // LoadLibraryW returns the module handle; zero means it failed inside
        // the target and the thread exit code carries no Win32 error.
        ok = thread.completed && thread.exit_code != 0;
    }

    VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
    result.success = ok;
    return result;
}

Result UnloadDll(HANDLE process, unsigned long process_id, const wchar_t* module_name)
{
    Result result;
    if (!process || !module_name || !module_name[0]) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HMODULE remote_module = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, module_name) == 0) {
                remote_module = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!remote_module) {
        // Already unloaded (or never injected): not an error worth retrying.
        result.error = ERROR_MOD_NOT_FOUND;
        return result;
    }

    auto* freeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"));
    if (!freeLibrary) {
        result.error = GetLastError();
        return result;
    }

    const RemoteThreadResult thread = RunRemoteThread(
        process, freeLibrary, reinterpret_cast<void*>(remote_module));
    result.error = thread.last_error;
    result.remote_exit_code = thread.exit_code;
    result.success = thread.completed && thread.exit_code != 0;
    return result;
}

} // namespace dll_injector
