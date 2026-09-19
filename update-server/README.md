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

1. Run the publishing script after compiling:

```powershell
.\publish-update.ps1 -Version 1.0.3 -ReleaseExe ..\Release\DzjsTrainerUpdater.exe -Notes "修复窗口识别并改进更新流程。"
```

It creates `releases\DzjsTrainerUpdater.exe`, calculates SHA-256, and writes `manifest.json`.
2. Publish this directory through PHP/IIS/Apache. The desktop client already contains the update endpoint and does not require an INI setting.

The client requests `?manifest=1`. The legacy `checkupdate`, `getupdateinfo`,
`getnewver`, and `getupdate` query parameters remain available.
