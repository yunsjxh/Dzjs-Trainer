# 贡献指南

> 本文件只讲流程。构建步骤见 [docs/BUILD.md](docs/BUILD.md)，目录职责见 [docs/REPO-LAYOUT.md](docs/REPO-LAYOUT.md)，架构概览见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 适用范围

请仅在你拥有、管理或获得明确授权的设备与网络中使用和测试本项目。提交前请确认改动不会让工具在未授权环境中产生副作用。

## 提交流程

1. **先开 issue**（Bug 用仓库的 issue 表单，务必附日志与复现步骤）。项目没有专职 reviewer，先对齐范围能少走弯路。
2. Fork 本仓库，从 `main` 拉分支：`fix/<短描述>` 或 `docs/<短描述>`。
3. 一个 PR 只做一件事。整理类改动与功能改动分开提。
4. PR 描述里写清楚：改了什么、为什么、**怎么验证**（命令或步骤）、影响哪些工程/产物。

## 不要提交

- `Release/`、`obj/`、`bin/`、`x64/`、`JiYuAvRelease/` 等构建产物，以及 `*.sys`、`*.pdb`、`*.lib`、`*.exe`。
- 证书、私钥、卸载令牌、`.enc`/`.unlock` 文件、`unlock-key-material/`。
- 更新服务器的私钥、`admin-password.php`、`ticket-sign.pem*`。
- 含设备标识、机房名、账号或个人路径的日志。日志请先脱敏再贴。
- `DzjsTrainerUpdater/UpdaterTicketKey.h`（由 `tools/make-ticket-key.py` 生成，仓库只带 `.example`）。

`.gitignore` 已覆盖上述内容；新增产物类型时请顺手补规则。

## 代码风格

- C/C++：4 宽制表符缩进，与现有文件保持一致的命名（`hkXxx` detour、`VXxx` 导出的钩子管理函数、`Xxx` 内部函数）。
- PowerShell：4 空格，动词-名词命名，禁止在脚本里硬编码本机路径。
- 新增源文件请同时加入对应 `.vcxproj`；`Driver` 工程只编译 `*64.c`，`legacy-x86/` 下的文件不参与构建。
- 资源脚本（`.rc`）编码不要改动：UI 那份是 GBK，驱动与 Hook 两份是 UTF-16LE。

## 提交前自检

```powershell
python tools\verify_repo.py --strict
```

该脚本检查：冲突标记、`.rc`/`.vcxproj` 里引用的资源是否存在、`#include` 是否可解析、是否误提交构建产物、解决方案引用的工程是否存在、根目录是否只有约定条目。CI 会跑同一份检查。

能本地编译的部分请至少编译一次（`DzjsTrainerHooks` 与 `DzjsTrainerUI` 不依赖 WDK）。

## 涉及签名与发布的改动

以下内容需要维护者本地私钥或证书，外部贡献者无法端到端验证，请只提 issue 或提供补丁交由维护者验证：

- `tools/Complete-DriverSigning.ps1`、`tools/Build-DzjsTrainerRelease.ps1`、`tools/New-DriverUnlock*.ps1`
- `DzjsTrainer/DriverSignerPin.h`、`DzjsTrainerDriver/UnlockPublicKey.h`
- `update-server/` 的票据密钥相关脚本
