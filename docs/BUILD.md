# 构建与复现

## 环境

| 项 | 要求 |
| --- | --- |
| 系统 | Windows 10 19041+ / Windows 11（驱动侧按 x64 编写） |
| 编译 | Visual Studio 2022，含"使用 C++ 的桌面开发"与 MSBuild |
| SDK | Windows 10/11 SDK（`Pathcch.lib`、`Version.lib` 等） |
| WDK | 构建 `DzjsTrainerDriver` 时需要与 SDK 匹配的 WDK |
| 其他 | Python 3.8+（生成票据公钥、跑仓库自检）、PowerShell 5.1+ |

主程序是 **Win32**，驱动是 **x64**。`Release|x86` 对应 `Release|Win32` 工程配置。

## 干净 clone 编不出来的四个原因

这四条是当前事实，不是配置错误：

1. `DzjsTrainer/TrainerWorker.cpp` 引用 `../../DllInjector/DllInjector.h` —— 32 位注入实现**不在仓库内**。
2. `DzjsTrainer.rc` 与 `AvResources.rc` 内嵌 `..\Release\…`、`..\x64\Release\…`、`..\JiYuAvRelease\…` 下的产物 —— 这些目录必须**先构建出来**。
3. `DzjsTrainerUpdater/UpdaterTicketKey.h` 需要 `python tools/make-ticket-key.py` 生成；缺失时 `__has_include` 回退到 `.example`，能编但连不上正式更新服务器（这是刻意的失败方向）。
4. `tools/Protect-DriverUnlockToken.ps1` 被 `.gitignore` 排除且仓库不提供 —— 只有需要重签驱动的维护者才用得到。

## 正确的构建顺序

```powershell
# 0) 生成更新通道公钥（只需要一次；需要 update-server/ticket-sign.pub.der）
python tools\make-ticket-key.py

# 1) 32/64 位 Hook DLL（主程序资源 131 / 136 的来源）
MSBuild.exe DzjsTrainer.sln /t:DzjsTrainerHooks /p:Configuration=Release /p:Platform=Win32 /m
MSBuild.exe DzjsTrainer.sln /t:DzjsTrainerHooks /p:Configuration=Release /p:Platform=x64   /m

# 2) x64 驱动（资源 130 的来源）
MSBuild.exe DzjsTrainerDriver\DzjsTrainerDriver.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m

# 3) JiYuAv 特征扫描驱动（资源 133 的来源）
powershell -ExecutionPolicy Bypass -File tools\Build-JiYuAv.ps1

# 4) 64 位注入助手（资源 137 的来源）
#    工程：DzjsTrainerInjector64/，产物需落到 x64\Release\

# 5) 更新器 exe（资源 134 的来源）
MSBuild.exe DzjsTrainer.sln /t:DzjsTrainerUpdaterExe /p:Configuration=Release /p:Platform=Win32 /m

# 6) 主程序与 UI
MSBuild.exe DzjsTrainer.sln /t:Build /p:Configuration=Release /p:Platform=x86 /m
```

期望产物：

```
Release\DzjsTrainerDriver\DzjsTrainerDriver.sys   → 资源 130
Release\DzjsTrainerHooks.dll                      → 资源 131
JiYuAvRelease\JiYuAvKernel.sys                    → 资源 133
x64\Release\DzjsTrainerHooks.dll                  → 资源 136
x64\Release\DzjsTrainerInjector64.exe             → 资源 137
Release\DzjsTrainerUpdater.exe                    → 资源 134
Release\DzjsTrainer.exe                           ← 最终产物
```

## 只编一部分

不需要驱动的部分可以单独构建，CI 就是这么做的：

```powershell
MSBuild.exe DzjsTrainerHooks\DzjsTrainerHooks.vcxproj /p:Configuration=Release /p:Platform=Win32 /m
```

`DzjsTrainerUI` 依赖 `DzjsTrainer.lib` 与 `DzjsTrainerUpdater.lib`，而主程序工程需要上面所有 BIN 资源，因此单独编 UI 仍要先满足第 1–5 步。

## 常见错误

| 现象 | 原因 |
| --- | --- |
| `RC1015: cannot open include file 'Release\...sys'` | 第 1–5 步没跑，资源源目录不存在 |
| `Cannot open include file: '../../DllInjector/DllInjector.h'` | 缺 32 位注入实现（见上） |
| 驱动加载失败 `0x8007045B` / `577` | 签名不受信任：用测试签名（`bcdedit /set testsigning on`）或正式证书签后再加载 |
| 驱动加载失败、系统开了 HVCI/内存完整性 | HVCI 下未签名驱动无法加载；这不是代码问题 |
| 日志出现 `Embedded unload token is missing, invalid, or cannot be decrypted` | 用占位令牌构建时**预期如此**：驱动拒绝卸载。需要可卸载构建请走 [RELEASE-PIPELINE](RELEASE-PIPELINE.md) |
| 运行时报 `SEC_E_SECPKG_NOT_FOUND` | 进程内 Schannel 安全包缓存异常；代码已在 `wWinMain` 提前预热，仍失败请附日志 |

## 提交前自检

```powershell
python tools\verify_repo.py --strict
```

检查冲突标记、资源/`#include` 路径、误提交产物、解决方案成员、根目录条目。
