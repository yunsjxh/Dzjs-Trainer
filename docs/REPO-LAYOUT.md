# 仓库布局

## 目录树

```
DzjsTrainer.sln            主解决方案（主程序 Win32 / 驱动 x64）
JiYuAv.sln                 独立特征扫描驱动解决方案（全 x64）
LICENSE  README.md  CHANGELOG.md  CONTRIBUTING.md
.editorconfig  .gitattributes  .gitignore

.github/
  workflows/ci.yml         仓库自检 + hooks 编译
  ISSUE_TEMPLATE/          Bug 表单（要求先填日志）
  pull_request_template.md

docs/                      设计与流程文档（见 docs/README.md）
assets/branding/           图标与品牌资源（.ico 被资源脚本引用，见下文）
include/                   跨工程共享头（WindowCaptureProtection.h）
resources/                 构建期固定输入（卸载令牌占位）
scripts/                   开发者本地脚本（build-hooks.cmd）
third_party/               第三方源码（见 third_party/README.md）

DzjsTrainer/               主程序：启动编排、驱动装载、签名门禁、注入协调、用户态反制
DzjsTrainerUI/             自绘 UI（静态库）+ TeacherEndpoint 协议实现
DzjsTrainerDriver/         x64 WDM 驱动（现役 *64.c）
  legacy-x86/              2019 年 x86 驱动源码，不参与编译
DzjsTrainerHooks/          注入到 StudentMain.exe 的兼容层 + 内嵌 MinHook
DzjsTrainerUpdater/        更新器（Win32 静态库 / 独立 exe）
JiYuAvKernel/  JiYuAvCtl/  JiYuAvShared/  JiYuAvPatternTests/
CaptureProbe/              截屏对抗探针（研究工具，不进解决方案）
GuardTerminator/           独立守护进程终结器
PhoneDialogProbe/  ExitInputSimulator/  DzjsTrainerInjector64/
DriverPublisherTests/      驱动签名策略单测与探针
tools/                     签名/发布/令牌/特征/媒体/自检脚本
update-server/             PHP 更新后端
website/                   官网
```

## 为什么有文件"必须"留在根目录

MSBuild 的 `$(SolutionDir)` 和 `.rc` 里的 `..\` 都相对解决方案/工程目录解析。以下条目被构建脚本直接引用，移动它们等于改构建配置，所以保持原位：

| 条目 | 引用方 |
| --- | --- |
| `DzjsTrainer.sln`、`JiYuAv.sln` | VS、`tools/*.ps1` |
| `assets/branding/DzjsTrainerLogo.ico` 等 | 3 个 `.rc` 的 `ICON`、3 个 `.vcxproj` 的 `<Image>`、3 个 `.filters` |
| `resources/DzjsTrainerDriver.unlock.enc.sample` | `DzjsTrainer.rc`（资源 132）、`DzjsTrainer.vcxproj`（`$(SolutionDir)resources\…`）、`tools/Complete-DriverSigning.ps1` |
| `include/WindowCaptureProtection.h` | `DzjsTrainer/App.cpp`、`DzjsTrainerUI/MainWindow.cpp`、`DzjsTrainerUI/BugReportWindow.cpp` 的 `#include "../include/…"` |

改动这些位置时，请同步改上表中的**每一处**，再跑 `python tools/verify_repo.py --strict`。

## 不可移动/不应改动的内容

- `third_party/curl/**`：保留上游目录结构（curl 内部大量相对 `#include`）。未参与编译，仅为将来启用时保留；`DzjsTrainerUpdater.vcxproj` 的 `IncludePath` 已指向新位置。
- `DzjsTrainerDriver/legacy-x86/**`：内容不改，仅存档。它们与现役 `*64.c` 同名同职责，但使用 SSDT Hook/CR0/`_asm`，在 x64 下无法编译。
- `.rc` 文件编码：`DzjsTrainerUI.rc` 是 GBK，`DzjsTrainer.rc`、`DzjsTrainerHooks.rc`、`DzjsTrainerDriver*.rc` 是 UTF-16LE。用只支持 UTF-8 的编辑器保存会破坏构建。
- `DzjsTrainerUpdater/UpdaterTicketKey.h`：由 `tools/make-ticket-key.py` 生成，被 `.gitignore` 挡住；仓库里只有 `.example`。
- `update-server/` 下的 `manifest.json`、`releases/`、`ticket-sign.pem*`、`admin-password.php`：服务端私有材料，`.gitignore` 已排除。

## 本轮整理做了什么（纯搬移，无功能改动）

| 原位置 | 新位置 |
| --- | --- |
| 根目录 7 个图标/品牌文件 | `assets/branding/` |
| 根目录 `WindowCaptureProtection.h` | `include/` |
| 根目录 `DzjsTrainerDriver.unlock.enc.sample` | `resources/` |
| 根目录 `build-hooks.cmd` | `scripts/`（`ROOT` 改为 `%~dp0..\`） |
| `DzjsTrainer/DRIVER_SIGNATURE_POLICY.md` | `docs/DRIVER-SIGNATURE-POLICY.md` |
| `DzjsTrainerUpdater/curl/`（303 文件，未编译） | `third_party/curl/`（`vcxproj` 的 `IncludePath` 已更新） |
| `DzjsTrainer/MemoryModule.*`、`XZip.*`、`XUnzip.*`（未编译） | `third_party/MemoryModule/`、`third_party/XZip/` |
| `DzjsTrainerDriver/{Driver,Protect,Monitor,UnExp}.{c,h}` | `DzjsTrainerDriver/legacy-x86/` |
| 根目录 `analysis/`、`Dzjs-Trainer-全解.md`（本地笔记） | `docs/analysis/`（已在 `.gitignore`，不进仓库） |

同时：修正 `LICENSE` 里残留的 Git 冲突标记（此前 MIT 正文被截断）；删除 `.vcxproj`/`.filters` 中指向不存在文件 `..\icon1.ico` 的 `<Image>` 项；重排 `.gitignore` 与 `.gitattributes`；新增 `.editorconfig`、`CONTRIBUTING.md`、本目录文档与 CI。

**没有改动**任何源文件逻辑、第三方代码内容、资源二进制或 `update-server` 部署路径。
