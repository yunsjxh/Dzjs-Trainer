#include <windows.h>

#include <map>
#include <string>

namespace {

HANDLE g_stop_event = nullptr;
HANDLE g_worker_thread = nullptr;
std::wstring g_log_path;
std::map<HWND, std::wstring> g_last_snapshot;
CRITICAL_SECTION g_log_lock;

std::wstring ReadWindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring ReadClassName(HWND window) {
    wchar_t class_name[256] = {};
    GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    return class_name;
}

void AppendLog(const std::wstring& message) {
    EnterCriticalSection(&g_log_lock);
    HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const std::wstring line = L"[" + std::to_wstring(now.wHour) + L":" +
            std::to_wstring(now.wMinute) + L":" + std::to_wstring(now.wSecond) +
            L"] " + message + L"\r\n";
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written,
                  nullptr);
        CloseHandle(file);
    }
    LeaveCriticalSection(&g_log_lock);
}

BOOL CALLBACK SnapshotChild(HWND child, LPARAM parent_value) {
    const HWND parent = reinterpret_cast<HWND>(parent_value);
    const std::wstring class_name = ReadClassName(child);
    const std::wstring text = ReadWindowText(child);
    if (!class_name.empty() || !text.empty()) {
        AppendLog(L"child parent=0x" + std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) +
                  L" hwnd=0x" + std::to_wstring(reinterpret_cast<ULONG_PTR>(child)) +
                  L" class=" + class_name + L" text=" + text);
    }
    return TRUE;
}

BOOL CALLBACK SnapshotWindow(HWND window, LPARAM process_id_value) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != static_cast<DWORD>(process_id_value)) {
        return TRUE;
    }

    const std::wstring snapshot = ReadClassName(window) + L"|" + ReadWindowText(window) + L"|" +
        (IsWindowVisible(window) ? L"visible" : L"hidden");
    const auto previous = g_last_snapshot.find(window);
    if (previous == g_last_snapshot.end() || previous->second != snapshot) {
        g_last_snapshot[window] = snapshot;
        AppendLog(L"window hwnd=0x" + std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) +
                  L" " + snapshot);
        EnumChildWindows(window, SnapshotChild, reinterpret_cast<LPARAM>(window));
    }
    return TRUE;
}

DWORD WINAPI ProbeWorker(void*) {
    const DWORD process_id = GetCurrentProcessId();
    AppendLog(L"PhoneDialogProbe started, pid=" + std::to_wstring(process_id));
    while (WaitForSingleObject(g_stop_event, 300) == WAIT_TIMEOUT) {
        EnumWindows(SnapshotWindow, static_cast<LPARAM>(process_id));
    }
    AppendLog(L"PhoneDialogProbe stopped");
    return 0;
}

void InitializeLogPath() {
    wchar_t temp_path[MAX_PATH] = {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp_path)), temp_path);
    if (length == 0 || length >= std::size(temp_path)) {
        g_log_path = L"PhoneDialogProbe.log";
        return;
    }
    g_log_path = temp_path;
    g_log_path += L"PhoneDialogProbe-" + std::to_wstring(GetCurrentProcessId()) + L".log";
}

}  // namespace

extern "C" __declspec(dllexport) const wchar_t* WINAPI PhoneDialogProbeLogPath() {
    return g_log_path.c_str();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_lock);
        InitializeLogPath();
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stop_event != nullptr) {
            g_worker_thread = CreateThread(nullptr, 0, ProbeWorker, nullptr, 0, nullptr);
        }
        break;
    case DLL_PROCESS_DETACH:
        if (g_stop_event != nullptr) {
            SetEvent(g_stop_event);
        }
        if (g_worker_thread != nullptr) {
            WaitForSingleObject(g_worker_thread, 1500);
            CloseHandle(g_worker_thread);
        }
        if (g_stop_event != nullptr) {
            CloseHandle(g_stop_event);
        }
        DeleteCriticalSection(&g_log_lock);
        break;
    default:
        break;
    }
    return TRUE;
}
