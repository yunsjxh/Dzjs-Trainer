# Update server

## Web 后台

打开 `admin.php` 登录即可上传更新器：填写版本号、更新说明，选择 `DzjsTrainerUpdater.exe`，点击“上传并发布”。后台会自动把文件保存到 `releases/`，计算 SHA-256，并写入 `manifest.json`。每次发布都会生成新的随机下载地址：

```text
https://随机值.dzjstrainer.云散皆星河.cn/<admin.php 所在目录>/releases/DzjsTrainerUpdater-随机值.exe
```

`<admin.php 所在目录>` 由脚本从请求路径推导：本目录直接作为站点根目录时为空，放在子目录
（例如 `/update-server/`）时会自动带上。不要把这一段写死 —— 下载地址指向的位置必须和
`releases/` 实际被访问的位置一致，否则客户端会拿到 404。

后台默认禁用 —— 没有配置口令时它不提供任何上传能力。口令有两种来源，取到即用：

1. 环境变量 `UPDATE_ADMIN_PASSWORD`（优先级最高）；
2. 同目录的 `admin-password.php`，内容为 `<?php return '你的口令';`。

推荐第二种。原因不是口味问题：PHP-FPM 的 `env[]` 得写进 `php-fpm.conf`，而宝塔一改 PHP 参数就会重写那个文件，口令会静默消失、后台重新变成禁用；`admin-password.php` 落在站点目录里，没人会去动它，改口令也不需要重启 PHP。用 `.php` 而不是 `.txt` 也是刻意的：直接 GET 只会得到空响应，不会像静态文件那样被原样下载走。

这个文件等价于「能上传更新包并改写清单」的权限，和服务端私钥同级，绝不能进版本库（见 `.gitignore`）。

```text
UPDATE_DOWNLOAD_DOMAIN=dzjstrainer.云散皆星河.cn
UPDATE_DOWNLOAD_SCHEME=https
```

早期版本把口令写死成 `ChangeMe-2026!` 兜底，而 `admin.php` 是公网可达的 —— 等于谁都能登录、上传自己的程序并让服务端签名。这个兜底已经删掉，且永远不会加回来：宁可后台不可用，也不能有一个公开的默认口令。

泛域名解析需要将 `*.dzjstrainer.云散皆星河.cn` 指向同一台服务器，并在 Web 服务器证书中包含 `*.dzjstrainer.云散皆星河.cn`。客户端已内置更新接口地址，不需要在 `DzjsTrainer.ini` 中写入 `UpdateServer`。每次启动更新检查时会生成一个随机前缀并访问对应的泛域名，例如 `a1b2c3d4.dzjstrainer.云散皆星河.cn`。

后台地址：`https://你的主站/update-server/admin.php`

如果使用 PHP-FPM/Nginx，请确认 `releases/` 可直接下载，并将 PHP `upload_max_filesize` 和 `post_max_size` 设置为至少 `200M`。

## 站点更新日志自动同步

站点首页的版本号与更新日志读的是站点根目录的 `version.txt`（`script.js` 里 `fetch('version.txt')`）。
这个文件由 `sync-version.php` 从 GitHub Releases 生成，不需要手改。

```text
格式（块之间空行分隔）：
v1.0.4.2          ← 第 1 行版本号，第一块驱动全站版本号显示
2026.11.14        ← 第 2 行日期（published_at 按北京时间落日期）
版本 v1.0.4.2     ← 第 3 行标题
- 一条更新点       ← 其余每行一条
```

| 文件 | 作用 |
|---|---|
| `sync-version-lib.php` | 拉 API、把 release 正文转成上述纯文本、原子写入 |
| `sync-version.php` | 命令行入口，只允许 CLI（HTTP 访问直接 403） |

```bash
php sync-version.php              # 内容没变就不动文件
php sync-version.php --dry-run    # 只打印，不落盘
php sync-version.php --force      # 内容相同也重写
```

定时任务（每小时第 17 分，`flock` 防重入）：

```cron
17 * * * * flock -xn /tmp/dzjs-sync-version.lock /usr/bin/sudo -u www /www/server/php/82/bin/php "/www/wwwroot/DzjsTrainer.云散皆星河.cn/update-server/sync-version.php" >> "/www/wwwroot/DzjsTrainer.云散皆星河.cn/update-server/sync-version.log" 2>&1
```

后台 `admin.php` 也有「立即同步 GitHub 更新」按钮。它不调外部进程（宝塔默认禁用
`exec`/`shell_exec`），而是 `require_once` 同步库在同一次请求里跑完。

几个刻意的取舍：

- **这个通道只写 `version.txt`**，不碰任何 PHP 或页面文件。会覆盖 `download/index.php`
  这类站点文件的只有下一节的 `sync-site.php`，它有独立的白名单。
- 正文里 markdown 的折行会按字符类型拼接：中文与中文直接相连，中英文之间补空格，
  否则会出现「三段 （%u.%u.%u）」这种空洞。
- 列表项不足 3 条时用段落首句补；列表项本身够多就不拿段落凑数。
- `### 发布包说明` 里的元信息（主程序版本、目标平台、仅包含主程序…）会被过滤，
  它们对首页读者没有意义。
- 某个 release 正文过滤完一条不剩（`v1.0.0` 就是这种），沿用现有文件里同名版本块，
  避免自动同步把站点上原本更详细的内容清空。
- 非 `v` 开头的手写块（例如「项目翻新」）原样保留在末尾。
- 内容没有变化时不写文件、不改 mtime；变化时先备份 `version.txt.bak` 再原子替换。

可选：在 `github-token.php` 里 `<?php return 'ghp_xxx';`（或设环境变量 `GITHUB_TOKEN`），
把 API 限额从 60 次/小时提到 5000。仓库是公开的，不配也能用。

## 站点静态文件自动部署

仓库 `website/` 下的部分文件会同步到站点根目录，改 GitHub 就上线。

```bash
php sync-site.php              # 内容没变就不动文件
php sync-site.php --dry-run    # 只列出会更新哪些文件
php sync-site.php --force      # 内容相同也重写
```

| 文件 | 作用 |
|---|---|
| `sync-site-lib.php` | 拉 tree/blob、比对、原子写入、备份 |
| `sync-site.php` | 命令行入口，只允许 CLI（HTTP 访问直接 403） |

定时任务（每小时第 27 分，与 version 同步错开）：

```cron
27 * * * * flock -xn /tmp/dzjs-sync-site.lock /usr/bin/sudo -u www /www/server/php/82/bin/php "/www/wwwroot/DzjsTrainer.云散皆星河.cn/update-server/sync-site.php" >> "/www/wwwroot/DzjsTrainer.云散皆星河.cn/update-server/sync-site.log" 2>&1
```

后台 `admin.php` 也有「立即部署站点文件」按钮。

### 白名单（默认拒绝，不是黑名单）

| 会部署 | 不部署 |
|---|---|
| `script.js`、`404.html`、`download/index.php`、`assets/*` 下的图片 | `index.html`、`styles.css`、`version.txt`、`tools/`、`.verification/` |

`index.html` 与 `styles.css` 是**维护者自己维护的文案版本**，跟仓库里面向开源/
合规的措辞不是同一份 —— 自动部署绝不能把它们冲掉，所以白名单里没有它们，
`dzjs_site_allowed()` 里还额外写了一条显式拒绝，防止将来被误加进去。

将来仓库新增的文件也不会自动上线；要让它上线，必须在 `dzjs_site_allowed()` 里
明确加一条。这个方向是刻意的：自动部署的失败方向应该是「没更新」，不是「覆盖了不该覆盖的」。

### 几个实现取舍

- **用 git blob SHA 判断有没有变化**，不维护状态文件：`sha1("blob <字节数>\0" + 内容)`
  与 tree 里记的 SHA 一致就跳过。状态文件丢了也只是重算一次，不会误判。
- 落盘前再校验一次：blob 接口给的内容算出的 SHA 必须等于 tree 里的 SHA，否则报错不写。
- 只差换行（服务器 CRLF / 仓库 LF）时跳过。PHP、JS 执行没有区别，不值得为它改一个
  本来跑得好好的文件。
- 覆盖前备份到 `.site-backup/<时间戳>/`，只保留最近 5 批；nginx 侧对该目录返回 404。
- 两个 `*-lib.php` 都带 include 守卫：直接当 URL 打会 403，只有 CLI 入口和
  `admin.php` 能用。

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
