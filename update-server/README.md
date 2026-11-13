# Update server

## Web 后台

打开 `admin.php` 登录即可上传更新器：填写版本号、更新说明，选择 `DzjsTrainerUpdater.exe`，点击“上传并发布”。后台会自动把文件保存到 `releases/`，计算 SHA-256，并写入 `manifest.json`。每次发布都会生成新的随机下载地址：

```text
https://随机值.dzjstrainer.云散皆星河.cn/<admin.php 所在目录>/releases/DzjsTrainerUpdater-随机值.exe
```

`<admin.php 所在目录>` 由脚本从请求路径推导：本目录直接作为站点根目录时为空，放在子目录
（例如 `/update-server/`）时会自动带上。不要把这一段写死 —— 下载地址指向的位置必须和
`releases/` 实际被访问的位置一致，否则客户端会拿到 404。

后台默认禁用。部署前必须通过 PHP 环境变量设置管理员密码：

```text
UPDATE_ADMIN_PASSWORD=你的密码
UPDATE_DOWNLOAD_DOMAIN=dzjstrainer.云散皆星河.cn
UPDATE_DOWNLOAD_SCHEME=https
```

泛域名解析需要将 `*.dzjstrainer.云散皆星河.cn` 指向同一台服务器，并在 Web 服务器证书中包含 `*.dzjstrainer.云散皆星河.cn`。客户端已内置更新接口地址，不需要在 `DzjsTrainer.ini` 中写入 `UpdateServer`。每次启动更新检查时会生成一个随机前缀并访问对应的泛域名，例如 `a1b2c3d4.dzjstrainer.云散皆星河.cn`。

后台地址：`https://你的主站/update-server/admin.php`

如果使用 PHP-FPM/Nginx，请确认 `releases/` 可直接下载，并将 PHP `upload_max_filesize` 和 `post_max_size` 设置为至少 `200M`。

## 时效票据（清单接口必需）

客户端取清单时带上一次性 nonce 和本机时间戳，服务端验过格式后在响应头签回一张 5 分钟有效的票据；客户端用**内置公钥**验签，再验 nonce 回显、时间窗、正文哈希，四项全过才采信清单。所以 `?manifest=1` 不再是一个裸 GET。

与早期版本的区别：这个握手不需要客户端持有任何密钥。旧方案是 HMAC，共享密钥编在客户端里，开源即等于公开签发权；现在客户端只有公钥，泄漏不产生签发能力。

| 文件 | 作用 |
|---|---|
| `update-ticket.php` | 票据实现，`update.php` 会 require 它 |
| `ticket-sign.pem` | RSA-2048 签名私钥，推荐形式，权限 600 |
| `ticket-sign.php` | 同一把私钥的 PHP 形式（`<?php return '-----BEGIN ...'`），用于 `open_basedir` 锁死站点根目录、`.pem` 无法保证不被当静态文件下载的环境。宝塔线上用的就是这一种 |
| `ticket-sign.pem.example` | **公开的**示例私钥，仅供自建流程时跑通，绝不可用于正式发布 |

私钥三种来源，按优先级：环境变量 `DZJS_TICKET_SIGN_KEY` → `ticket-sign.pem` → `ticket-sign.php`。真实私钥都不进版本库（见 `.gitignore`）。

缺私钥时 `?manifest=1` 返回 500 `signing key not installed`，客户端提示「服务器未提供时效票据」—— 明确失败，不会静默放行。

### 公钥是怎么进客户端的

客户端不手写公钥，由脚本从 DER 生成头文件：

```bash
python tools/make-ticket-key.py            # 生成 DzjsTrainerUpdater/UpdaterTicketKey.h
python tools/make-ticket-key.py --check    # 确认头文件没有和 DER 脱节
```

生成的 `UpdaterTicketKey.h` 被 `.gitignore` 挡住。仓库里带的是 `UpdaterTicketKey.h.example`（示例公钥），`UpdaterTicket.h` 用 `__has_include` 回退到它，因此开源克隆不换钥也能编译，只是运行期会以「内置的是开源示例公钥」拒绝连正式服务器。

换钥流程：生成新私钥 → `openssl pkey -in ticket-sign.pem -pubout -outform DER -out ticket-sign.pub.der` → 跑一次 `tools/make-ticket-key.py` → 重新编译发布客户端，与服务端同时切换。旧客户端只认编进去的那把钥，只换服务端会让旧版本收不到更新。

1. Run the publishing script after compiling:

```powershell
.\publish-update.ps1 -Version 1.0.3 -ReleaseExe ..\Release\DzjsTrainerUpdater.exe -Notes "修复窗口识别并改进更新流程。"
```

It creates `releases\DzjsTrainerUpdater.exe`, calculates SHA-256, and writes `manifest.json`.
2. Publish this directory through PHP/IIS/Apache. The desktop client already contains the update endpoint and does not require an INI setting.

The client requests `?manifest=1` only. The legacy `checkupdate`, `getupdateinfo`,
`getnewver`, and `getupdate` parameters have been **removed** from `update.php`.
They answered without any credential check, so anyone could poll the version number
and fetch the package URL while bypassing the ticket entirely — the signature scheme
would not have mattered. Unrecognised queries now return `400 missing action`.
