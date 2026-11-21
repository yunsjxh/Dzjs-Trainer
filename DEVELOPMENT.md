# 开发者说明

README 写给使用者：怎么跑起来、什么系统能跑、驱动为什么要签名。这份文档写给改代码的人：模块怎么分、哪些写法是踩过坑之后定下来的、构建和验证怎么跑。

文档与 `main` 保持同步。代码和这里对不上时以代码为准，并且顺手把这里改掉。

## 代码地图

| 目录 | 内容 |
| --- | --- |
| `DzjsTrainer/` | 主程序（x86）。`App.cpp` 负责装配，`TrainerWorker.cpp` 是后台工作线程，`DriverLoader.cpp` / `DriverGuard.cpp` 管驱动，`KernelUtils.cpp` 封装 IOCTL，`DllInjector/` 是注入器，`EduProducts.h` 是其他电子教室的目标表 |
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
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  DzjsTrainer.sln /t:Build /p:Configuration=Release /p:Platform=x86 /m
```

### 环境

换台机器要装这些：

- **Visual Studio**，本机是 18 Community（MSVC 14.51）。装的时候勾「使用 C++ 的桌面开发」，组件里要有 **v143 生成工具**——新版默认给的是 v145，而工程全部固定 `v143`，缺了就报找不到工具集。
- **Windows SDK**，本机 10.0.28000。
- **WDK**，勾「Windows 驱动程序工具包」。装完才有 `WindowsKernelModeDriver10.0` 这个平台工具集，驱动工程靠它。

**别用 Build Tools 顶替 Visual Studio。** 驱动工具集只在完整 VS 实例里注册，Build Tools 加 WDK 的裸组合编驱动会直接报工具集缺失，而且报错不会告诉你是这个原因。

SDK 版本对不上时：用户态工程写的是 `10.0`，自动取本机最新，不用管；`DzjsTrainerDriver.vcxproj` 里固定写死 `10.0.28000.0`，装不到这个版本就改这一行，换成实际装上的版本号。

确认装在哪：

```powershell
# VS
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
# Kits
(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
```

本机两者都不在默认的 C 盘：VS 在 `D:\Program Files\Microsoft Visual Studio\18\Community`，Kits 在 `D:\Windows Kits\10`。普通 PowerShell 里 `MSBuild.exe` 不在 PATH，要么写完整路径，要么用 VS 附带的「开发者 PowerShell」。

### 几个必须知道的约束

- **解决方案的平台名是 `x86`，工程文件里叫 `Win32`。** 跑解决方案用 `x86`；单独构建某个 `.vcxproj` 时要写 `Platform=Win32`，写成 `x86` 会报 MSB4126。
- **单工程构建必须显式给 `-p:SolutionDir=<仓库根>\`。** 否则 `$(SolutionDir)` 会解析到 vcxproj 所在目录，资源路径和输出路径全错。
- **`-p:BuildProjectReferences=false` 不会重建 `DzjsTrainerUI` 静态库。** 只改 UI 时先单独编 UI，再编主程序；判据是新符号真的进了 `Release/DzjsTrainerUI.lib`（`dumpbin /symbols` 或直接 grep），不是构建返回 0。
- **发版时重建主程序会把压好的 `DzjsTrainerUpdater.exe` 覆盖回未压缩版。** 主工程对它有 ProjectReference，`-t:Rebuild` 会连带重建依赖，`upx -9` 白做，内嵌进主程序的是 275 KB 的胖版。压完只能 `-t:Build -p:BuildProjectReferences=false` 重链；判据是内嵌资源的 size（压缩后约 130 KB），不是"构建成功"。
- **内存紧张时 `MainWindow.cpp` 会报 `C1060: 编译器的堆空间不足`。** 32 位 cl.exe 的堆先到顶，跟代码无关——本机开着两个 VMware 虚拟机、空闲内存只剩 1 GB 时必现。临时给 `Release|Win32` 的 `AdditionalOptions` 加 `/Zm500` 编一次，编完还原；根治是少开一个虚拟机。`PreferredToolArchitecture=x64` 不行，它会把工具集换成 14.42。
- **`/t:Build` 会静默跳过它认为「最新」的工程。** 构建成功与否看产物时间戳，不看退出码。
- **带中文注释的源文件必须是 UTF-8 BOM。** MSVC 在没有 BOM 时按本地代码页读，中文注释会把后面的换行和字符串一起吃掉，报出来的错误位置和真实位置对不上。
- **Boost 只能外部提供。** `DzjsTrainerUI` 的 `TeacherEndpoint/` 用 asio，仓库不含 Boost 本体，通过 `-p:BoostIncludeDir=` 传入；路径不对会在编译前直接报错，不会编到一半才炸。
- **`MSB6001 命令行的开关无效` 通常不是参数写错。** 这个报错往外带的是 `System.ArgumentException: 已添加项。字典中的关键字:"Path" 所添加的关键字:"PATH"` —— 说明当前进程的环境块里同时存在 `Path` / `PATH` / `path` 几份同名变量，MSBuild 给 cl.exe 拼环境时字典撞了。换个干净的 shell 跑就没事（本机 PowerShell 会话里是脏的，走 Git Bash 反而正常）。查法：`[Environment]::GetEnvironmentVariables().Keys | ? { $_ -match '(?i)^path$' }`，返回好几个就是它。

构建顺序（手工分步时）：驱动 x64 → `DzjsTrainerHooks`（两个位数）→ `DzjsTrainerInjector64` → `DzjsTrainerUpdaterExe` → `DzjsTrainerUI` → `DzjsTrainer`。用解决方案构建就不用管，依赖已经配好。

驱动单独构建时平台写 `x64`，这是唯一不用 `x86` / `Win32` 的工程：

```powershell
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  DzjsTrainerDriver\DzjsTrainerDriver.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

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

**因为公钥编在 `.sys` 里，换密钥就必须重编并重签驱动。** 反过来，驱动重签了但令牌没重生成，主程序就会拒绝卸载——「发布构建」开头那段日志说的就是这个。

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

### 全屏与置顶的现场询问

被注入的窗口要全屏或置顶时，以前是直接吞掉（极域名单里那些），现在先问用户一句。**覆盖的不只是极域**：极域走 `jiYuWnds` 名单，其他适配的电子教室（红蜘蛛、伽卡他卡、联想传奇）和「进程注入」指定的任意宿主走通用判据 —— `VWindowOpAskable` 判「本进程的顶层窗口 + 这次操作要把它铺满屏幕」。

判据分两条：

- 全屏：`SetWindowPos` / `MoveWindow` / `DeferWindowPos` 的目标矩形铺满所在显示器；第三方宿主放宽成「贴住屏幕左上角 + 覆盖 90%」，它们的尺寸未必等于主屏（工作区、副屏、DPI 取整都见过）。`ShowWindow` 有两条路都算：`SW_MAXIMIZE`（Delphi 的 `WindowState := wsMaximized` 走这条，判据不看当前尺寸 —— 它先把状态设成最大化、尺寸随后才撑开），以及窗口本来就铺满 >= 90% 时的 `SW_SHOW` / `SW_SHOWNORMAL`（先按整屏建成、再 ShowWindow 的写法）。
- 置顶：插到 `HWND_TOPMOST`，或 `GWL_EXSTYLE` 新增 `WS_EX_TOPMOST`。第三方宿主只在窗口覆盖屏幕 50% 以上才问 —— 宿主的置顶工具条常年在那儿，每条都弹框会把用户烦死。
- **后台巡查兜底。** 钩子只看得见走了导出 API 的路径。`CreateWindowEx` 直接按整屏建窗口、`SetWindowPlacement` 恢复最大化、`SW_VISIBLE` 一次成型，这三种一次窗口钩子都不过，钩子全程沉默 —— 2026-11-21 首次上线就是这样，五个宿主全注进去了，一个框都没弹。现在消息中心线程每 300ms 巡一遍本进程的顶层窗口，见到「已经铺满 >= 90% 且没问过」的补问一次，答「否」就缩回 3/4 屏幕（`VEduSetFull(hWnd, false)`，最大化过的先 `SW_RESTORE` 否则尺寸不生效）。代价是被拦下的窗口会先闪一下全屏，所以钩子那条路仍然是主力，巡查只兜底。
- 注入完成时就已经全屏的窗口**记成允许**（`VEduEnumWindowsProc` 里做），那是「本来就在上课」，不该弹框，更不该把画面缩小。副作用是：广播窗口若在注入后 100ms 内就建好，也会被这条规则顺走 —— 可接受。

- **不能在钩子里同步弹框。** 这些调用发生在宿主自己的 UI 线程上，模态框会把「全屏」卡到用户回答为止，中间的绘制和计时器全停。所以第一次只拦不问，`PostMessage` 到消息中心线程，答「是」以后再用原参数重放。重放走 `ra*` / `fa*`，不经过 detour，不会二次询问。
- 决定按窗口记（未定 / 拒绝 / 允许），窗口销毁时清。表满 12 条淘汰最早的一条。「取消」只拒绝这一次，下次还问。
- 同一时刻只问一个问题。正在问的时候又来请求，先挡下并记成 `lastBlocked`；右键菜单「允许全屏和置顶」会把它补上。
- **放行之后必须同步 `gbFullManual` 和 `allowGbTop`。** 主程序每两秒扫一次广播窗口：`gbFullManual` 为假就给窗口加回边框、按 3/4 屏幕摆好，`allowGbTop` 为假就摘掉置顶——学生刚允许的全屏会被它自己拆掉。第三方宿主没有这个扫描，同步过去也无害（`VSendMessageBack` 在 `g_customHost` 下丢弃）。
- 询问不受 `BlockWindowOps` 约束（那个开关管的是静默拦下窗口操作），单独用 `AskWindowOps` 控制，缺键按开处理。
- 右键菜单两套。极域的 `JiYuWndProc` 那套菜单项把意图发回主程序；第三方宿主由 `VEduSubclassProc` 挂一个独立菜单（全屏/退出、置顶/取消、允许、关闭），**改动一律走 `ra*` / `fa*`**，否则点「全屏」会自己弹出一个「是否允许全屏」。全屏前的矩形存在 `WindowOpState.restore` 里，退出时还原；没记录过就按 3/4 屏幕摆回去。
- **`g_opAsking` 在框弹出期间必须保持为真。** 它原来在 `VAskWindowOp` 开头就清了，巡查每 300ms 一趟、窗口还在全屏、决定又没落库，正好会一路叠出第二个、第三个模态框。清在记录答复的同一段临界区里，并顺手把 `sweepSkip` 处理掉（「取消」= 只拒绝这一次，巡查没有「下一次」可拦，只能记「等它退出全屏再问」）。
- **诊断走 `hkb:eduop`，不走 `VSendMessageBack`。** 后者在 `g_customHost` 下整体丢弃（怕误触发主程序的失联判定）。注入进程自己的 `DzjsTrainerHooks-debug.log` 落在宿主的安装目录里，用户平时看不到，所以「询问已就绪（active）/ 答复（allow|deny|once）」由 `VEduOpNotice` 单独发一份给主程序，进主日志。排查「该弹框却没弹」时先看主日志有没有 `Window op prompt ready in edu host` —— 没有就说明跑的不是带询问的那版 DLL。

- 注入常常晚于宿主全屏：`WM_HOOK_INITIALIZED` 里 `EnumWindows` 把已经铺满的窗口补挂一遍，否则第一次右键什么都没有。

### 替换画面

判据只认「屏幕 DC → 内存 DC 的整块 SRCCOPY」。屏幕 DC 分三档（`ClassifyScreenDc`）：

- `None`：普通 DC，放行。
- `Window`：`WindowFromDC` 拿到桌面窗口。
- `Display`：`WindowFromDC` 返回 NULL，但对象是 `OBJ_DC` 且 `TECHNOLOGY == DT_RASDISPLAY`。

第三档是给 `CreateDCW(L"DISPLAY")` 用的。**只判 `WindowFromDC` 会漏掉这条路径**，看起来像漏画面，其实是判据不全。也不要退回 `GetObjectType(hdcSrc) == OBJ_DC` 这种写法，那会把宿主自己的窗口绘制一起贴图。

16bpp 可以贴（555 `BI_RGB` / 565 `BI_BITFIELDS`，掩码 F800-07E0-001F），8bpp 只能纯色兜底。DXGI 桌面复制、WGC、内核态抓屏目前没覆盖——OBS、会议软件、浏览器共享走的就是这些路径。

## 其他电子教室

伽卡他卡、红蜘蛛、联想传奇走的是**另一条路**，不碰极域那套单进程语义。

极域这条线写死了：`StudentMain.exe` 一个 PID，注入状态、握手、`hkb:` 协议全绑在一起，主程序按 2 秒一次的定时器盯着它。这些产品没有这套东西，所以复用「进程注入」的通用宿主路径（`g_customHost`），装同一张 34 项钩子表。

目标表在 `DzjsTrainer/EduProducts.h`，主程序和 Hooks 共用一份。每条规则是「进程名 + 路径特征」，两个都命中才作数。路径特征不是保险，是必要条件：`Student.exe` 这种名字满地都是，只按名字匹配等于给无关进程装钩子。

**伽卡他卡是这条规则的例外，也是它起初整个没被认出来的原因。** 它在国内基本装在中文目录下（`D:\咖卡他卡安装\Student\`），`gakataka` 一个字母都匹配不上；主程序又叫 `Student.exe`，跟联想传奇撞名，不能放宽成只认名字。折中办法是「伴生文件」：`EduProcessRule` 多一个 `companion` 字段，路径特征留空时改看同目录（或上一级）里有没有另一个进程 —— `Student.exe` 和 `Smonitor.exe` 装在一起，认出其中一个就等于认出这一款。两个字段都为空表示只认名字，留给 `Smonitor.exe` 这种本身够独特的名字。

守护那条规则现在自带 `isHost`：它是不是学生端本身，写在表里，不再靠「这个名字在不在 rules 里」推。伽卡他卡把 `Student.exe` 登记进来之后，`Smonitor.exe` 会被那个旧判据算成宿主，结束就变成杀守护进程 —— 正好是红蜘蛛那段要避免的事。

**命中产品表的宿主（`g_eduHost`）显示域与极域同口径，泛化的注入宿主不是。** 这条区别是整套适配的核心：

- 泛化宿主强制 `allowMonitor = true`，装钩子但不拦；
- 电子教室宿主按 ini 的 `AllowMonitor` / `EnableVideoModify` 走，`whitePreviewOnly` 生效，假桌面窗口会在屏幕外（`-screenWidth, -screenHeight`）show 出来，给 `hk26/27/33` 提供取 DC 的真窗口；
- 也只在这条路径上，`hk41`（BitBlt）在「禁止教师看屏」时跟着装。极域不装是因为它的抓屏走 LibDeskMonitor / DispFilter，而这三款的抓屏就是裸 BitBlt，不装等于没拦。拦截范围仍由 `IsScreenCaptureInto` 限死在「屏幕 → 内存 DC」，宿主自己的双缓冲和 UI 绘制不受影响。
- 白屏同样只对电子教室宿主生效。极域 `blackFill` 分支画的是黑或替换图，改颜色属于行为变更，没顺手统一。

**注入的实例用独立窗口标题**（`EDZU_CUSTOM_HOST_WINDOW_TITLE`）。主程序靠 `FindWindow("JiYu Trainer Virus Window")` 找极域消息中心，注入进程要是同名，`hk:` 命令会投给错误的进程。标题分开之后，要给它们同步设置得走 `MsgCenterSendToCustomHosts()` —— `InitSettings()` 每次保存设置都会发一条 `hk:reset`，对端重读 ini 并按新开关补装钩子。

自动注入在 `TrainerWorker::RunEduCompatInject()`，扫描 10 秒一次（`RunResetPid` 是 2 秒一次的定时器，直接挂上去太密），失败的目标压 10 分钟再试。

**开关在「进程注入」页顶部**（「自动接管其他电子教室」）。点一下会写 ini 的 `[JTSettings] EduCompatAutoInject`，同时调用 `SetEduCompatAutoInject` 让工作线程立刻改状态、把下一次扫描提前到本轮；只改 ini 的话要重启程序才生效。关掉只停止注入新进程，已经注进去的宿主保持原样。

界面上有三处跟着这个状态走，改后端时别漏：

- 宿主数量由 `NotifyEduHosts()` 推给界面（`TrainerWorkerCallback::OnUpdateEduHosts`），只在注入成功、卸载成功、清理死进程三处触发。界面不轮询 `_CustomInjected` —— 那是工作线程的数据。
- 概览页的徽标和「受控宿主」卡片。极域没跑但其他教室已经接管时显示「已接管 / 其他教室 N 个」，卡片下面一行列产品名；原来固定写「待检测 / 进程未运行」，装了别的教室也像没生效。
- 概览页「防护状态」那一格点开是块只读信息面板（`PaintProtectDialog`）：总体状态、受控宿主、已接管教室、主驱动、自我保护、拦截项数、版本、构建时间，一行一项。数据在打开那一刻采一次，不跟着后面刷新 —— 用户是来看「现在什么情况」的，数字在眼前跳反而看不明白。采集段在 `stateMutex` 里，采完立刻放锁；之后才碰窗口，因为 `SetForegroundWindow` / `SetFocus` 会同步回调窗口过程，那些路径也走这把锁。

**「构建」那行精确到分钟**（`2026-11-21 16:14`），来自本进程 PE 头里的 `TimeDateStamp`，也就是链接时刻，增量构建也准。打包器把时间戳清零时退回 `BuildStamp.cpp` 的 `__DATE__` + `__TIME__` —— 那一档停在它自己被重编的时刻，会旧一些，但不至于空着。
- 注入列表里，命中产品表的目标带 `· 红蜘蛛` 这样的后缀，手动注入的普通进程不带。

`SyncCustomHosts()` 是给「界面改了显示开关」用的。`ApplyTemporaryVideoProtection()` 改完媒体和 `EnableVideoModify` 之后要广播一次 `hk:reset`，否则宿主还按旧的 ini 值拦或放 —— 极域那几条 `hk:` 命令它们不认。

**几套产品同时装、同时跑是常态，所以一轮把所有命中的目标都注进去**，不是每轮注一个。每轮注一个的话，第二套要等 10 秒才受控，这中间它对教师端完全透明。单轮上限 6 个：64 位目标要起注入助手并等它返回，一轮全注会占住界面线程。

每个宿主进程里是**各自一份** hooks 实例，互不干扰；设置靠 `MsgCenterSendToCustomHosts()` 广播到所有带 `EDZU_CUSTOM_HOST_WINDOW_TITLE` 的窗口，改一次全都跟着变。所以「同时控制」不需要额外的调度，需要的是别把宿主当成只有一个：扫描、注入记录、卸载循环都得按集合写。

**「结束」要连上游一起停。** 红蜘蛛的学生端不是自己起来的，父链是 `REDAgent.exe ← checkrs.exe ← services.exe`：`checkrs` 是服务，学生端被杀掉之后它会把学生端再拉起来，用户看到的就是「点了没反应」。所以 `KillEduHosts()` 先调 `StopEduGuards()`，把 `EduProducts.h` 里 `guards` 声明的服务 `ControlService(STOP)` 并等到 `SERVICE_STOPPED`，再走 `SweepEduCompanions()`：把学生端所在目录（含子目录）里的进程一起结束，再顺着父链往上，直到碰见系统进程（`%SystemRoot%` 下的、或取不到路径的）为止。只杀注入记录里那一个进程没用——旁边还有托盘、检查器、升级器，上游还有服务盯着。

清扫有三道保险：目录是盘根或 `Program Files` / `Windows` / `ProgramData` 这类公共目录本身时不做整目录清理，只结束目标；一次最多 32 个进程；服务宿主先按 `pid → 服务名` 反查并用 SCM 正常停，避免恢复策略又把它拉起来。`pid → 服务名` 这张表每次清扫只枚举一遍——这条路径跑在界面线程上，逐个枚举会卡。

**服务停不掉就挂起，别杀。** 红蜘蛛的 `checkapp2` 服务停在「标记删除」状态，`ControlService(STOP)` 直接返回 1052（服务不存在），代码退回去杀 `checkrs.exe`——这正好踩中服务恢复：进程被杀算异常退出，SCM 按恢复策略把它重新启动，它再把 `REDAgent.exe` 拉起来，用户看到的就是「杀了还会重启」。现在纯守护（`guards` 里有、`rules` 里没有的 exe）一律 `NtSuspendProcess`，挂起不触发恢复动作；`PsGhost` 这种既是守护又是学生端的还是杀，因为用户要的是它消失。

**杀完还要盯一会儿。** 不管是谁在拉，`KillEduHosts()` 结束后都会把名单记进 `_EduKillWatch*`，`RunResetPid()` 每 2 秒看一眼（`StepEduKillWatch()`）：重新出现的宿主再收一次，纯守护挂起，并把父进程 PID 和路径写进日志。安静 30 秒算完，最长 8 分钟；盯梢期间这款产品不再自动注入，否则会一边杀一边接管。日志里出现「又被拉起，父进程 …」就说明还有一层我们没清掉，那行路径就是线索。

**「启动」是「结束」的逆操作，顺序不能反。** `StartEduHost()` 先撤盯梢（不撤的话起来一个被收一个），再 `StartServiceW` 起服务（服务拉起学生端时用的参数和版本都对），然后 `NtResumeProcess` 恢复被挂起的守护进程，最后才用注入时记下的 `_EduLaunchPaths` 直接拉学生端——联想传奇没有服务、`PsGhost` 又被结束掉了，只剩这一条路。

**别把「学生端还没出现」当成启动失败。** 红蜘蛛从恢复 `checkrs.exe` 到拉起 `REDAgent.exe` 要十秒上下。原来在这里等 800 毫秒，等不到就返回失败，界面弹「无法启动红蜘蛛」，而它几秒后自己就出来了——日志里的「学生端没有起来，也不知道它在哪」就是这么来的。现在只要服务起来了、或者守护进程恢复过，就算启动指令已下达；接管交给后面的扫描，不在一次点击里干等。`_EduStartWatchUntil` 把扫描间隔从 10 秒收紧到 2 秒、管 30 秒，学生端一出现就注进去。

**「在跑」和「已接管」是两件事。** 菜单原来只按 `_CustomInjected` 判断，可学生端从被守护拉起到下一轮扫描之间（最多十秒）不在记录里：用户看得见进程，菜单却只有「启动」。`IsEduProductRunning()` 改成按产品表找进程（名字 + 路径特征），`KillEduHosts()` 也不再因为记录为空直接返回——先用产品表扫一遍在跑的宿主，再补上记录里的，两边取并集。否则「没有结束项」和「点了没反应」会一起出现。

概览页同理。`UpdateState()` 多一档「检测到红蜘蛛在运行」，用在没接管但进程在跑的时候（自动接管关掉，或者还没轮到扫描）。`NotifyEduHosts()` 这时把产品名放进 summary，界面靠 `count == 0 && summary 非空` 认出「待接管」，卡片不会一边说在运行一边说未运行。

`StopEduGuards()` 里 `NtSuspendProcess` 是计数式的：同一个进程挂两次，之后一次 `NtResumeProcess` 救不回来。「结束」一次会走两趟（停服务 + 清扫上游），所以用 `_EduSuspendedGuards` 记下挂起过的 PID，一个进程只挂一次；恢复成功后从表里删掉。

三个容易写错的点：

- **死 PID 必须每轮清掉。** PID 会被复用，`_CustomInjected` 里留着的死记录会让新进程被当成「已注入」，永远拿不到钩子。清的时候顺带比进程名，名字对不上说明 PID 已经被别的进程占了。
- **手动卸载要能压住自动注入。** 用户在「进程注入」里卸掉某个宿主后，`_EduManualSkip` 记一笔，同一个进程不再注回去；进程退出后自动解禁，下次启动照常接管。
- **先比名字再查路径。** 全量扫描要过一百多个进程，逐个 `OpenProcess` 查路径比 `EduCompatNameCandidate()` 贵得多，非候选在名字这一步就筛掉。

**验证状态**：匹配逻辑有离线和多组用例（真实路径、大小写、同名反例、极域 `StudentMain.exe` 不该被认领），`cl` 编过。「在跑但没接管」这条路用伪造目录（`.../red spider/REDAgent.exe`）在本机走过一遍：菜单里有结束项、没注入也能结束、标题和卡片都报「待接管」。伽卡他卡另造了一份中文目录（`D:\咖卡他卡安装\Student\` 下放 `Student.exe` 和 `Smonitor.exe`）：靠伴生文件认出来，标题和宿主卡都报「伽卡他卡 ×2」，菜单点它能结束。MD3 菜单在 100% 缩放下截图核对过落点、悬停状态层和红色强调项，**125% / 150% 还没看**。**真机上的注入拦截仍未验证** —— 三款软件各自走哪些抓屏路径、锁屏用什么手段，都还不知道。真机跑起来先看日志里的 `product=` 和「已自动注入」，再看教师端画面。

## 界面层

主窗口是**整块 GDI+ 自绘**的，没有控件树。侧栏、卡片、列表、按钮、滚动条都是画出来的，命中测试自己算。

- **坐标一律按 96 DPI 的逻辑值写**，绘制时统一 `ScaleTransform(uiScale)`，鼠标事件进来先反向换算回逻辑坐标。加新面板时不要混着写物理像素。
- **原生子窗口是例外**：进程选择器的搜索框（真 `EDIT`）、教师端宿主窗口、驱动状态区里的按钮都是 `CreateWindow`，`MoveWindow` 时必须过 `ToPhysical()`。漏一处就是高 DPI 下控件跑到别的位置。
- **不要为了「好画」把输入框画成图形。** 光标、选区、剪贴板、IME、右键菜单全得自己重写一遍，得不偿失。需要输入就建真控件。
- **找窗口按类名**（`DzjsTrainerNativeWindow`），标题只做 fallback。按标题找会因为改文案而恒返回 NULL，最后表现成「失联」弹窗。
- 自绘控件要吃双击就得带 `CS_DBLCLKS`，否则 `WM_LBUTTONDBLCLK` 根本不来。

**弹出菜单（`Md3RunMenu`）也是自绘的**，「电子教室操作」和电源控制都走它。`TrackPopupMenu` 出来的是方角 + 系统字体 + 灰底，跟整套圆角卡片摆一起像从别的程序借的。现在的做法是一个 `WS_EX_LAYERED` 的无边框窗口，自己画 16 圆角、48 行高（这里取 42）、悬停与按下各压一层主色状态层，键盘上下键 / 回车 / Esc 都能用。四个坑：

- **画在 `Bitmap` 上再拷进 DIB，不要 `Graphics(HDC)`。** 往 DIB 的 DC 上画，GDI+ 不保证写 alpha，分层窗口拿到的是一块不透明方块。拷的时候顺手把颜色乘上 alpha 变成预乘，否则圆角边缘挂一圈发白。
- **位置用 `SetWindowPos` 定，`UpdateLayeredWindow` 的 `pptDst` 传 `nullptr`。** 窗口显示出来、`SetFocus` 激活那一刻，系统会拿一个我们没给过的坐标再 `SetWindowPos` 一次（`SWP_NOSIZE|SWP_NOZORDER`），菜单会跑到触发卡片的左上方约 129×235 的位置。所以激活之后再显式摆一次。
- **命令要 `PostMessage` 回主窗口**，不要在自己的消息循环里直接调处理函数 —— 那是在菜单窗口销毁的半路上重入界面。
- **命中判定要连 x 一起算**：菜单自己 `SetCapture`，光标从左右两侧移出去之后鼠标消息仍然往这个窗口送，`WM_MOUSELEAVE` 不会来。只按 y 算的话，那一行会一直停在悬停色上；同样的原因，在外面点一下也会被判成点中那一行，直接执行了。

**退出确认是独立桌面上的全屏窗口**（`ExitConfirmProc`）：暗化壁纸 + 一块居中面板，`SwitchDesktop` 切过去，之后切回来才算退出成功。原来是 6 位数字的真人校验，现在整块面板点一下就退，`Esc` 取消 —— 但「只认真实输入」这条没丢，判据从输入验证码换成了 RawInput 的设备句柄：

- 键盘和鼠标都按 `RIDEV_INPUTSINK` 注册到弹窗窗口。`SendInput` 造出来的 `WM_INPUT` 事件 `header.hDevice` 是空句柄，`PostMessage` 投的鼠标消息压根不产生 `WM_INPUT`，两条注入路径都进不来。
- 按钮的 `BN_CLICKED` 分不出来源，所以真正的门设在 `WM_COMMAND`：没有 `hardwareConfirm` 就不退。`WM_LBUTTONDOWN` 也只在 RawInput 没注册上的机器才当数。
- 面板命中按逻辑坐标算，物理坐标要先除以 `scale`，不然 125%/150% 下点不中。
- 「取消」是真子按钮，点在它身上消息不会到父窗口，所以「点面板哪都算确认」不必额外排除它 —— 但鼠标的 RawInput 路径要自己 `PtInRect` 排除，否则点取消也会被当成确认。

## 关机拦截

主窗口处理 `WM_QUERYENDSESSION`：不是自己发起的关机 / 重启 / 注销，一律 `ShutdownBlockReasonCreate` 再返回 `FALSE`。原因文本在 `WM_ENDSESSION`（`wParam` 为 `FALSE`，表示关机被取消）里撤掉，否则下一次关机会挂着一句已经过期的话。

- 「自己发起的」靠 `allowShutdown` 一位，只有电源菜单里的关机 / 重启会立起来。极域远程关机、关机脚本都没有这位，全部拦下。
- 拦下之后弹提示，走 `PostMessage(WM_MY_SHUTDOWN_BLOCKED)` 回到正常消息流 —— `WM_QUERYENDSESSION` 只有几毫秒预算，不能在里面显示窗口。这个 ID 用 `WM_APP+47`，`+46` 被 `UpdaterIpc.h` 的下载进度占了，撞上就是 `case 值已使用`。
- 窗口缩在托盘时先 `ShowWindow` 再提示，提示里写清出路：要关机用「电源控制」。否则用户从开始菜单关机被拦一次，会以为程序坏了。
- 这一层只挡得住没带 `EWX_FORCE` 的关机。教师端的关机基本都带 force，系统会照样结束进程、照样关 —— 2026-11-21 那次「拦截没成功」的日志里就有 `Blocked a shutdown/restart request`，机器还是关了。
- 所以真正干活的是钩子层：`blockShutdown`（默认开，ini 里 `BlockShutdown=FALSE` 可关）在注入的教室进程里挡住 `ExitWindowsEx`、`InitiateSystemShutdownExW/A`、`InitiateSystemShutdownW/A`、`NtShutdownSystem`、`NtSetSystemPowerState`、`NtInitiatePowerAction`，以及 `CreateProcess*/WinExec` 里对 `shutdown.exe` 的拉起。挡下之后 `VNotifyShutdownBlocked()` 发 `hkb:shutdown:<api>` 回主窗口，界面弹一句提示。
- 那条通知不能复用 `VSendMessageBack()`：它在 `g_customHost` 下直接丢弃回传，而红蜘蛛、伽卡他卡这些宿主就是通用宿主，挡了关机主程序也不知道。
- `EWX_LOGOFF` 的值是 0，判断「是不是注销」不能位测试，要先看有没有关机 / 重启 / 断电位。
- 还没兜住的一种：关机由系统服务发起、且带着 force 时，用户态没有否决点。要封死得在内核里拦（SSDT / inline hook，本项目明确不做）。

## 意见反馈

关于页「检查更新」右边那个「意见反馈」，弹出卡片要用户填**问题描述**和**复现步骤**（都必填），联系方式可选。提交时顺手把当天的运行日志和一份系统信息打包上传。

上传不在主程序里做。主进程里 WinHTTP/Schannel 起不来（`SEC_E_SECPKG_NOT_FOUND`），所以照旧借 `DzjsTrainerUpdater.exe` 子进程跑：`--feedback <包> --meta <文件> --result <文件> --no-elevate`，结果文件第一行 `ok` / `fail`，第二行起是服务端原话或失败原因。父进程等结果文件，最多 90 秒。

- **包里有什么**：`logs/DzjsTrainer-YYYY-MM-DD.log`（超过 1.5 MB 只取尾部）、`logs/DzjsTrainerHooks-debug.log`（384 KB 尾部）、`system.txt`（版本、构建时间、运行环境、窗口 DPI、分辨率、CPU、内存、驱动状态、自我保护、拦截策略、日志目录）、`feedback.txt`（用户填的三段，给人看）。zip 是自己写的 store-only 实现，`XZip.cpp` 不在任何工程里，链接会报 LNK2001。
- **接口**：`update-server/feedback.php`，multipart 两个 part（`meta` / `bundle`），单包上限 8 MB。限流按真实来源 IP 算：一小时内 1 次、24 小时内 2 次，都是滚动窗口，不按自然日重置。落盘 `feedback/<日期>/<时分秒>-<8hex>/`，`meta.json` 里追加一段 `_server`（收件时间、客户端 IP、边缘节点 IP、UA、实际字节数、客户端时间）。
- **请求必须签名，这是"只收官方客户端"的唯一依据。** 接口在公网上，User-Agent 谁都能抄，限流换个 IP 就绕过去了。客户端拿内嵌的 RSA-2048 私钥签 `DZJS-FEEDBACK/1|<ts>|<nonce>|<metaSha>|<bundleSha>`，服务端用 `feedback-pub.php` 里的公钥验。两个摘要都对着服务端**实收的字节**算，header 里声称的值不作数 —— 中途改一个字节就验不过。
- **回复也签名，方向相反。** 成功响应带 `X-Dzjs-Reply: 1|<ts>|<sha256(body)>|<sig>`，客户端用编进 exe 的票据公钥验。这条挡的是"企业代理换张根证书伪造一个 200"：TLS 只证明连到了一个证书合法的对端，证明不了那是我们。两个方向合起来才算双向。
- **私钥加密存放，但别当成保密措施。** `tools/make-feedback-key.py` 把 PKCS#1 DER 用 AES-256-CBC 包成 `FeedbackSignKey.h`。AES 密钥与密文必然同处一个文件，它把"`grep` 一下就有"抬成"要真正逆向"，仅此而已。真正的防线是服务端那四道：验签、时间窗、nonce、限流。
- **四道的顺序不能乱。** 签名格式和时间窗先看（纯字符串判断），再查限流记录（读一个小文件），然后才是 meta / bundle 校验，最后验签，**验签通过后才写 nonce**。写早了，一个随机 nonce 就能把 `.nonce/` 撑满；nonce 用 `fopen(..., 'x')` 原子创建，两个并发的相同请求只有一个能过。
- **换钥必须两边一起改。** 换私钥 → 重跑脚本 → 重编发版，同时替换服务端的 `feedback-pub.php`。只改一半的表现是所有反馈 403「签名验证失败」。开源构建用 `FeedbackSignKey.h.example`（示例私钥，公开），它的请求会被正式服务端拒掉 —— 客户端在签名前就报「当前为开源示例构建」，不用等服务器。
- **换私钥之后必须 `-t:Rebuild`，`touch` 不管用。** `__has_include` 的切换不被增量构建感知：把 `FeedbackSignKey.h` 移走编一次（走 `.example`），再放回来，MSBuild 的 tlog 里没有这个文件的依赖记录，`-t:Build` 会一路"成功"却继续用示例私钥。现象是产物能跑、体积略小，但每次提交都报「当前为开源示例构建」。跟 rc 里那个令牌的坑是一类。
- **`BCryptImportKeyPair` 这条路走不通。** 自拼的 `BCRYPT_RSAKEY_BLOB`（magic、位长、e/n/p1/p2 全对，密钥的数学关系也验过）一直回 `STATUS_UNSUCCESSFUL(0xC0000001)`。最终用 CryptoAPI：`CryptDecodeObjectEx(PKCS_RSA_PRIVATE_KEY)` 一步把 DER 变成它认的 `PRIVATEKEYBLOB`，导入一个临时 key container 再 `CryptSignHash`。注意 `CryptSignHash` 只认 `AT_SIGNATURE` / `AT_KEYEXCHANGE`，不接收导入的句柄，所以那两个槽要先 `CryptGetUserKey` 探一遍 —— 猜错就是 `NTE_BAD_KEYSET(0x80090016)`，从错误码看不出是猜错槽位还是压根没导入。容器签完立刻 `CRYPT_DELETEKEYSET`，不在用户 profile 里留东西。
- **nginx 那两道比 PHP 早。** 站点配了 CF 网段的 `set_real_ip_from` + `real_ip_header CF-Connecting-IP`（直连源站伪造这个头不会被采信），`limit_req` 打在 `location = /update-server/feedback.php` 上：20 次/分钟、突发 5 个。`limit_req` 在 preaccess 阶段生效，早于 nginx 读请求体 —— 被拒的请求连那 8 MB 都不会开始收，PHP 也就不会落临时文件。这个 location 不能 `include enable-php-82.conf`（那本身是个 location 块），是抄了一份进去的；换 PHP 版本要改两处。
- **429 只回一个 `429`。** 还剩多久是客户端该说的话，服务端报个数就够；客户端把这一档翻译成「提交太频繁，过一会儿再试」再显示。其余 4xx 的正文才原样当提示用，所以那边写的都是给人看的中文、不带状态码。
- **看反馈在 `admin.php` 下半页。** 登录后有一个「用户反馈」区块：按时间倒序列出最近 40 条，点开看描述、复现、联系方式、版本构建、真实 IP 与边缘 IP、系统信息快照；`bundle.zip` 走后台下载（链接带 csrf），旁边还有删除。总数与总大小照实算，超过 40 条会写「只列最近 N 条」。
- **`feedback/` 不让直接访问。** nginx 里一条 `location ^~ /update-server/feedback/ { return 404; }` 就挡掉了，下载统一走后台；`admin.php` 读的是文件系统，不受影响。加这条之前，知道完整路径的人能直接把别人的日志包拖走。
- 输入框是真 `EDIT`（两个多行带 `ES_WANTRETURN`，一个单行），不是画出来的 —— 理由和进程选择器那条一样。

三个坑，加别的弹窗时会再遇到：

- **弹窗按钮要登记进 `TargetRect()`。** `FocusableTargets()` 只负责列出可聚焦的 id，真正取矩形、以及 Tab 之后回车触发（`ActivateFocus()` 是取中心点再仿一次点击）都走 `TargetRect()`。漏一个的表现是那按钮只能鼠标点，键盘按下去没反应，而且不报错。
- **模态弹窗得自己画焦点环。** 通用那圈在 `focusTarget >= 0 && !ModalDialogVisible()` 里，弹窗一开就被关掉。更新弹窗、防护面板、进程选择器各画各的，新的也得画，否则 Tab 转过去看不见焦点在哪。
- **Tab 的环要连按钮一起算。** 输入框的 Tab 由 `FeedbackEditProc` 接管（多行 EDIT 默认拿 Tab 当制表符），焦点一旦换到按钮就走主窗口的 `WM_KEYDOWN`。两处各管一段没关系，但必须围成同一个环 —— 否则从按钮回不到输入框，纯键盘用户就卡在按钮上。

行为上两个刻意的选择：点卡片外面**不关**弹窗（里面可能是刚敲的几百字，误点一下全丢比关不掉难受），以及上传过程中不给关（关了也看不到结果，请求还在服务器那边跑）。

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

**照「构建」那节的命令编出来的 exe，卸载不了自己加载的驱动。** `.vcxproj` 把 `IDR_DRIVER_UNLOCK_TOKEN` 绑定到 `DzjsTrainerDriver.unlock.enc.sample`，占位令牌和这次产出的 `.sys` 对不上，驱动会拒绝。日志长这样：

```
[E] Embedded unload token is missing, invalid, or cannot be decrypted
[E] Driver unload authorization failed; service stop was not attempted
```

故意的，不是 bug。要能卸载就得走下面的签名流程重新生成令牌；调试阶段等不及，手工 `sc stop <驱动服务名>` 再重启也行。

`tools/` 下的脚本负责这一段，`Complete-DriverSigning.ps1` 签名并回嵌，`Build-DzjsTrainerRelease.ps1` 校验产物。手工发版时有两个地方容易翻车：

1. **资源里的令牌路径要切成真实文件。** `.rc` 里默认绑的是 `.sample` 占位令牌，`.vcxproj` 的 `EmbeddedDriverUnlockResource` 只提供增量构建的 Inputs，不决定内容。切完要**同时删掉 `.res` 和 stamp**，只删 stamp 不会重跑资源编译。
2. **构建完把 `.rc` 和 `.vcxproj` 还原成 `.sample`。** 还原后 mtime 比旧 `.res` 早，增量构建会静默复用上一次的真令牌产物——本地看着能用，clean 一次就退回占位符。这种「不可复现」只有换机器或 clean 构建才暴露。

另外 UPX 压缩是**不保真**的：`upx -d` 回来会丢掉调试目录和头部校验和，差几 KB。验收判据是「节表一致 + 内嵌资源逐字节可寻址」，不要用整文件哈希比对。

更新通道有**两份端点实现**：`UpdaterMain.cpp`（子进程真正执行的）和 `UpdaterManifest.cpp`（主程序里的静态库）。探测走的是子进程，只改后者等于白改。

### 更新通道公钥

`DzjsTrainerUpdater` 需要一把内嵌的 RSA 公钥来验证服务器返回的时效票据。仓库里不放真实公钥，只有 `UpdaterTicketKey.h.example`（对应私钥 `update-server/ticket-sign.pem.example`，是公开的示例）。

`UpdaterTicket.h` 用 `__has_include` 回退到 `.example`，所以克隆下来直接就能编。代价是编出来的程序连不上正式更新服务器，会提示「此版本内置的是开源示例公钥」。

要构建能接收正式更新的发行版，先把自己的公钥生成进去：

```powershell
python tools\make-ticket-key.py
```

脚本读 `update-server/ticket-sign.pub.der`，生成被 Git 忽略的 `DzjsTrainerUpdater/UpdaterTicketKey.h`。加 `--check` 可以确认现有头文件有没有和 DER 脱节。

这份 DER 也不进版本库，否则任何人都能一键编出一个连得上正式服务器的客户端，占位符就白做了。它从私钥导出：

```powershell
openssl pkey -in update-server\ticket-sign.pem -pubout -outform DER -out update-server\ticket-sign.pub.der
```

## 不要做的事

这些都在 issue 里以「疑似 bug」的形式出现过，结论是设计如此或者改法错误：

- 别用 `Get-AuthenticodeSignature` 的「根不受信任」去推断「必须开测试模式」。它只看第一个嵌入签名，跟内核实际加载时走的检查不是一回事。
- 别把 `/MT` 换成 `/MD`。发布包是静态 CRT，换掉就要求目标机装运行库——这是被人提过的「修复」，实际是回归。
- 别在 UI 线程做更新探测。子进程 + 超时是刻意设计，探测期间的卡顿全部来自这里。
- 别加「绕过 MSBuild 直接用 cl 编驱动」的兜底脚本。绕过去拿不到 Inf2Cat 生成的 `.cat`，驱动装不上；`.sys` 又是嵌进主程序资源的，绕出来的产物想进包还得回头重编一次 exe。想验证驱动改动能编过，cl 单文件检查就够了，犯不上凑一整条 link 命令。
- 别加「便携包」脚本往 exe 旁边拷 VC++ 运行时。工程是静态 CRT，拿发布版 exe 查导入表，`vcruntime140.dll`、`msvcp140.dll`、`ucrtbase.dll` 一个都没有，独立一个文件就是完整的。
- 别提交 `Release`、`obj`、`bin`、PDB、日志、证书、私钥、令牌。

## 提交约定

- 一个 PR 只做一件事。混着格式化、重构和功能修复的 PR 没法评审。
- 改行为要有可复现步骤：系统版本、目标软件版本、操作序列、日志片段。
- 提交 AI 生成的代码之前人工过一遍，重点确认三件事：问题是不是**共性**的（不是只在本机这套环境下成立）、改动是不是**必要**的（有没有更小的改法）、上下文有没有被收窄到只看得见自己那一份日志。把本机环境问题当成普适缺陷提交，比不提 PR 更费时间。
- 安全问题不要在公开 Issue 里贴可复现细节。
