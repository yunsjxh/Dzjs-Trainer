# 文档索引

Dzjs Trainer 是 JiYu Trainer 的翻新版：面向**经授权**的教学机房、设备维护与兼容性研究。项目由云散皆星河维护，原作者为快乐的梦鱼（imengyu），MIT 协议。

| 文档 | 内容 | 什么时候看 |
| --- | --- | --- |
| [README](../README.md) | 项目简介、特性、支持范围、发布说明入口 | 第一次接触 |
| [ARCHITECTURE](ARCHITECTURE.md) | 进程/驱动/Hook/网络/更新的整体结构与关键机制 | 想读懂代码 |
| [REPO-LAYOUT](REPO-LAYOUT.md) | 目录职责、哪些文件必须在根目录、为什么有 `third_party/` 与 `legacy-x86/` | 找文件、提 PR 前 |
| [BUILD](BUILD.md) | 从零复现构建的**真实**顺序与四个已知缺口 | 要编译 |
| [RELEASE-PIPELINE](RELEASE-PIPELINE.md) | 签名、打包、更新服务、站点同步的现状与断点 | 要发版 |
| [DRIVER-SIGNATURE-POLICY](DRIVER-SIGNATURE-POLICY.md) | 驱动发布者白名单与内核 guard 的规则 | 改驱动加载逻辑 |
| [CONTRIBUTING](../CONTRIBUTING.md) | 提交流程、代码风格、禁提交清单、自检命令 | 要提 PR |
| [CHANGELOG](../CHANGELOG.md) | 版本变更历史 | 查某行为是哪版引入的 |

## 子项目自带文档

| 位置 | 内容 |
| --- | --- |
| [JiYuAvKernel/README.md](../JiYuAvKernel/README.md) | 独立特征扫描驱动：能力、构建、特征库用法、架构边界 |
| [CaptureProbe/README.md](../CaptureProbe/README.md) | 截屏对抗探针：14 条抓屏路径、视频靶、判定规则、实测结论 |
| [PhoneDialogProbe/README.md](../PhoneDialogProbe/README.md) | WinForms 对话框诊断 DLL 的用法 |
| [update-server/README.md](../update-server/README.md) | 更新后端部署、票据签名、同步任务 |
| [third_party/README.md](../third_party/README.md) | 第三方组件版本、许可证、是否参与编译 |
| [DzjsTrainerDriver/legacy-x86/](../DzjsTrainerDriver/legacy-x86/) | 2019 年 x86 驱动源码（不参与编译，仅存档） |

## 快速定位

| 想改… | 去这里 |
| --- | --- |
| 主界面、面板、退出确认 | `DzjsTrainerUI/MainWindow.cpp` |
| 注入决策、窗口反控、心跳 | `DzjsTrainer/TrainerWorker.cpp` |
| 注入后 hook 了哪些 API | `DzjsTrainerHooks/HookRegistry.cpp` + 同名 `.cpp` 里的 detour |
| 驱动 IOCTL 与保护策略 | `DzjsTrainerDriver/Driver64.c`、`Protection64.c` |
| 驱动加载与签名门禁 | `DzjsTrainer/DriverLoader.cpp`、`DriverSignaturePolicy.cpp` |
| 更新协议与票据 | `DzjsTrainerUpdater/UpdaterManifest.cpp`、`UpdaterTicket.h` |
| 极域报文 | `DzjsTrainer/JyUdpAttack.cpp`、`DzjsTrainerUI/TeacherEndpoint/protocol.cpp` |
