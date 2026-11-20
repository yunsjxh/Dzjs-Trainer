#include "DllInjector.h"

#include <TlHelp32.h>

#include <cwchar>
#include <string>

namespace dll_injector {
namespace {

constexpr DWORD kProcessAccess =
    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
    PROCESS_VM_READ | PROCESS_VM_WRITE;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

class RemoteAllocation {
public:
    RemoteAllocation(HANDLE process, SIZE_T size) noexcept
        : process_(process), address_(VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {}

    ~RemoteAllocation() {
        if (address_ != nullptr) {
            VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
        }
    }

    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;

    void* get() const noexcept { return address_; }
    void release() noexcept { address_ = nullptr; }

private:
    HANDLE process_;
    void* address_;
};

Result Failure(DWORD error) noexcept {
    return {false, error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error, 0};
}

bool IsUsableProcessHandle(HANDLE process) noexcept {
    return process != nullptr && process != INVALID_HANDLE_VALUE;
}

bool HaveCompatibleArchitectures(HANDLE process, DWORD& error) noexcept {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto is_wow64_process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(kernel32, "IsWow64Process2"));

    if (is_wow64_process2 != nullptr) {
        USHORT current_process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT current_native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT target_process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT target_native_machine = IMAGE_FILE_MACHINE_UNKNOWN;

        if (!is_wow64_process2(
                GetCurrentProcess(), &current_process_machine, &current_native_machine) ||
            !is_wow64_process2(process, &target_process_machine, &target_native_machine)) {
            error = GetLastError();
            return false;
        }

        const USHORT current_machine = current_process_machine == IMAGE_FILE_MACHINE_UNKNOWN
            ? current_native_machine
            : current_process_machine;
        const USHORT target_machine = target_process_machine == IMAGE_FILE_MACHINE_UNKNOWN
            ? target_native_machine
            : target_process_machine;
        if (current_machine != target_machine) {
            error = ERROR_BAD_EXE_FORMAT;
            return false;
        }
    }

    return true;
}

HANDLE CreateModuleSnapshot(DWORD process_id) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) {
            return snapshot;
        }
    }
    return INVALID_HANDLE_VALUE;
}

HMODULE FindRemoteModule(DWORD process_id, const wchar_t* module_name, DWORD& error) noexcept {
    ScopedHandle snapshot(CreateModuleSnapshot(process_id));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        error = GetLastError();
        return nullptr;
    }

    do {
        if (_wcsicmp(entry.szModule, module_name) == 0 ||
            _wcsicmp(entry.szExePath, module_name) == 0) {
            return entry.hModule;
        }
    } while (Module32NextW(snapshot.get(), &entry));

    error = GetLastError();
    if (error == ERROR_NO_MORE_FILES) {
        error = ERROR_MOD_NOT_FOUND;
    }
    return nullptr;
}

Result WaitForRemoteThread(HANDLE thread, DWORD timeout_ms) noexcept {
    const DWORD wait_result = WaitForSingleObject(thread, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        return Failure(WAIT_TIMEOUT);
    }
    if (wait_result != WAIT_OBJECT_0) {
        return Failure(GetLastError());
    }

    DWORD exit_code = 0;
    if (!GetExitCodeThread(thread, &exit_code)) {
        return Failure(GetLastError());
    }
    if (exit_code == 0) {
        return {false, ERROR_DLL_INIT_FAILED, 0};
    }
    return {true, ERROR_SUCCESS, exit_code};
}

} // namespace

Result InjectDll(HANDLE process, const wchar_t* dll_path, DWORD timeout_ms) noexcept {
    if (!IsUsableProcessHandle(process) || dll_path == nullptr || dll_path[0] == L'\0') {
        return Failure(ERROR_INVALID_PARAMETER);
    }

    const DWORD full_path_size = GetFullPathNameW(dll_path, 0, nullptr, nullptr);
    if (full_path_size == 0) {
        return Failure(GetLastError());
    }
    std::wstring full_path(full_path_size, L'\0');
    const DWORD full_path_length =
        GetFullPathNameW(dll_path, full_path_size, &full_path[0], nullptr);
    if (full_path_length == 0 || full_path_length >= full_path_size) {
        return Failure(GetLastError());
    }
    full_path.resize(full_path_length);

    const DWORD attributes = GetFileAttributesW(full_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Failure(GetLastError());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return Failure(ERROR_FILE_NOT_FOUND);
    }

    DWORD architecture_error = ERROR_SUCCESS;
    if (!HaveCompatibleArchitectures(process, architecture_error)) {
        return Failure(architecture_error);
    }

    const SIZE_T path_size = (full_path.size() + 1) * sizeof(wchar_t);
    RemoteAllocation remote_path(process, path_size);
    if (remote_path.get() == nullptr) {
        return Failure(GetLastError());
    }

    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(
            process, remote_path.get(), full_path.c_str(), path_size, &bytes_written) ||
        bytes_written != path_size) {
        return Failure(GetLastError());
    }

    const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (load_library == nullptr) {
        return Failure(GetLastError());
    }

    ScopedHandle thread(CreateRemoteThread(
        process, nullptr, 0, load_library, remote_path.get(), 0, nullptr));
    if (thread.get() == nullptr) {
        return Failure(GetLastError());
    }

    const Result result = WaitForRemoteThread(thread.get(), timeout_ms);
    if (result.error == WAIT_TIMEOUT) {
        // The remote thread may still read the path after our timeout.
        remote_path.release();
    }
    return result;
}

Result InjectDll(DWORD process_id, const wchar_t* dll_path, DWORD timeout_ms) noexcept {
    if (process_id == 0) {
        return Failure(ERROR_INVALID_PARAMETER);
    }

    ScopedHandle process(OpenProcess(kProcessAccess, FALSE, process_id));
    if (process.get() == nullptr) {
        return Failure(GetLastError());
    }
    return InjectDll(process.get(), dll_path, timeout_ms);
}

Result UnloadDll(
    HANDLE process,
    DWORD process_id,
    const wchar_t* module_name,
    DWORD timeout_ms) noexcept {
    if (!IsUsableProcessHandle(process) || process_id == 0 ||
        module_name == nullptr || module_name[0] == L'\0') {
        return Failure(ERROR_INVALID_PARAMETER);
    }

    DWORD architecture_error = ERROR_SUCCESS;
    if (!HaveCompatibleArchitectures(process, architecture_error)) {
        return Failure(architecture_error);
    }

    DWORD find_error = ERROR_SUCCESS;
    const HMODULE remote_module = FindRemoteModule(process_id, module_name, find_error);
    if (remote_module == nullptr) {
        return Failure(find_error);
    }

    const auto free_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"));
    if (free_library == nullptr) {
        return Failure(GetLastError());
    }

    ScopedHandle thread(CreateRemoteThread(
        process, nullptr, 0, free_library, remote_module, 0, nullptr));
    if (thread.get() == nullptr) {
        return Failure(GetLastError());
    }
    return WaitForRemoteThread(thread.get(), timeout_ms);
}

Result UnloadDll(DWORD process_id, const wchar_t* module_name, DWORD timeout_ms) noexcept {
    if (process_id == 0) {
        return Failure(ERROR_INVALID_PARAMETER);
    }

    ScopedHandle process(OpenProcess(kProcessAccess, FALSE, process_id));
    if (process.get() == nullptr) {
        return Failure(GetLastError());
    }
    return UnloadDll(process.get(), process_id, module_name, timeout_ms);
}

} // namespace dll_injector
