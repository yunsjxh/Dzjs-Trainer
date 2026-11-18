# legacy-x86（2019 年 x86 驱动，存档）

这些文件来自 JiYuTrainer 初版（快乐的梦鱼，2019），是 **Windows XP/Win7 x86** 时代的实现。它们**不参与任何构建**：

- `DzjsTrainerDriver.vcxproj` 的 `ClCompile` 只包含 `Driver64.c`、`DriverGuard64.c`、`Protection64.c`、`RegistryProtection64.c`、`UnloadToken64.c`；
- 本目录同时存在 `DriverEntry` 定义，与 `Driver64.c` 重名，若加入工程会 `LNK2005`；
- `Protect.c` 使用 `_asm` 与 `KeServiceDescriptorTable`，x64 下无法编译。

## 里面是什么

| 文件 | 内容 |
| --- | --- |
| `Driver.c` / `Driver.h` | 入口、设备 `\??\JKRK`（无 SDDL）、进程/线程操作、断电/重启手段 |
| `Protect.c` / `Protect.h` | **SSDT Hook**（改系统服务表）+ 关 CR0.WP；`KxShadowSSDTHook` 是空壳 |
| `Monitor.c` / `Monitor.h` | 进程创建/退出回调的早期实现（含拼写 `KxLmageNotifyRoutine`） |
| `UnExp.c` / `UnExp.h` | 扫描未导出函数（`PspTerminateThreadByPointer` 等） |

## 为什么保留

1. 现役 x64 驱动是"同一职责的第二代实现"，对照阅读能看出哪些问题是刻意规避的（PatchGuard、SDDL、IRQL）。
2. 历史 issue 与讨论里引用的行为（例如"驱动装上去就下不来"）出自这份代码。
3. 文件编码是 **GBK**，与现役 UTF-8 源码不同；阅读时请用支持 GBK 的编辑器，不要"顺手转码"，否则 diff 会失去可比性。

## 不要做的事

- 不要把本目录加回 `.vcxproj`。
- 不要按这份代码的行为推断当前版本（例如 ACPI 断电、`0xCC` 填充、`MmVerifyCallbackFunction` 绕过都**不在**现役二进制里）。
- 不要在这里修 bug：它不参与构建，改了也不会生效。
