# Dzjs Trainer

当前版本：`v1.0.1`

Dzjs Trainer 是基于 JiYu Trainer 翻新和持续维护的 Windows 桌面项目，面向经授权的教学机房、设备维护与兼容性研究场景。

本项目由云散皆星河维护，感谢 JiYu Trainer 原作者快乐的梦鱼。项目已获得原作者 @快乐的梦鱼 的口头许可，在保留原项目贡献说明的基础上继续演进。

> 请仅在你拥有、管理或获得明确授权的设备与网络中使用本项目。不要将其用于绕过组织的安全管理、侵犯他人隐私或影响他人设备正常使用。

## 项目概览

相较于早期 JiYu Trainer，Dzjs Trainer 主要完成了以下翻新工作：

- 重新编写并维护 x64 系统驱动，支持独立签名和发布流程。
- 替换旧驱动中容易引发系统稳定性问题的实现路径。例如，将容易触发 PatchGuard 风险的 SSDT Hook 方案替换为内核回调等等效实现。
- 重构防护与驱动加载流程，增加驱动签名校验、发布者约束和运行状态诊断。
- 翻新主界面，补充退出确认、信息流保护、预览与设备诊断等交互能力。
- 增加对机房管理助手的适配支持。
- 保留并完善极域相关的兼容与反控能力。

当前重点维护极域电子教室与机房管理助手环境。Gakataka、红蜘蛛等电子教室产品属于后续适配计划，不代表当前版本已经完整支持。

## 特性

- Windows 桌面管理界面与托盘运行模式。
- 教学终端兼容性检测、状态诊断与运行日志。
- 可配置的设备防护策略和信息流保护开关。
- x64 内核驱动与驱动签名验证链路。
- 图片与视频预览、选择框与相关界面能力。
- 极域电子教室、机房管理助手的兼容适配基础。

## 架构

| 目录 | 说明 |
| --- | --- |
| `JiYuTrainer` | 主程序、驱动加载和资源嵌入逻辑 |
| `JiYuTrainerUI` | 原生 Windows UI 与交互逻辑 |
| `JiYuTrainerDriver` | x64 WDM 驱动源码 |
| `JiYuTrainerHooks` | 兼容层与 Hook 模块源码 |
| `JiYuTrainerUpdater` | 更新组件源码 |
| `JiYuAvKernel` / `JiYuAvCtl` | 辅助驱动与控制工具 |
| `tools` | 驱动构建、签名、令牌与验证脚本 |
| `DzjsTrainer.sln` | 主解决方案 |

主程序使用 x86 配置构建；驱动使用 x64 配置构建。解决方案已配置二者的依赖关系。

## 环境要求

- Windows 10 或更高版本。
- Visual Studio 2022，包含 C++ 桌面开发和 MSBuild。
- Windows 10/11 SDK。
- 构建 `JiYuTrainerDriver` 时需要安装与系统匹配的 WDK。

## 构建

使用 Visual Studio 打开 `DzjsTrainer.sln`，选择 `Release | x86` 后生成整个解决方案。

也可以在开发者命令行中执行：

```powershell
MSBuild.exe DzjsTrainer.sln /t:Build /p:Configuration=Release /p:Platform=x86 /m
```

驱动可单独构建：

```powershell
MSBuild.exe JiYuTrainerDriver\JiYuTrainerDriver.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

构建产物会写入被 Git 忽略的 `Release` 目录，不应提交到源码仓库。

## 驱动签名与发布

仓库不包含发布者的代码签名证书、私有解锁密钥或实际解锁令牌。`JiYuTrainerDriver.unlock.enc.sample` 是公开构建所需的占位资源。

发布维护者需要自行完成以下工作：

1. 使用自己的证书签名最终驱动文件。
2. 运行 `tools\Complete-DriverSigning.ps1` 更新驱动包，并重新嵌入主程序资源。
3. 使用 `tools\Build-JiYuTrainerRelease.ps1` 验证签名者、资源哈希与发布产物。

如需启用实际解锁令牌流程，请在本地使用 `tools\New-DriverUnlockKeys.ps1` 和 `tools\New-DriverUnlockToken.ps1` 生成自己的密钥与令牌；私有材料不得提交到仓库。

## 支持范围

| 类型 | 状态 |
| --- | --- |
| 极域电子教室 | 持续维护 |
| 机房管理助手 | 已提供适配基础 |
| Gakataka | 计划中 |
| 红蜘蛛 | 计划中 |

不同系统版本、终端版本和安全策略的行为可能不同。请先在隔离测试环境验证，再进行经授权的部署。

## 贡献

欢迎提交 Issue、修复建议和 Pull Request。提交前请确保：

- 不提交 `Release`、`obj`、`bin`、日志、PDB 或其他构建产物。
- 不提交证书、私钥、令牌、设备标识或包含敏感环境信息的日志。
- 为功能修改提供可复现的测试步骤。
- 保持现有 C++ 与 PowerShell 代码风格，避免无关格式化。

安全问题请不要在公开 Issue 中披露可复现细节，应先联系维护者。

## 致谢

- JiYu Trainer 原作者：快乐的梦鱼。
- 当前维护与翻新：云散皆星河。
- 第三方组件包括 curl、mhook、MemoryModule、XZip/XUnZip、dnlib 等；请分别遵循其许可证要求。

## 许可证

本项目采用 [MIT License](LICENSE)。使用、修改和分发时请保留原有版权与许可证声明。
