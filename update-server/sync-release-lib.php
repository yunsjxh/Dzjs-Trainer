<?php
// 把最新 GitHub Release 里的主程序同步到站点根的 Release/DzjsTrainer.exe。
//
// 首页「直接写入文件夹」读的就是这个文件。它不进仓库（每版 1.3 MB 的二进制会把
// 历史撑爆），也不在 sync-site 的白名单里，所以单独走一条同步：从 release 资产里
// 取裸 exe；没有裸 exe 就从发布 zip 里解出来，解之前先核对 zip 的 SHA-256。
//
// 站点上的文件名固定，内容随版本覆盖；写之前备份到 .DzjsTrainer.exe.bak，
// 落盘用临时文件 + rename，避免下载者拿到半截程序。
//
// 每小时第 37 分由 cron 跑一次，后台也有「立即同步发布包」按钮。

if (!defined('DZJS_LIB_CONTEXT')) {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "Forbidden\n";
    exit(1);
}

// 复用 release 列表的拉取与凭证处理，和 version.txt 用同一套。
require_once __DIR__ . '/sync-version-lib.php';

if (!defined('DZJS_RELEASE_REPO')) define('DZJS_RELEASE_REPO', DZJS_SYNC_REPO);
if (!defined('DZJS_RELEASE_TARGET')) define('DZJS_RELEASE_TARGET', dirname(__DIR__) . '/Release/DzjsTrainer.exe');
if (!defined('DZJS_RELEASE_EXE_NAME')) define('DZJS_RELEASE_EXE_NAME', 'DzjsTrainer.exe');
// 太小的一定不是主程序（防止把错误页、空文件当成发布会写上线）。
if (!defined('DZJS_RELEASE_MIN_BYTES')) define('DZJS_RELEASE_MIN_BYTES', 300 * 1024);

function dzjs_release_log(string $message): void
{
    $line = '[' . date('Y-m-d H:i:s') . '] ' . $message . "\n";
    @file_put_contents(__DIR__ . '/sync-release.log', $line, FILE_APPEND | LOCK_EX);
}

function dzjs_release_state_file(): string
{
    return __DIR__ . '/.release-asset.state';
}

function dzjs_release_state(): array
{
    $file = dzjs_release_state_file();
    if (!is_readable($file)) return array();
    $data = json_decode((string)file_get_contents($file), true);
    return is_array($data) ? $data : array();
}

function dzjs_release_save_state(array $state): void
{
    @file_put_contents(
        dzjs_release_state_file(),
        json_encode($state, JSON_UNESCAPED_SLASHES) . "\n",
        LOCK_EX
    );
}

// 下载到指定文件，返回字节数。
//
// 刻意不带 Authorization：资产的下载地址是带签名的临时 URL，多一个 Authorization
// 头会被对象存储拒掉。token 只在调 api.github.com 时用。
function dzjs_release_download(string $url, string $dest): int
{
    $fp = @fopen($dest, 'wb');
    if ($fp === false) throw new RuntimeException('无法写入 ' . basename($dest));

    $ch = curl_init($url);
    curl_setopt_array($ch, array(
        CURLOPT_FILE => $fp,
        CURLOPT_FOLLOWLOCATION => true,
        CURLOPT_MAXREDIRS => 5,
        CURLOPT_CONNECTTIMEOUT => 15,
        CURLOPT_TIMEOUT => 180,
        CURLOPT_HTTPHEADER => array(
            'Accept: application/octet-stream',
            'User-Agent: DzjsTrainer-SiteSync',
        ),
    ));
    $ok = curl_exec($ch);
    $status = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $error = curl_error($ch);
    curl_close($ch);
    fclose($fp);

    if ($ok === false || $error !== '') {
        @unlink($dest);
        throw new RuntimeException('下载失败：' . ($error !== '' ? $error : '无响应'));
    }
    if ($status !== 200) {
        @unlink($dest);
        throw new RuntimeException('下载返回 HTTP ' . $status);
    }
    $size = (int)@filesize($dest);
    if ($size <= 0) {
        @unlink($dest);
        throw new RuntimeException('下载到的文件是空的');
    }
    return $size;
}

// 在 release 的资产里挑出主程序、发布 zip 与校验文件。
function dzjs_release_assets(array $release): array
{
    $assets = isset($release['assets']) && is_array($release['assets']) ? $release['assets'] : array();
    $exe = null;
    $zip = null;
    $sha = null;
    foreach ($assets as $asset) {
        $name = isset($asset['name']) ? (string)$asset['name'] : '';
        if ($name === '') continue;
        if ($name === DZJS_RELEASE_EXE_NAME) {
            $exe = $asset;
        } elseif (preg_match('/\.zip$/i', $name)) {
            $zip = $asset;
        } elseif (preg_match('/\.sha256$/i', $name)) {
            $sha = $asset;
        }
    }
    return array($exe, $zip, $sha);
}

// 从 zip 里取出主程序写进 $dest。只认第一个 .exe，发布包里本来也只有一个。
function dzjs_release_unzip(string $zipPath, string $dest): void
{
    if (!class_exists('ZipArchive')) throw new RuntimeException('服务器 PHP 缺少 zip 扩展');

    $zip = new ZipArchive();
    if ($zip->open($zipPath) !== true) throw new RuntimeException('无法打开下载到的 zip');

    $index = $zip->locateName(DZJS_RELEASE_EXE_NAME);
    if ($index === false) {
        for ($i = 0; $i < $zip->numFiles; $i++) {
            $name = (string)$zip->getNameIndex($i);
            if (preg_match('/\.exe$/i', $name)) {
                $index = $i;
                break;
            }
        }
    }
    if ($index === false) {
        $zip->close();
        throw new RuntimeException('zip 里没有可执行文件');
    }

    $data = $zip->getFromIndex($index);
    $zip->close();
    if ($data === false || $data === '') throw new RuntimeException('读取 zip 内的主程序失败');
    if (file_put_contents($dest, $data, LOCK_EX) === false) throw new RuntimeException('写入临时文件失败');
}

// 落盘前的最后一道门：确实是 PE，且大小像话。
function dzjs_release_verify(string $path): int
{
    $size = (int)@filesize($path);
    if ($size < DZJS_RELEASE_MIN_BYTES) {
        throw new RuntimeException('产物只有 ' . $size . ' 字节，不像主程序');
    }
    $fp = @fopen($path, 'rb');
    if ($fp === false) throw new RuntimeException('无法读取产物');
    $magic = (string)fread($fp, 2);
    fclose($fp);
    if ($magic !== 'MZ') throw new RuntimeException('产物不是 PE 文件');
    return $size;
}

/**
 * 完整流程：取最新正式 release → 挑资产 → 与上次记录比对 → 下载 → 校验 → 原子替换。
 * 返回 array(ok, changed, version, bytes, sha256, skipped, note, error)。
 */
function dzjs_release_sync(bool $force = false, bool $dryRun = false, string $repo = DZJS_RELEASE_REPO): array
{
    $result = array(
        'ok' => false,
        'changed' => false,
        'version' => '',
        'bytes' => 0,
        'sha256' => '',
        'skipped' => false,
        'note' => '',
        'error' => '',
    );

    $tmpZip = '';
    $tmpExe = '';

    try {
        $releases = dzjs_sync_fetch_releases($repo);

        $release = null;
        foreach ($releases as $item) {
            if (!empty($item['draft'])) continue;
            if (!empty($item['prerelease'])) continue;
            $release = $item;
            break;
        }
        if ($release === null) throw new RuntimeException('没有找到正式 release');

        $tag = isset($release['tag_name']) ? (string)$release['tag_name'] : '';
        $result['version'] = dzjs_sync_normalize_tag($tag);

        list($exeAsset, $zipAsset, $shaAsset) = dzjs_release_assets($release);
        if ($exeAsset === null && $zipAsset === null) {
            throw new RuntimeException('release ' . $tag . ' 没有主程序，也没有发布 zip');
        }

        $target = DZJS_RELEASE_TARGET;
        $targetSize = is_file($target) ? (int)filesize($target) : 0;
        $state = dzjs_release_state();

        // 发布 zip 的 SHA-256 每次都会变（顶替重发也一样），拿它判断有没有新东西。
        // 校验文件只有百来字节，先拉它比拼目标文件更省。
        $expected = '';
        if ($shaAsset !== null) {
            $tmpSha = tempnam(sys_get_temp_dir(), 'dzjs-sha-');
            try {
                dzjs_release_download((string)$shaAsset['browser_download_url'], $tmpSha);
                $raw = (string)file_get_contents($tmpSha);
                if (preg_match('/([0-9a-fA-F]{64})/', $raw, $m)) $expected = strtolower($m[1]);
            } finally {
                if (is_file($tmpSha)) @unlink($tmpSha);
            }
        }

        // 跳过要同时满足两条：release 资产没变过，且线上文件还是上次写下的那一份。
        // 只比资产会漏掉「文件被换掉/截断」的情况——那种时候必须重新下载覆盖。
        if (!$force && !$dryRun && $targetSize > 0) {
            $assetUnchanged = ($expected !== '' && $expected === (string)($state['asset_sha256'] ?? ''))
                || ($expected === '' && $tag === (string)($state['tag'] ?? ''));
            $targetIntact = ($targetSize >= DZJS_RELEASE_MIN_BYTES
                && $targetSize === (int)($state['size'] ?? 0));
            if ($assetUnchanged && $targetIntact) {
                $result['ok'] = true;
                $result['skipped'] = true;
                $result['bytes'] = $targetSize;
                $result['sha256'] = (string)($state['exe_sha256'] ?? '');
                $result['note'] = '已是 ' . $result['version'];
                dzjs_release_log('无需更新，当前已是 ' . $result['version'] . '（' . $targetSize . ' 字节）');
                return $result;
            }
        }

        $dir = dirname($target);
        if (!is_dir($dir) && !@mkdir($dir, 0755, true)) throw new RuntimeException('无法创建 ' . $dir);

        // 优先裸 exe，没有就把 zip 拉下来解。
        if ($exeAsset !== null) {
            $tmpExe = tempnam($dir, '.release-exe-');
            dzjs_release_download((string)$exeAsset['browser_download_url'], $tmpExe);
            $assetSha = '';
        } else {
            $tmpZip = tempnam($dir, '.release-zip-');
            dzjs_release_download((string)$zipAsset['browser_download_url'], $tmpZip);
            $assetSha = hash_file('sha256', $tmpZip);
            if ($expected !== '' && $assetSha !== $expected) {
                throw new RuntimeException('zip 校验不通过：期望 ' . substr($expected, 0, 16) . '，实际 ' . substr((string)$assetSha, 0, 16));
            }
            $tmpExe = tempnam($dir, '.release-exe-');
            dzjs_release_unzip($tmpZip, $tmpExe);
        }

        $size = dzjs_release_verify($tmpExe);
        $exeSha = hash_file('sha256', $tmpExe);

        if ($size === $targetSize && $exeSha === (string)($state['exe_sha256'] ?? '')) {
            @unlink($tmpExe);
            $tmpExe = '';
            $result['ok'] = true;
            $result['skipped'] = true;
            $result['bytes'] = $size;
            $result['sha256'] = $exeSha;
            $result['note'] = '内容未变化';
            dzjs_release_log('无需更新，内容与 ' . $result['version'] . ' 一致');
            return $result;
        }

        if ($dryRun) {
            @unlink($tmpExe);
            $tmpExe = '';
            $result['ok'] = true;
            $result['changed'] = true;
            $result['bytes'] = $size;
            $result['sha256'] = $exeSha;
            $result['note'] = '预览，未写入';
            return $result;
        }

        @chmod($tmpExe, 0644);
        if (is_file($target)) {
            // 备份带点前缀：同目录下的文件可被当成静态资源下载，没必要多一个入口。
            @copy($target, $dir . '/.DzjsTrainer.exe.bak');
        }
        if (!@rename($tmpExe, $target)) throw new RuntimeException('替换 ' . basename($target) . ' 失败');
        $tmpExe = '';
        @chmod($target, 0644);

        dzjs_release_save_state(array(
            'tag' => $tag,
            'version' => $result['version'],
            'size' => $size,
            'asset_sha256' => $assetSha,
            'exe_sha256' => $exeSha,
            'time' => date('c'),
        ));

        $result['ok'] = true;
        $result['changed'] = true;
        $result['bytes'] = $size;
        $result['sha256'] = $exeSha;
        $result['note'] = '已更新到 ' . $result['version'];
        dzjs_release_log('已更新到 ' . $result['version'] . '（' . $size . ' 字节，sha256 ' . substr((string)$exeSha, 0, 16) . '）');
        return $result;
    } catch (Throwable $e) {
        $result['error'] = $e->getMessage();
        dzjs_release_log('同步失败：' . $e->getMessage());
        return $result;
    } finally {
        if ($tmpZip !== '' && is_file($tmpZip)) @unlink($tmpZip);
        if ($tmpExe !== '' && is_file($tmpExe)) @unlink($tmpExe);
    }
}
