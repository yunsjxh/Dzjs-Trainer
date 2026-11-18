# 架构概览

> 面向"要改代码"的人。只看结构、数据流和边界，不复述实现细节。

## 1. 运行时组成

```
DzjsTrainer.exe (Win32, 管理员, 单实例 JYTMutex)
├─ App                启动编排、资源释放、ini/命令行、线程
├─ TrainerWorker      发现极域 → 注入 → 窗口反控 → 命令下发 → 心跳
├─ DriverLoader       SCM 装载、签名门禁、事件流、心跳、看门狗
├─ DriverSignaturePolicy / DriverGuard
├─ AvIntegrated       JiYuAv 特征扫描驱动封装
├─ GuardTerminator    定点处置教师端守护进程/服务
├─ RepositoryCleanup  还原浏览器/USB/CMD 策略
├─ JyUdpAttack        以极域端口吻发 DMOC 报文
└─ DzjsTrainerUI (静态库)  自绘界面 + TeacherEndpoint
        │
        ├─ DeviceIoControl → \\.\JKRK        (DzjsTrainerDriver.sys)
        ├─ DeviceIoControl → \\.\JiYuAv      (JiYuAvKernel.sys)
        ├─ WM_COPYDATA     → 注入的 DzjsTrainerHooks.dll
        └─ WinHTTP + CryptoAPI → 更新服务器
```

## 2. 启动顺序（要点）

1. `wWinMain` 先判断 `--heartbeat-watchdog`（看门狗子进程模式，不建日志/不载驱动/不建互斥体）。
2. 装异常过滤器（MiniDump + `-bugreport`）、预热 SSPI 凭据。
3. `RunInternal`：`MLoadNt` → 提权 → 路径/日志 → **先加载驱动** → 命令行/ini → 版本检查 → 单实例 → 资源释放 → `TrainerWorker` → 守卫/清理线程 → UI。

"先驱动后一切"是刻意的：自保护必须在任何可能被干预的动作之前建立。

## 3. 内核驱动 `DzjsTrainerDriver`

| 项 | 值 |
| --- | --- |
| 设备 | `\Device\JKRK` → `\\.\JKRK`，`IoCreateDeviceSecure` + `SDDL_DEVOBJ_SYS_ALL_ADM_ALL` |
| 协议 | Type `0x8338`，magic `"JDRV"`，version 3，14 个 `METHOD_BUFFERED` IOCTL |
| 回调 | `ObRegisterCallbacks`（进程/线程句柄降权）、进程通知、镜像加载通知、线程创建通知、`CmRegisterCallbackEx`（只锁自身服务键） |
| 保护模型 | 单目标 PID（用户态自报），剥 `TERMINATE/CREATE_THREAD/VM_*/SUSPEND` 等位，保留 `DUP_HANDLE` |
| 卸载 | `DriverUnload` 初始为 `NULL`；`CTL_UNINIT` 通过 ECDSA 令牌后才装上 → 因此 `sc stop` 默认无效 |
| 驱动狙击 | `DriverGuard64.c` 在镜像加载回调里用 MDL 改写未白名单驱动的 `DriverEntry`（返回 `STATUS_ACCESS_DENIED`） |

`legacy-x86/` 是 2019 年 SSDT Hook 版源码，**不参与编译**，只作历史参考。

## 4. 注入与 Hook

- 定位 `StudentMain.exe`：注册表 `DisplayName` → ini → 6 条候选路径 → 进程枚举；要求同目录存在 `LibTDMaster.dll`。
- 注入：32 位走外部 `DllInjector`（仓库外），64 位走 `DzjsTrainerInjector64.exe`。
- DLL 与助手来自内嵌 BIN 资源，释放到 exe 同目录（隐藏属性），内容变化才原子替换。
- 引擎：内嵌 MinHook；`HookRegistry.cpp` 是唯一的注册表（45 个编号 / 38 项实装）。
- 四个域：Window（置顶/全屏/锁屏）、Display（抓屏）、Process（远程运行/关机/结束进程/驱动通信）、Input（键鼠锁定）。
- 命令通道：`WM_COPYDATA`，窗口标题 `JiYu Trainer Virus Window`，格式 `动词:子命令[:参数]`。
- 卸载必须先远程调用 `VUnloadAll()` 再 `FreeLibrary`，否则留下悬空 detour。

## 5. 画面替换的方向

替换发生在**上行采集**：教师端看到替换图/纯色，学生本机仍显示真实桌面。实现分两条线：

1. 极域自己的编码器（`LibJPEG20!EncodeToJPEGBuffer`）：函数体 inline hook + 多模块 IAT patch + `GetProcAddress` 拦截。
2. 通用 GDI（`BitBlt`/`StretchBlt`/`PrintWindow`/`GetDIBits`）：带"屏幕 DC → 内存 DC"的方向门控，避免画花本机 UI。

素材来自 ini/命令指定的图片或视频（GDI+ / Media Foundation 解码），不内嵌资源。

## 6. 网络

- 极域发现：组播 `224.50.50.42:4705`；会话：`225.2.(ch+1).1:5000+0x200·ch`；视频：TCP 4806。
- 报文：`LE32 "DMOC" | 0x10000 | len | 16B GUID | payload`；关键命令码 `0x0F`（远程运行）、`0x10000013/14`（关机/重启）。
- `JyUdpAttack` 只发不收（单播模板）；`TeacherEndpoint` 是完整教师端实现（组播抢身份 + 会话 + H.264 拉流 + 键鼠远控），当前在 UI 里不可达。

## 7. 更新

- 两种形态：静态库（主程序内联）+ 独立 exe（资源 134）。
- 清单受 RSA 票据保护（7 段 ASCII + 正文 SHA-256 绑定 + 时间窗 + 单调性），细节见 [RELEASE-PIPELINE](RELEASE-PIPELINE.md)。
- TLS SPKI pin 仅作诊断，不做拦截。

## 8. 信任模型（七道锁）

| # | 锁 | 实现 | 位置 |
| --- | --- | --- | --- |
| 1 | 驱动 Authenticode + 53 发布者白名单 | `WinVerifyTrust` + 目录签名回退 + 简单名精确匹配 | `DriverSignaturePolicy.cpp` |
| 2 | 签名者 pin | subject + SHA-1 | `DriverSignerPin.h` |
| 3 | 设备 ACL | SDDL SYSTEM+Administrators | `Driver64.c` |
| 4 | 卸载令牌 | ECDSA P-256 / SHA-256 | `UnloadToken64.c` |
| 5 | 驱动 guard | 镜像通知 + MDL 改写 `DriverEntry` | `DriverGuard64.c` |
| 6 | 更新票据 | RSA-2048 PKCS#1v1.5 / SHA-256 | `UpdaterTicket.h` |
| 7 | TLS pin | 仅诊断 | `UpdaterTrust.h` |

## 9. 结构上的已知缺口

这些是实现现状，不是待办清单；修它们属于功能改动。

- 卸载令牌只验签名，不校验机器码/时间/nonce。
- 更新器不验 Authenticode；引导流程在清单 `sha256` 为空时跳过校验。
- `--heartbeat-recovery` 没有处理分支，看门狗拉起的恢复实例会被单实例逻辑吞掉。
- `TeacherEndpoint` 与网络工具页在 UI 层不可达（入口被劫持 / 控件被隐藏）。
- `DriverGuard` 在文件无法哈希时放行；`DISARM` 不需要令牌。
