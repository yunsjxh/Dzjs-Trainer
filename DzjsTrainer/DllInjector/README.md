# DllInjector

从 `JiYuTrainer/TrainerWorker.cpp` 提取的 Windows DLL 注入与卸载模块。它只依赖 Windows SDK，采用与原项目相同的 `LoadLibraryW` / `CreateRemoteThread` 注入方式。

## API

- `InjectDll(DWORD process_id, const wchar_t* dll_path)`：打开目标进程并加载 DLL；相对路径会先转换为绝对路径。
- `UnloadDll(DWORD process_id, const wchar_t* module_name)`：查找目标模块并调用 `FreeLibrary`。
- 两个操作均提供接收已有进程句柄的重载；调用方仍拥有该句柄。
- `Result.error` 是 Win32 错误码，`Result.remote_exit_code` 是远程线程退出码。

注入器与目标进程必须采用相同架构，例如 x86 对 x86、x64 对 x64。调用方也必须拥有打开目标进程及创建远程线程的权限。

## 构建

```powershell
cmake -S . -B build
cmake --build build --config Release
```

本模块采用同目录 `LICENSE` 中的 MIT 许可。
