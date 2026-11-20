# 开发者说明

README 写给使用者：怎么用、装不上怎么办。这份文档写给改代码的人：模块怎么分、哪些写法是踩过坑之后定下来的、构建和验证怎么跑。

文档与 `main` 保持同步。代码和这里对不上时以代码为准，并且顺手把这里改掉。

## 代码地图

| 目录 | 内容 |
| --- | --- |
| `DzjsTrainer/` | 主程序（x86）。`App.cpp` 负责装配，`TrainerWorker.cpp` 是后台工作线程，`DriverLoader.cpp` / `DriverGuard.cpp` 管驱动，`KernelUtils.cpp` 封装 IOCTL，`DllInjector/` 是注入器 |
| `DzjsTrainerUI/` | 界面静态库，链进主程序。`MainWindow.cpp` 是自绘主窗口，`DriverServiceActions.cpp` 管驱动服务启停 |
| `DzjsTrainerDriver/` | x64 WDM 驱动。`IoCtl.h` 是与用户态共享的协议定义 |
| `DzjsTrainerHooks/` | 注入到目标进程的兼容层（Win32 / x64 两个配置）。`HookEngine.*` 是引擎，`HookRegistry.cpp` 是钩子表 |
| `DzjsTrainerUpdater/` | `DzjsTrainerUpdater.vcxproj` 是静态库，`DzjsTrainerUpdaterExe.vcxproj` 是真正去联网的子进程 |
| `GuardTerminator/` | 兜底结束进程的小工具 |
| `JiYuAvKernel/`、`JiYuAvCtl/`、`JiYuAvShared/` | 辅助 AV 驱动及其控制端 |
| `CaptureProbe/` | 抓屏路径验证器，不进解决方案，见下面的「验证工具」 |
| `tools/` | 驱动签名、令牌生成、发布打包脚本 |
| `update-server/` | 更新通道服务端（PHP） |
| `website/` | 站点静态资源 |

依赖是单向的：`DzjsTrainer` 依赖 `DzjsTrainerUI` 和 `DzjsTrainerHooks`，反过来不成立。UI 通过 `TrainerWorkerCallback` 拿状态，不直接碰驱动。

## 三条运行路径

理解这个项目最关键的一点：**驱动、注入层、主程序是三条可以独立失败的路径。**

1. **主程序**：装不上驱动也照常起。`PrepareAndStartProtectionEarly` 失败只写一条 warn，后面继续安装、继续开界面。
2. **驱动**：可选能力。没有它，缺的是内核级杀进程、进程自保护和 AV 扫描，其余功能不受影响。
3. **注入层**：由主程序按目标进程释放并注入，跟驱动是否存在无关。

所以排查问题先问「哪一条路径坏了」，不要一上来就当整体故障。

## 构建

```powershell
MSBuild.exe DzjsTrainer.sln /t:Build /p:Configuration=Release /p:Platform=x86 /m
```

### 几个必须知道的约束

- **解决方案的平台名是 `x86`，工程文件里叫 `Win32`。** 跑解决方案用 `x86`；单独构建某个 `.vcxproj` 时要写 `Platform=Win32`，写成 `x86` 会报 MSB4126。
- **单工程构建必须显式给 `-p:SolutionDir=<仓库根>\`。** 否则 `$(SolutionDir)` 会解析到 vcxproj 所在目录，资源路径和输出路径全错。
- **`-p:BuildProjectReferences=false` 不会重建 `DzjsTrainerUI` 静态库。** 只改 UI 时先单独编 UI，再编主程序；判据是新符号真的进了 `Release/DzjsTrainerUI.lib`（`dumpbin /symbols` 或直接 grep），不是构建返回 0。
- **`/t:Build` 会静默跳过它认为「最新」的工程。** 构建成功与否看产物时间戳，不看退出码。
- **带中文注释的源文件必须是 UTF-8 BOM。** MSVC 在没有 BOM 时按本地代码页读，中文注释会把后面的换行和字符串一起吃掉，报出来的错误位置和真实位置对不上。
- **Boost 只能外部提供。** `DzjsTrainerUI` 的 `TeacherEndpoint/` 用 asio，仓库不含 Boost 本体，通过 `-p:BoostIncludeDir=` 传入；路径不对会在编译前直接报错，不会编到一半才炸。

构建顺序（手工分步时）：驱动 x64 → `DzjsTrainerHooks`（两个位数）→ `DzjsTrainerInjector64` → `DzjsTrainerUpdaterExe` → `DzjsTrainerUI` → `DzjsTrainer`。用解决方案构建就不用管，依赖已经配好。

注意 `DzjsTrainerInjector64` 不在解决方案里，用 `cl.exe` + `build.sh` 单独出。同理 `CaptureProbe`。

## 驱动

### 用户态接口

IOCTL 号是 `0x800`–`0x80D` 连续一段，全部 `METHOD_BUFFERED`，定义在 `DzjsTrainerDriver/IoCtl.h`，主程序侧走 `KernelUtils.cpp`。加新功能就在这段后面续号，不要重排——用户态和驱动可能不是同一版编译出来的。

### 加载与校验

- 发布者按**证书 CN** 判定，白名单在 `DriverPublisherAllowlist.h`；`DriverSignerPin.h` 是构建期生成的名字 pin。**换签名证书必须重新生成 `DriverSignerPin.h`**，否则新证书签的驱动会被自己拦下。
- 驱动内置镜像加载回调，按「文件名 + SHA-256」比对白名单。不匹配就把 `DriverEntry` 的返回值改写成 `0xC0000022`。所以**换驱动而不更新哈希表，等于把驱动变成加载不上的废文件**。
- 驱动要求 Windows 10 2004（19041）起，因为用了 `ExAllocatePool2`。更早的系统没有这个导入，签名再对也加载不了。
- 不要引入 SSDT hook、inline hook 或 DKOM。老版本踩过 PatchGuard，现在的实现靠官方回调 + 对象回调。

### 卸载令牌（两把钥匙，别搞混）

| 用途 | 算法 | 存哪 |
| --- | --- | --- |
| 卸载授权 | ECDSA P-256 | 公钥编进 `.sys`（`UnlockPublicKey.h`），私钥不进仓库 |
| 版本身份校验 | AES-256 | 令牌加密后作为 `IDR_DRIVER_UNLOCK_TOKEN` 资源嵌进主程序 |

卸载令牌是 144 字节，对前 80 字节做 SHA-256，签名格式是 IEEE P1363 的 `r||s`。

**因为公钥编在 `.sys` 里，换密钥就必须重编并重签驱动。** 反过来，驱动重签了但令牌没重生成，主程序就会拒绝卸载——README 里那段「预期失败」说的就是这个。

AES 那半边的拆分明文（partA / partB 异或还原）**硬编码在三处，改一处等于全坏**：`tools/Protect-DriverUnlockToken.ps1`、`tools/Test-DriverUnlockToken.ps1`、`DzjsTrainer/KernelUtils.cpp`。

顺带一个坑：解密时不要用「先问长度再分配」的写法。`BCryptDecrypt(..., NULL, 0, &len, BCRYPT_BLOCK_PADDING)` 返回的是密文长度，不是明文长度，拿它跟 `sizeof(JDRV_UNLOAD_TOKEN)` 比会恒假，结果是解密一次都不执行。按密文长度分配，一次解完。

## 用户态 Hook 层

### 钩子表

`HookRegistry.cpp` 里是一张按 `seq` 升序排列的有序表（条数以 `kFullSetCount` 为准，别信注释里的数字），按域分给四个 guard 模块（Window / Process / Input / Display）。

- **表必须保持 `seq` 升序。** `HooksLimit=N` 是靠全局序号做二分的，顺序动了语义就变了。
- 每项有 `Install` / `InstallIfOriginal` / `SkipWithLog` / `Action` 四种形态。只有 `useLimit=true` 的项消耗一次 `HooksLimit` 计数，`Action` 和 `SkipWithLog` 不消耗。
- 历史保留位（`hk12`–`hk15`、`hk20`/`hk21`、`hk32`）不要复用去装新钩子，序号是排查现场的历史坐标。
- 改完表和旧版对拍总数，别凭感觉。

引擎是内嵌源码的 MinHook（`minhook/`），不要换成预编译 `.lib`，CRT 不匹配会 C1047。对外导出只有三个：`TDAjustCreateInstance`、`VLoad`、`VUnloadAll`。

**卸载有硬约束**：`DllMain(DLL_PROCESS_DETACH)` 里不能清理 detour。必须先远程调用 `VUnloadAll()`，再 `FreeLibrary`。顺序反了就是卸载即崩。

### 替换画面

判据只认「屏幕 DC → 内存 DC 的整块 SRCCOPY」。屏幕 DC 分三档（`ClassifyScreenDc`）：

- `None`：普通 DC，放行。
- `Window`：`WindowFromDC` 拿到桌面窗口。
- `Display`：`WindowFromDC` 返回 NULL，但对象是 `OBJ_DC` 且 `TECHNOLOGY == DT_RASDISPLAY`。

第三档是给 `CreateDCW(L"DISPLAY")` 用的。**只判 `WindowFromDC` 会漏掉这条路径**，看起来像漏画面，其实是判据不全。也不要退回 `GetObjectType(hdcSrc) == OBJ_DC` 这种写法，那会把宿主自己的窗口绘制一起贴图。

16bpp 可以贴（555 `BI_RGB` / 565 `BI_BITFIELDS`，掩码 F800-07E0-001F），8bpp 只能纯色兜底。DXGI 桌面复制、WGC、内核态抓屏目前没覆盖——OBS、会议软件、浏览器共享走的就是这些路径。

## 界面层

主窗口是**整块 GDI+ 自绘**的，没有控件树。侧栏、卡片、列表、按钮、滚动条都是画出来的，命中测试自己算。

- **坐标一律按 96 DPI 的逻辑值写**，绘制时统一 `ScaleTransform(uiScale)`，鼠标事件进来先反向换算回逻辑坐标。加新面板时不要混着写物理像素。
- **原生子窗口是例外**：进程选择器的搜索框（真 `EDIT`）、教师端宿主窗口、驱动状态区里的按钮都是 `CreateWindow`，`MoveWindow` 时必须过 `ToPhysical()`。漏一处就是高 DPI 下控件跑到别的位置。
- **不要为了「好画」把输入框画成图形。** 光标、选区、剪贴板、IME、右键菜单全得自己重写一遍，得不偿失。需要输入就建真控件。
- **找窗口按类名**（`DzjsTrainerNativeWindow`），标题只做 fallback。按标题找会因为改文案而恒返回 NULL，最后表现成「失联」弹窗。
- 自绘控件要吃双击就得带 `CS_DBLCLKS`，否则 `WM_LBUTTONDBLCLK` 根本不来。

## 调试

日志和配置都跟着 exe 走：同名 `.ini`、同名 `.log`，写在 exe 同级目录；目录不可写时回落到 `%LOCALAPPDATA%\DzjsTrainer\DzjsTrainer.log`。

`[JTSettings]` 下有几个开关，只在排查时用：

| 键 | 作用 |
| --- | --- |
| `HooksLimit=N` | 全量集只装前 N 个钩子，用来二分「哪个钩子把极域卡死」 |
| `HooksTrace=1` | 打开逐次钩子调用跟踪，默认只做采样 |
| `CaptureExclusion=0` | 关掉窗口的抓屏排除，本机截图/录像验证时才需要 |

### 验证工具

- `CaptureProbe/`：14 项真实抓屏路径的验证器，输出 REPL / SOLID / PASS / CLEAN。**必须注入探针进程跑才有意义**，在别的进程里跑等于没测。它故意不按钩子的覆盖表挑路径，避免「表里有的都过、表里漏的都漏」。虚拟机或远程桌面里 DXGI 那几项失败是环境没有 Desktop Duplication，不是漏画面。
- `DriverPublisherTests/`：发布者判定与 pin 的用例。
- `JiYuAvPatternTests/`：AV 规则匹配的用例。

## 发布构建

`tools/` 下的脚本负责这一段，`Complete-DriverSigning.ps1` 签名并回嵌，`Build-DzjsTrainerRelease.ps1` 校验产物。手工发版时有两个地方容易翻车：

1. **资源里的令牌路径要切成真实文件。** `.rc` 里默认绑的是 `.sample` 占位令牌，`.vcxproj` 的 `EmbeddedDriverUnlockResource` 只提供增量构建的 Inputs，不决定内容。切完要**同时删掉 `.res` 和 stamp**，只删 stamp 不会重跑资源编译。
2. **构建完把 `.rc` 和 `.vcxproj` 还原成 `.sample`。** 还原后 mtime 比旧 `.res` 早，增量构建会静默复用上一次的真令牌产物——本地看着能用，clean 一次就退回占位符。这种「不可复现」只有换机器或 clean 构建才暴露。

另外 UPX 压缩是**不保真**的：`upx -d` 回来会丢掉调试目录和头部校验和，差几 KB。验收判据是「节表一致 + 内嵌资源逐字节可寻址」，不要用整文件哈希比对。

更新通道有**两份端点实现**：`UpdaterMain.cpp`（子进程真正执行的）和 `UpdaterManifest.cpp`（主程序里的静态库）。探测走的是子进程，只改后者等于白改。

## 不要做的事

这些都在 issue 里以「疑似 bug」的形式出现过，结论是设计如此或者改法错误：

- 别用 `Get-AuthenticodeSignature` 的「根不受信任」去推断「必须开测试模式」。它只看第一个嵌入签名，跟内核实际加载时走的检查不是一回事。
- 别把 `/MT` 换成 `/MD`。发布包是静态 CRT，换掉就要求目标机装运行库——这是被人提过的「修复」，实际是回归。
- 别在 UI 线程做更新探测。子进程 + 超时是刻意设计，探测期间的卡顿全部来自这里。
- 别提交 `Release`、`obj`、`bin`、PDB、日志、证书、私钥、令牌。

## 提交约定

- 一个 PR 只做一件事。混着格式化、重构和功能修复的 PR 没法评审。
- 改行为要有可复现步骤：系统版本、目标软件版本、操作序列、日志片段。
- 提交 AI 生成的代码之前人工过一遍，重点确认三件事：问题是不是**共性**的（不是只在本机这套环境下成立）、改动是不是**必要**的（有没有更小的改法）、上下文有没有被收窄到只看得见自己那一份日志。把本机环境问题当成普适缺陷提交，比不提 PR 更费时间。
- 安全问题不要在公开 Issue 里贴可复现细节。
