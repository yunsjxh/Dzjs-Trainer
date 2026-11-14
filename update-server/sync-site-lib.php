<?php
// 把 GitHub 仓库 website/ 目录同步到站点根。
//
// 只同步「没有人手工改过」的文件。首页 index.html 与 styles.css 是维护者自己
// 维护的文案版本，跟仓库里面向开源/合规的措辞不是同一份，**必须排除** ——
// 否则每次同步都会把站点上的文案冲掉。version.txt 也不在这里，它由
// sync-version.php 从 Releases 生成。
//
// 判断某个文件要不要写：本地内容算出的 git blob sha 与 tree 里的一致就跳过，
// 所以不需要额外的状态文件，反复执行天然幂等。

// 只能被 CLI 入口或后台页面 include，不能当 URL 直接访问。
// 直接打这个路径会执行到函数定义就结束、返回 200 空响应，看起来像"什么都没有"，
// 但那仍是一个不该存在的入口，显式 403 更清楚。
if (!defined('DZJS_LIB_CONTEXT')) {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "Forbidden\n";
    exit(1);
}

if (!defined('DZJS_SITE_REPO')) define('DZJS_SITE_REPO', 'yunsjxh/Dzjs-Trainer');
if (!defined('DZJS_SITE_BRANCH')) define('DZJS_SITE_BRANCH', 'main');
if (!defined('DZJS_SITE_PREFIX')) define('DZJS_SITE_PREFIX', 'website/');
if (!defined('DZJS_SITE_BACKUPS_KEEP')) define('DZJS_SITE_BACKUPS_KEEP', 5);

// 白名单：只有这些路径允许被 GitHub 覆盖。
// 刻意「默认拒绝」而不是「黑名单排除」——将来仓库里新增的文件不会自动上线，
// 想让它上线必须在这里明确加一条。
function dzjs_site_allowed(string $rel): bool
{
    if ($rel === '' || $rel[0] === '/' || strpos($rel, '..') !== false) return false;
    if (strpos($rel, '\\') !== false) return false;

    // 明确排除：首页、样式、版本清单。写在这里是为了让规则一眼可见，
    // 即使有人误把它们加进下面的列表也会被挡住。
    if (in_array($rel, array('index.html', 'styles.css', 'version.txt'), true)) return false;

    if (in_array($rel, array('script.js', '404.html', 'download/index.php'), true)) return true;

    // 站点图片：只放行静态图片扩展名。
    if (strpos($rel, 'assets/') === 0 && preg_match('/\.(png|jpe?g|webp|gif|svg|ico)$/i', $rel)) {
        return true;
    }
    return false;
}

function dzjs_site_root(): string
{
    return dirname(__DIR__);
}

// git 的 blob 哈希：sha1("blob <字节数>\0" + 内容)。
// 用它跟 tree 里的 sha 比对，比记一份状态文件更可靠——状态文件丢了也只是重算一次。
function dzjs_site_blob_sha(string $content): string
{
    return sha1('blob ' . strlen($content) . "\0" . $content);
}

function dzjs_site_token(): string
{
    $tokenFile = __DIR__ . '/github-token.php';
    if (is_readable($tokenFile)) {
        $loaded = include $tokenFile;
        if (is_string($loaded) && trim($loaded) !== '') return trim($loaded);
    }
    $env = getenv('GITHUB_TOKEN');
    return $env ? trim((string)$env) : '';
}

function dzjs_site_api(string $path, string $accept = 'application/vnd.github+json'): array
{
    $headers = array(
        'Accept: ' . $accept,
        'User-Agent: DzjsTrainer-SiteDeploy',
        'X-GitHub-Api-Version: 2022-11-28',
    );
    $token = dzjs_site_token();
    if ($token !== '') $headers[] = 'Authorization: Bearer ' . $token;

    $ch = curl_init('https://api.github.com' . $path);
    curl_setopt_array($ch, array(
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_HTTPHEADER => $headers,
        CURLOPT_CONNECTTIMEOUT => 10,
        CURLOPT_TIMEOUT => 30,
        CURLOPT_FOLLOWLOCATION => true,
        CURLOPT_MAXREDIRS => 3,
    ));
    $body = curl_exec($ch);
    $status = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $error = curl_error($ch);
    curl_close($ch);

    if ($body === false || $error !== '') {
        throw new RuntimeException('请求 GitHub 失败：' . ($error !== '' ? $error : '无响应'));
    }
    if ($status !== 200) {
        throw new RuntimeException('GitHub 返回 HTTP ' . $status . '：' . substr((string)$body, 0, 200));
    }
    $data = json_decode((string)$body, true);
    if (!is_array($data)) throw new RuntimeException('GitHub 返回的不是有效 JSON');
    return $data;
}

// 仓库 website/ 下所有 blob：[相对路径 => array(sha, size)]。
function dzjs_site_remote_files(string $repo = DZJS_SITE_REPO): array
{
    $tree = dzjs_site_api('/repos/' . $repo . '/git/trees/' . DZJS_SITE_BRANCH . '?recursive=1');
    if (empty($tree['tree']) || !is_array($tree['tree'])) {
        throw new RuntimeException('tree 响应里没有文件列表');
    }

    $out = array();
    foreach ($tree['tree'] as $node) {
        if (($node['type'] ?? '') !== 'blob') continue;
        $path = (string)($node['path'] ?? '');
        if (strpos($path, DZJS_SITE_PREFIX) !== 0) continue;

        $rel = substr($path, strlen(DZJS_SITE_PREFIX));
        if (!dzjs_site_allowed($rel)) continue;

        $out[$rel] = array(
            'sha' => (string)($node['sha'] ?? ''),
            'size' => (int)($node['size'] ?? 0),
        );
    }
    ksort($out);
    return $out;
}

function dzjs_site_fetch_blob(string $sha, string $repo = DZJS_SITE_REPO): string
{
    $blob = dzjs_site_api('/repos/' . $repo . '/git/blobs/' . $sha);
    $encoding = (string)($blob['encoding'] ?? '');
    $content = (string)($blob['content'] ?? '');
    if ($encoding === 'base64') {
        $decoded = base64_decode(str_replace(array("\n", "\r"), '', $content), true);
        if ($decoded === false) throw new RuntimeException('blob ' . substr($sha, 0, 8) . ' 解码失败');
        return $decoded;
    }
    if ($encoding === 'utf-8' || $encoding === '') return $content;
    throw new RuntimeException('不支持的 blob 编码：' . $encoding);
}

function dzjs_site_write(string $path, string $content): void
{
    $dir = dirname($path);
    if (!is_dir($dir) && !@mkdir($dir, 0755, true) && !is_dir($dir)) {
        throw new RuntimeException('无法创建目录 ' . $dir);
    }
    $tmp = tempnam($dir, '.site-');
    if ($tmp === false) throw new RuntimeException('无法在 ' . $dir . ' 创建临时文件');
    if (file_put_contents($tmp, $content, LOCK_EX) === false) {
        @unlink($tmp);
        throw new RuntimeException('写入临时文件失败');
    }
    @chmod($tmp, 0644);
    if (!rename($tmp, $path)) {
        @unlink($tmp);
        throw new RuntimeException('替换 ' . basename($path) . ' 失败');
    }
    @chmod($path, 0644);
}

// 覆盖前把旧文件留一份：按时间戳分目录，只保留最近几次。
function dzjs_site_backup(string $relative, string $content): string
{
    $stamp = date('Ymd-His');
    $dir = __DIR__ . '/.site-backup/' . $stamp;
    $target = $dir . '/' . str_replace('/', DIRECTORY_SEPARATOR, $relative);
    if (!is_dir(dirname($target))) @mkdir(dirname($target), 0755, true);

    if (file_put_contents($target, $content, LOCK_EX) === false) {
        throw new RuntimeException('备份 ' . $relative . ' 失败');
    }

    $root = __DIR__ . '/.site-backup';
    $dirs = array();
    foreach ((array)@scandir($root) as $entry) {
        if ($entry === '.' || $entry === '..') continue;
        if (is_dir($root . '/' . $entry)) $dirs[] = $entry;
    }
    sort($dirs);
    while (count($dirs) > DZJS_SITE_BACKUPS_KEEP) {
        $old = array_shift($dirs);
        $it = new RecursiveIteratorIterator(
            new RecursiveDirectoryIterator($root . '/' . $old, FilesystemIterator::SKIP_DOTS),
            RecursiveIteratorIterator::CHILD_FIRST
        );
        foreach ($it as $item) {
            $item->isDir() ? @rmdir($item->getPathname()) : @unlink($item->getPathname());
        }
        @rmdir($root . '/' . $old);
    }
    return $target;
}

function dzjs_site_is_text(string $rel): bool
{
    return (bool)preg_match('/\.(php|js|mjs|html?|css|txt|json|svg)$/i', $rel);
}

// 只统一换行，别的字节不动。
function dzjs_site_normalize(string $text): string
{
    return str_replace(array("\r\n", "\r"), "\n", $text);
}

function dzjs_site_log(string $message): void
{
    @file_put_contents(
        __DIR__ . '/sync-site.log',
        '[' . date('Y-m-d H:i:s') . '] ' . $message . "\n",
        FILE_APPEND | LOCK_EX
    );
}

/**
 * 拉取并部署白名单文件。
 * 返回 array(ok, error, changed[], skipped[], removed[], dry).
 */
function dzjs_site_run(bool $dryRun = false, bool $force = false, string $repo = DZJS_SITE_REPO): array
{
    $result = array('ok' => false, 'error' => '', 'changed' => array(), 'skipped' => array(), 'dry' => $dryRun);
    try {
        $remote = dzjs_site_remote_files($repo);
        if (!$remote) throw new RuntimeException('白名单里没有匹配到任何文件（检查 tree 与白名单）');

        $root = dzjs_site_root();
        foreach ($remote as $rel => $info) {
            $target = $root . '/' . $rel;
            $current = is_file($target) ? (string)file_get_contents($target) : null;

            if (!$force && $current !== null && dzjs_site_blob_sha($current) === $info['sha']) {
                $result['skipped'][] = $rel;
                continue;
            }

            $content = dzjs_site_fetch_blob($info['sha'], $repo);
            // blob 接口给的内容必须与 tree 记的哈希对得上，否则宁可失败也不落盘。
            if (dzjs_site_blob_sha($content) !== $info['sha']) {
                throw new RuntimeException('内容哈希与 tree 不符：' . $rel);
            }

            if (!$force && $current !== null) {
                if ($current === $content) {
                    $result['skipped'][] = $rel;
                    continue;
                }
                // 只差换行（服务器上有些文件是 CRLF，仓库里是 LF）：PHP/JS 执行
                // 没有区别，不值得为它改一个本来跑得好好的文件。
                if (dzjs_site_is_text($rel) && dzjs_site_normalize($current) === dzjs_site_normalize($content)) {
                    $result['skipped'][] = $rel;
                    continue;
                }
            }

            if ($dryRun) {
                $result['changed'][] = $rel;
                continue;
            }

            if ($current !== null) dzjs_site_backup($rel, $current);
            dzjs_site_write($target, $content);
            $result['changed'][] = $rel;
        }

        if ($result['changed'] && !$dryRun) {
            dzjs_site_log(
                '部署完成：更新 ' . count($result['changed']) . ' 个文件（' . implode(', ', $result['changed']) . '），'
                . '跳过 ' . count($result['skipped']) . ' 个'
            );
        }
        $result['ok'] = true;
        return $result;
    } catch (Throwable $e) {
        $result['error'] = $e->getMessage();
        dzjs_site_log('部署失败：' . $e->getMessage());
        return $result;
    }
}

// 后台展示用：白名单当前会同步哪些路径。
function dzjs_site_whitelist(): array
{
    return array('script.js', '404.html', 'download/index.php', 'assets/*.png|jpg|webp|gif|svg|ico');
}

// 后台展示用：日志最后一条。
function dzjs_site_last_log(): string
{
    $file = __DIR__ . '/sync-site.log';
    if (!is_readable($file)) return '';
    $lines = preg_split('/\r?\n/', (string)file_get_contents($file));
    for ($i = count($lines) - 1; $i >= 0; $i--) {
        if (trim($lines[$i]) !== '') return trim($lines[$i]);
    }
    return '';
}
