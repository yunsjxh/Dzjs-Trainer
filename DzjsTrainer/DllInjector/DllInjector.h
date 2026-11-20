#pragma once

#include <Windows.h>

namespace dll_injector {

struct Result {
    bool success;
    DWORD error;
    DWORD remote_exit_code;

    explicit operator bool() const noexcept { return success; }
};

// The HANDLE overloads do not take ownership of process.
Result InjectDll(HANDLE process, const wchar_t* dll_path, DWORD timeout_ms = 30000) noexcept;
Result InjectDll(DWORD process_id, const wchar_t* dll_path, DWORD timeout_ms = 30000) noexcept;

Result UnloadDll(
    HANDLE process,
    DWORD process_id,
    const wchar_t* module_name,
    DWORD timeout_ms = 30000) noexcept;
Result UnloadDll(DWORD process_id, const wchar_t* module_name, DWORD timeout_ms = 30000) noexcept;

} // namespace dll_injector
