## 这个 PR 做了什么

<!-- 一句话说明；如果同时包含"整理"和"功能改动"，请拆成两个 PR。 -->

## 类型

- [ ] 修复 bug（行为改变，附复现步骤）
- [ ] UI / 文案
- [ ] 文档
- [ ] 仓库整理（只搬移/改名，不改行为）
- [ ] 构建或工具链

## 如何验证

<!-- 命令、步骤或截图。涉及驱动的改动请说明测试环境（系统版本、是否开启 HVCI、驱动签名方式）。 -->

```powershell
python tools\verify_repo.py --strict
```

## 影响面

- [ ] 主程序（`DzjsTrainer`）
- [ ] UI（`DzjsTrainerUI`）
- [ ] 驱动（`DzjsTrainerDriver`，需要重新签名/生成卸载令牌）
- [ ] Hook（`DzjsTrainerHooks`，32/64 两份都要编）
- [ ] 更新器 / 更新服务
- [ ] 仅文档或脚本

## 检查

- [ ] 没有提交 `Release/`、`obj/`、`*.sys`、`*.pdb`、日志或证书
- [ ] 移动/改名文件时，同步更新了 `.rc` / `.vcxproj` / `#include` 的引用
- [ ] 改动了 `.rc` 或 `.vcxproj` 时，保持原文件编码（GBK / UTF-16LE）
- [ ] 如果需要新增第三方代码，已在 `third_party/README.md` 记录来源与许可证
