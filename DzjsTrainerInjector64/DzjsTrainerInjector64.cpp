// DzjsTrainerInjector64.exe - 64 位注入助手
//
// 主程序 DzjsTrainer.exe 是 32 位，无法对 64 位进程 CreateRemoteThread。
// 这个 x64 控制台程序只做一件事：在 64 位侧完成 LoadLibraryW / VUnloadAll /
// FreeLibrary 三个远程调用，退出码即结果（0 = 成功，其余为 Win32 错误码）。
//
// 用法:
//   DzjsTrainerInjector64.exe --pid 1234 --dll "C:\...\DzjsTrainerHooks64.dll"
//   DzjsTrainerInjector64.exe --pid 1234 --dll "C:\...\DzjsTrainerHooks64.dll" --unload
//
// 卸载顺序不能颠倒：hooks DLL 的 DllMain(DLL_PROCESS_DETACH) 不做清理，
// 直接 FreeLibrary 会把仍被 detour 引用的代码卸载掉，目标进程下次调用被钩
// API 就会跳到无效地址。必须先远程调用导出的 VUnloadAll() 禁用并排空钩子。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <cstdlib>

namespace {

constexpr DWORD kExitBadArguments = 20001;
constexpr DWORD kExitModuleNotFound = 20002;
constexpr DWORD kExitExportNotFound = 20003;
constexpr DWORD kExitTimeout = 20004;

const wchar_t* FileNamePart(const wchar_t* path)
{
	const wchar_t* slash = wcsrchr(path, L'\\');
	const wchar_t* name = slash ? slash + 1 : path;
	const wchar_t* slash2 = wcsrchr(name, L'/');
	return slash2 ? slash2 + 1 : name;
}

HMODULE FindRemoteModule(DWORD pid, const wchar_t* moduleName)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	HMODULE result = nullptr;
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szModule, moduleName) == 0) { result = entry.hModule; break; }
		} while (Module32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return result;
}

// 用 DONT_RESOLVE_DLL_REFERENCES 把 DLL 当数据映射进来：不执行 DllMain、
// 不解析导入，但导出表可用。函数地址 - 模块基址 = RVA，可用于目标进程。
DWORD GetUnloadExportRva(const wchar_t* dllPath)
{
	HMODULE local = LoadLibraryExW(dllPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!local) return 0;
	const FARPROC proc = GetProcAddress(local, "VUnloadAll");
	const DWORD rva = proc ? static_cast<DWORD>(
		reinterpret_cast<const unsigned char*>(proc) - reinterpret_cast<const unsigned char*>(local)) : 0;
	FreeLibrary(local);
	return rva;
}

HANDLE OpenTarget(DWORD pid)
{
	return OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
		PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
}

DWORD DoInject(DWORD pid, const wchar_t* dllPath)
{
	if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) return ERROR_FILE_NOT_FOUND;
	HANDLE process = OpenTarget(pid);
	if (!process) return GetLastError();

	const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
	LPVOID remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remotePath) { const DWORD error = GetLastError(); CloseHandle(process); return error; }
	SIZE_T written = 0;
	if (!WriteProcessMemory(process, remotePath, dllPath, bytes, &written) || written != bytes) {
		const DWORD error = GetLastError();
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		CloseHandle(process);
		return error;
	}
	const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
	if (!loadLibrary) { VirtualFreeEx(process, remotePath, 0, MEM_RELEASE); CloseHandle(process); return GetLastError(); }
	HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
	if (!thread) {
		const DWORD error = GetLastError();
		VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
		CloseHandle(process);
		return error;
	}
	WaitForSingleObject(thread, 30000);
	DWORD moduleResult = 0;
	GetExitCodeThread(thread, &moduleResult);
	CloseHandle(thread);
	VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
	CloseHandle(process);
	return moduleResult != 0 ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
}

DWORD DoUnload(DWORD pid, const wchar_t* dllPath)
{
	const wchar_t* moduleName = FileNamePart(dllPath);
	const HMODULE remoteBase = FindRemoteModule(pid, moduleName);
	if (!remoteBase) return kExitModuleNotFound;
	const DWORD unloadRva = GetUnloadExportRva(dllPath);
	if (!unloadRva) return kExitExportNotFound;

	HANDLE process = OpenTarget(pid);
	if (!process) return GetLastError();
	const auto unloadFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		reinterpret_cast<unsigned char*>(remoteBase) + unloadRva);
	HANDLE thread = CreateRemoteThread(process, nullptr, 0, unloadFn, nullptr, 0, nullptr);
	if (!thread) { const DWORD error = GetLastError(); CloseHandle(process); return error; }
	WaitForSingleObject(thread, 15000);
	CloseHandle(thread);

	const auto freeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"));
	thread = CreateRemoteThread(process, nullptr, 0, freeLibrary, remoteBase, 0, nullptr);
	if (!thread) { const DWORD error = GetLastError(); CloseHandle(process); return error; }
	WaitForSingleObject(thread, 15000);
	CloseHandle(thread);
	CloseHandle(process);
	return ERROR_SUCCESS;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	DWORD pid = 0;
	const wchar_t* dllPath = nullptr;
	bool unload = false;
	for (int i = 1; i < argc; ++i) {
		if (_wcsicmp(argv[i], L"--pid") == 0 && i + 1 < argc) pid = static_cast<DWORD>(_wtol(argv[++i]));
		else if (_wcsicmp(argv[i], L"--dll") == 0 && i + 1 < argc) dllPath = argv[++i];
		else if (_wcsicmp(argv[i], L"--unload") == 0) unload = true;
	}
	if (pid == 0 || !dllPath || !*dllPath) return kExitBadArguments;
	return unload ? DoUnload(pid, dllPath) : DoInject(pid, dllPath);
}
