// Compatible replacement for the 32-bit injector that used to live outside the
// repository (..\..\DllInjector). DzjsTrainer/TrainerWorker.cpp only needs
// "open the target, inject one DLL, later unload it", so this keeps the same
// two entry points and the same Result shape.
//
// Scope: 32-bit -> 32-bit only. 64-bit targets go through
// DzjsTrainerInjector64.exe, which loads DzjsTrainerHooks64.dll itself.
#pragma once

#include <windows.h>

namespace dll_injector {

struct Result {
    bool success = false;
    unsigned long error = 0;
    // LoadLibraryW / FreeLibrary return value as seen inside the target.
    unsigned long remote_exit_code = 0;

    explicit operator bool() const { return success; }
};

// Injects |dll_path| into |process| through a remote LoadLibraryW call.
Result InjectDll(HANDLE process, const wchar_t* dll_path);

// Unloads the module named |module_name| from |process_id|.
// Fails when the module is not loaded; the caller treats that as "already gone".
Result UnloadDll(HANDLE process, unsigned long process_id, const wchar_t* module_name);

} // namespace dll_injector
