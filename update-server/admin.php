<?php
declare(strict_types=1);

// Simple upload console for the update server. Keep this file beside update.php.

// 允许本次请求 include 同步库（sync-version-lib.php / sync-site-lib.php）。
// 它们各自带守卫：没有这个常量就直接访问会 403，避免把内部脚本当 URL 打。
define('DZJS_LIB_CONTEXT', 1);
$root = __DIR__;
$manifestPath = $root . DIRECTORY_SEPARATOR . 'manifest.json';
$releaseDir = $root . DIRECTORY_SEPARATOR . 'releases';
$adminPassword = (string)(getenv('UPDATE_ADMIN_PASSWORD') ?: '');
if ($adminPassword === '') {
    // 环境变量缺失时退回同目录的 admin-password.php，与 ticket-sign.php 同一套做法。
    // 用 PHP 文件而不是 .txt / .ini：直接 GET 只会拿到一个空响应，不会像静态文件
    // 那样被原样下载走。宝塔改 PHP 参数会重写 php-fpm.conf，写在那里的 env 会丢，
    // 这个文件不会被任何人碰。
    $passwordFile = $root . DIRECTORY_SEPARATOR . 'admin-password.php';
    if (is_file($passwordFile)) {
        try {
            $loaded = include $passwordFile;
            if (is_string($loaded)) $adminPassword = trim($loaded);
        } catch (Throwable $e) {
            $adminPassword = '';
        }
    }
}
$downloadDomain = (string)(getenv('UPDATE_DOWNLOAD_DOMAIN') ?: 'dzjstrainer.xn--9kq396ceqaq4si9m.cn');
$downloadScheme = strtolower((string)(getenv('UPDATE_DOWNLOAD_SCHEME') ?: 'https')) === 'http' ? 'http' : 'https';

// The package is written beside this script, but the script is not necessarily
// served from the document root. Derive the public directory from the request so
// the advertised URL points at the file that was actually written. Hardcoding
// '/releases/' produced URLs that 404 whenever the files are served from a
// subdirectory such as /update-server/.
$scriptName = str_replace('\\', '/', (string)($_SERVER['SCRIPT_NAME'] ?? ''));
$basePath = rtrim(str_replace('\\', '/', dirname($scriptName)), '/');
if ($basePath === '.' || $basePath === '/') $basePath = '';

session_start([
    'cookie_httponly' => true,
    'cookie_samesite' => 'Lax',
    'cookie_secure' => !empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off',
]);

function esc(string $value): string
{
    return htmlspecialchars($value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function token(int $bytes = 5): string
{
    try {
        return bin2hex(random_bytes($bytes));
    } catch (Throwable $e) {
        return bin2hex(pack('N', mt_rand())) . bin2hex(pack('N', mt_rand()));
    }
}

function readManifest(string $path): array
{
    $default = ['version' => '0.0.0', 'notes' => 'No release notes supplied.', 'url' => '', 'sha256' => ''];
    if (!is_file($path)) return $default;
    $value = json_decode((string)file_get_contents($path), true);
    if (!is_array($value)) return $default;
    $value = array_merge($default, $value);
    foreach (array_keys($default) as $key) $value[$key] = is_scalar($value[$key]) ? (string)$value[$key] : '';
    return $value;
}

function writeManifest(string $path, array $manifest): void
{
    $temporary = $path . '.tmp.' . token(6);
    $json = json_encode($manifest, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT) . PHP_EOL;
    if (file_put_contents($temporary, $json, LOCK_EX) === false) throw new RuntimeException('无法写入临时清单文件。');
    if (!rename($temporary, $path)) {
        @unlink($path);
        if (!rename($temporary, $path)) { @unlink($temporary); throw new RuntimeException('无法替换 manifest.json。'); }
    }
}

function go(string $message = ''): void
{
    if ($message !== '') $_SESSION['update_message'] = $message;
    header('Location: ?');
    exit;
}

$manifest = readManifest($manifestPath);
$error = '';
$message = (string)($_SESSION['update_message'] ?? '');
unset($_SESSION['update_message']);
$action = (string)($_POST['admin_action'] ?? '');

if ($action === 'login') {
    if ($adminPassword === '') {
        $error = '服务器未配置管理口令，后台已禁用。请创建 admin-password.php（内容为 <?php return \'口令\';），或设置环境变量 UPDATE_ADMIN_PASSWORD。';
    } elseif (hash_equals($adminPassword, (string)($_POST['password'] ?? ''))) {
        session_regenerate_id(true);
        $_SESSION['update_admin'] = true;
        $_SESSION['update_csrf'] = token(16);
        go('登录成功。');
    } else {
        $error = '密码错误。';
    }
}
if ($action === 'logout') {
    $_SESSION = [];
    session_destroy();
    header('Location: ?');
    exit;
}
if ($action === 'upload' && !empty($_SESSION['update_admin'])) {
    $csrf = (string)($_POST['csrf'] ?? '');
    $version = trim((string)($_POST['version'] ?? ''));
    $notes = trim((string)($_POST['notes'] ?? ''));
    $upload = $_FILES['package'] ?? null;
    if (empty($_SESSION['update_csrf']) || $csrf === '' || !hash_equals((string)$_SESSION['update_csrf'], $csrf)) $error = '页面已过期，请刷新后重试。';
    elseif (!preg_match('/^\d+(?:\.\d+){1,3}(?:[-+][A-Za-z0-9.-]+)?$/', $version)) $error = '版本号格式不正确，例如 1.0.4。';
    elseif (strlen($notes) > 4000) $error = '更新说明不能超过 4000 个字符。';
    elseif (!is_array($upload) || (int)($upload['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_OK) $error = '请选择上传成功的 EXE 文件。';
    elseif (strtolower(pathinfo((string)($upload['name'] ?? ''), PATHINFO_EXTENSION)) !== 'exe' || (int)$upload['size'] <= 0 || (int)$upload['size'] > 200 * 1024 * 1024) $error = '文件必须是 200 MB 以内的 EXE。';
    elseif (!is_dir($releaseDir) && !mkdir($releaseDir, 0755, true) && !is_dir($releaseDir)) $error = '无法创建 releases 目录。';
    if ($error === '') {
        $fileName = 'DzjsTrainerUpdater-' . token(6) . '.exe';
        $target = $releaseDir . DIRECTORY_SEPARATOR . $fileName;
        if (!move_uploaded_file((string)$upload['tmp_name'], $target)) $error = '保存上传文件失败。';
        else {
            $sha256 = hash_file('sha256', $target);
            if ($sha256 === false) { @unlink($target); $error = '无法计算文件 SHA-256。'; }
            else {
                $newManifest = [
                    'version' => $version,
                    'notes' => $notes !== '' ? $notes : 'No release notes supplied.',
                    'url' => $downloadScheme . '://' . token(4) . '.' . $downloadDomain . $basePath . '/releases/' . rawurlencode($fileName),
                    'sha256' => strtolower($sha256),
                ];
                try { writeManifest($manifestPath, $newManifest); $manifest = $newManifest; go('发布成功，客户端清单已更新。'); }
                catch (Throwable $e) { @unlink($target); $error = $e->getMessage(); }
            }
        }
    }
}

// 站点首页的版本号与更新日志来自站点根的 version.txt（由 sync-version.php 从
// GitHub Releases 生成）。这里放一个手动触发的入口：不调外部进程（exec 被禁），
// 直接 include 同步库在同一次请求里跑完。
if ($action === 'syncversion' && !empty($_SESSION['update_admin'])) {
    $csrf = (string)($_POST['csrf'] ?? '');
    if (empty($_SESSION['update_csrf']) || $csrf === '' || !hash_equals((string)$_SESSION['update_csrf'], $csrf)) {
        $error = '页面已过期，请刷新后重试。';
    } else {
        require_once __DIR__ . '/sync-version-lib.php';
        $sync = dzjs_sync_run(dirname(__DIR__) . '/version.txt');
        if (!$sync['ok']) {
            $error = '同步失败：' . $sync['error'];
        } elseif ($sync['changed']) {
            go('已同步 GitHub 更新：' . $sync['version'] . '，站点更新日志已刷新。');
        } else {
            go('无需更新，站点已经是最新：' . $sync['version'] . '。');
        }
    }
}

// 站点静态文件（script.js / 404.html / download/index.php / assets 图片）来自
// GitHub 的 website/ 目录。首页 index.html 与 styles.css 是维护者自己维护的文案
// 版本，刻意不在白名单里 —— 自动部署绝不能把线上文案冲掉。
if ($action === 'syncsite' && !empty($_SESSION['update_admin'])) {
    $csrf = (string)($_POST['csrf'] ?? '');
    if (empty($_SESSION['update_csrf']) || $csrf === '' || !hash_equals((string)$_SESSION['update_csrf'], $csrf)) {
        $error = '页面已过期，请刷新后重试。';
    } else {
        require_once __DIR__ . '/sync-site-lib.php';
        $deploy = dzjs_site_run();
        if (!$deploy['ok']) {
            $error = '部署失败：' . $deploy['error'];
        } elseif ($deploy['changed']) {
            go('已部署站点文件：' . implode('、', $deploy['changed']));
        } else {
            go('无需更新，站点文件已经是最新。');
        }
    }
}

$loggedIn = !empty($_SESSION['update_admin']);
$csrf = (string)($_SESSION['update_csrf'] ?? '');

// 站点首页当前展示的版本（version.txt 首行），以及上次同步时间。
$siteVersion = '';
$siteVersionFile = dirname(__DIR__) . '/version.txt';
$rawVersion = @file_get_contents($siteVersionFile);
if (is_string($rawVersion)) {
    $versionLines = preg_split('/\r?\n/', $rawVersion);
    $siteVersion = trim((string)($versionLines[0] ?? ''));
}
$siteSyncTime = '';
$syncLogFile = __DIR__ . '/sync-version.log';
if (is_readable($syncLogFile)) {
    $logLines = preg_split('/\r?\n/', (string)file_get_contents($syncLogFile));
    for ($i = count($logLines) - 1; $i >= 0; $i--) {
        if (trim($logLines[$i]) !== '') { $siteSyncTime = trim($logLines[$i]); break; }
    }
}

// 站点静态文件的部署状态（白名单来自 sync-site-lib.php）。
$siteDeployLog = '';
$siteDeployFiles = array();
$siteDeployLib = __DIR__ . '/sync-site-lib.php';
if (is_file($siteDeployLib)) {
    require_once $siteDeployLib;
    $siteDeployLog = dzjs_site_last_log();
    $siteDeployFiles = dzjs_site_whitelist();
}
header('Content-Type: text/html; charset=utf-8');
?>
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DzjsTrainer 更新后台</title>
<style>
:root{color-scheme:dark;font-family:Segoe UI,Microsoft YaHei,sans-serif;background:#10141b;color:#e8edf5}body{margin:0;min-height:100vh;background:radial-gradient(circle at 20% 0,#1f3147 0,#10141b 48%);padding:32px 16px;box-sizing:border-box}.panel{width:min(760px,100%);margin:auto;background:#18202b;border:1px solid #2d3b4e;border-radius:14px;box-shadow:0 20px 60px #0007;padding:28px;box-sizing:border-box}h1{font-size:24px;margin:0 0 8px}.muted{color:#9eabbc;font-size:14px}.section{border-top:1px solid #2d3b4e;margin-top:22px;padding-top:22px}label{display:block;font-size:14px;color:#b9c6d7;margin:14px 0 7px}input,textarea{width:100%;box-sizing:border-box;border:1px solid #3a4b61;background:#101720;color:#eef4ff;border-radius:8px;padding:11px 12px;font:inherit}textarea{min-height:110px;resize:vertical}button{border:0;border-radius:8px;padding:11px 16px;background:#3c8cff;color:#fff;font-weight:600;cursor:pointer}button:hover{background:#5a9dff}.danger{background:#394555;float:right}.notice{padding:11px 13px;border-radius:8px;background:#173b2b;color:#9ef0be;margin:16px 0}.error{background:#4a2429;color:#ffb4b8}.facts{display:grid;grid-template-columns:120px 1fr;gap:8px 14px;font-size:14px;word-break:break-all}.facts b{color:#9eabbc;font-weight:500}code{color:#c4d7ff}small{color:#8e9caf}
</style></head><body><main class="panel"><h1>DzjsTrainer 更新后台</h1>
<?php if (!$loggedIn): ?><p class="muted">登录后上传新的更新器并生成客户端清单。</p><?php if ($error !== ''): ?><div class="notice error"><?= esc($error) ?></div><?php endif; ?><form method="post"><input type="hidden" name="admin_action" value="login"><label for="password">管理员密码</label><input id="password" name="password" type="password" autocomplete="current-password" required><p><button type="submit">登录</button></p></form>
<?php else: ?><?php if ($message !== ''): ?><div class="notice"><?= esc($message) ?></div><?php endif; ?><?php if ($error !== ''): ?><div class="notice error"><?= esc($error) ?></div><?php endif; ?><form method="post" enctype="multipart/form-data"><input type="hidden" name="admin_action" value="upload"><input type="hidden" name="csrf" value="<?= esc($csrf) ?>"><label for="version">版本号</label><input id="version" name="version" value="<?= esc((string)$manifest['version']) ?>" placeholder="例如 1.0.4" required><label for="notes">更新说明</label><textarea id="notes" name="notes" placeholder="写给用户看的更新内容"><?= esc((string)$manifest['notes']) ?></textarea><label for="package">更新器 EXE</label><input id="package" name="package" type="file" accept=".exe,application/vnd.microsoft.portable-executable" required><p><button type="submit">上传并发布</button><button class="danger" type="submit" formnovalidate name="admin_action" value="logout">退出</button></p></form><section class="section"><div class="muted">当前发布</div><div class="facts"><b>版本</b><span><?= esc((string)$manifest['version']) ?></span><b>下载地址</b><span><code><?= esc((string)$manifest['url'] ?: '尚未发布') ?></code></span><b>SHA-256</b><span><code><?= esc((string)$manifest['sha256'] ?: '尚未发布') ?></code></span></div><p><small>新地址格式：随机值.<?= esc($downloadDomain) ?>，协议：<?= esc($downloadScheme) ?></small></p></section><section class="section"><div class="muted">站点更新日志（来源：GitHub Releases）</div><div class="facts"><b>首页显示</b><span><?= esc($siteVersion !== '' ? $siteVersion : '尚未生成') ?></span><b>同步方式</b><span>每小时自动 · 有变化才写入</span><b>上次同步</b><span><code><?= esc($siteSyncTime !== '' ? $siteSyncTime : '尚未同步') ?></code></span></div><form method="post"><input type="hidden" name="admin_action" value="syncversion"><input type="hidden" name="csrf" value="<?= esc($csrf) ?>"><p><button type="submit">立即同步 GitHub 更新</button></p></form></section><section class="section"><div class="muted">站点静态文件（来源：GitHub website/）</div><div class="facts"><b>会部署</b><span><code><?= esc(implode(' · ', $siteDeployFiles)) ?></code></span><b>不部署</b><span><code>index.html · styles.css · version.txt</code></span><b>上次部署</b><span><code><?= esc($siteDeployLog !== '' ? $siteDeployLog : '尚未部署') ?></code></span></div><form method="post"><input type="hidden" name="admin_action" value="syncsite"><input type="hidden" name="csrf" value="<?= esc($csrf) ?>"><p><button type="submit">立即部署站点文件</button></p></form><p><small>首页与样式是你自己维护的文案版本，这里刻意不覆盖；要改它们请直接改服务器上的文件。</small></p></section><?php endif; ?></main></body></html>
