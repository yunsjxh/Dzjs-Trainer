# 发布流水线

> 现状：**全手动**。`.github/workflows/ci.yml` 只做仓库自检与 hooks 编译，不产出发布物。
> 本文记录真实链路、产物去向和已知断点，避免下一个人重新踩。

## 一、驱动签名与令牌（`tools/`，Windows）

```powershell
# 1) 生成/轮换卸载令牌密钥对（私钥进 unlock-key-material/，已被 .gitignore 排除）
tools\New-DriverUnlockKeys.ps1

# 2) 签名驱动（构建期 SignMode=Off，签名是后置步骤）
signtool sign /sha1 <证书指纹> /fd SHA256 /tr <RFC3161 时间戳> /td SHA256 `
  Release\DzjsTrainerDriver\DzjsTrainerDriver.sys

# 3) 校验并提取签名者，写入主程序 pin
tools\Get-DriverMachineCode.ps1        # 可选：一机一码材料
tools\New-DriverUnlockToken.ps1        # 生成 .unlock（ECDSA P-256）

# 4) 打包链：Inf2Cat → 签 catalog → 包裹令牌 → 回嵌主程序资源
tools\Complete-DriverSigning.ps1

# 5) 发布前验证：签名者、资源哈希、产物清单
tools\Build-DzjsTrainerRelease.ps1
```

产出的关键不变式：

- `DzjsTrainer/DriverSignerPin.h` 的 subject/thumbprint 必须与**本次**签名证书一致，否则运行时会拒绝自己内嵌的驱动。
- `DzjsTrainerDriver/UnlockPublicKey.h` 的 ECDSA 公钥必须与 `unlock-key-material/` 里的私钥配对，否则卸载令牌验签失败。
- 主程序资源 132（卸载令牌）必须由**本次** `.sys` 对应的私钥签发。

## 二、更新通道

```
客户端                          服务器
DzjsTrainer.exe / Updater.exe   update-server/update.php
  GET ?manifest=1&n=<nonce>&t=<ts>&v=t1
  ← 正文 manifest.json（url / version / sha256 / notes）
  ← 响应头 X-Dzjs-Ticket: 1|nonce|issued|expires|scope|sha256(body)|sig
     · 签名原文 DZJS-TICKET/1|nonce|issued|expires|scope|sha256(body)
     · RSA-2048 / PKCS#1 v1.5 / SHA-256，签名需按字节反转后交给 CryptoAPI
```

- 客户端公钥来自 `DzjsTrainerUpdater/UpdaterTicketKey.h`（`tools/make-ticket-key.py` 从 `update-server/ticket-sign.pub.der` 生成）。
- 服务端私钥 `update-server/ticket-sign.pem` 与 `admin-password.php` 均不入库。
- 下载后校验 SHA-256 → 等主程序退出 → 替换 `DzjsTrainer.exe`。

### 发布一次更新

```powershell
# 服务器侧
update-server/publish-update.ps1 -ReleaseExe <产物路径>
# 或手动：把包放到 update-server/releases/，写 update-server/manifest.json
```

**注意命名陷阱**：`releases/` 下的默认文件名与 `DzjsTrainerUpdater.exe` 同名，而客户端替换的目标是 `DzjsTrainer.exe`。发布前请确认 `manifest.json` 的 `url` 指向的是**主程序包**，并填了非空 `sha256`。

## 三、站点资产同步（服务器 cron）

| 脚本 | 作用 | 写入目标 |
| --- | --- | --- |
| `update-server/sync-release.php` | 从 GitHub Release 同步安装包 | 站点 `Release/` |
| `update-server/sync-version.php` | 生成更新说明 | 站点 `version.txt` |
| `update-server/sync-site.php` | 同步页面资源 | 站点静态文件 |

三条链**互不衔接**：`sync-*.php` 不会写 `manifest.json`，也不写 `releases/`。更新包仍需人工发布。

## 四、版本号有四个来源，必须手动对齐

| 位置 | 当前值 |
| --- | --- |
| `DzjsTrainer/AppPublic.h`（`CURRENT_VERSION`） | 1.0.5.2 |
| `DzjsTrainer/DzjsTrainerVersion.rc` | 1.0.5.2 |
| `CHANGELOG.md` 顶部 | 1.0.5.2 |
| `update-server/update.php` 默认清单 / `manifest.json.example` / `website/version.txt` | 1.0.2 / 1.0.3 / v1.0.1 |

发版清单：改 `AppPublic.h` + `.rc` + `CHANGELOG` + `update.php` 默认值 + `manifest.json` + `website/version.txt`。

## 五、已知断点（照现状记录）

| # | 断点 | 影响 |
| --- | --- | --- |
| 1 | `tools/Protect-DriverUnlockToken.ps1` 被 `.gitignore` 排除且仓库不提供；`Complete-DriverSigning.ps1` 检测到 `.sys.unlock` 时会直接 `throw` | 发布链断在打包步，克隆者无法复现 |
| 2 | 更新器不校验 Authenticode，只比清单 SHA-256 | 包签名无法作为第二道门 |
| 3 | 引导流程在清单 `sha256` 为空时打印"跳过校验"并继续安装 | 清单缺哈希即等于不校验 |
| 4 | `update-server/update.php` 在 `manifest.json` 缺失时仍签发默认清单（空哈希） | 与第 3 条串成完整路径 |
| 5 | `tools/Build-JiYuAv.ps1` 只编驱动/控制端/测试，不签 catalog | 交付前需手工 `Inf2Cat` + 签名 |
| 6 | `DzjsTrainerDriver/DzjsTrainerDriver.inf` 的 `DriverVer=` 为空 | INF 不能直接 `pnputil` 安装 |
| 7 | `third_party/curl` 未参与编译，`DzjsTrainerUpdater/NetUtils.cpp` 未进工程 | 若要启用需自行接入并处理证书校验 |

修其中任何一条都算功能改动，请按 `CONTRIBUTING.md` 单独提 PR 并说明验证方式。
