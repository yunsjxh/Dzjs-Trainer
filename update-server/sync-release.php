<?php
// 命令行入口：把最新 GitHub Release 的主程序同步到站点根的 Release/DzjsTrainer.exe。
//
//   php sync-release.php               正常同步（内容没变就不下载）
//   php sync-release.php --dry-run     只下载并校验，不替换线上文件
//   php sync-release.php --force       忽略上次记录，强制重新下载
//   php sync-release.php --repo=owner/name --target=/path/DzjsTrainer.exe
//
// 只允许命令行运行：这个脚本会覆盖站点上对外分发的程序。
// 后台按钮不调这里，而是直接 include sync-release-lib.php（PHP 的 exec 被禁用）。

if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "CLI only\n";
    exit(1);
}

$options = getopt('', array('dry-run', 'force', 'repo:', 'target:', 'help'));
if (isset($options['help'])) {
    fwrite(STDOUT, "用法：php sync-release.php [--dry-run] [--force] [--repo=owner/name] [--target=/path/DzjsTrainer.exe]\n");
    exit(0);
}

define('DZJS_LIB_CONTEXT', 1);
if (isset($options['target'])) {
    define('DZJS_RELEASE_TARGET', (string)$options['target']);
}
require __DIR__ . '/sync-release-lib.php';

$repo = isset($options['repo']) ? (string)$options['repo'] : DZJS_RELEASE_REPO;
$dryRun = isset($options['dry-run']);
$force = isset($options['force']);

$result = dzjs_release_sync($force, $dryRun, $repo);

if (!$result['ok']) {
    fwrite(STDERR, '同步失败：' . $result['error'] . "\n");
    exit(2);
}

$line = $result['changed']
    ? sprintf('已更新到 %s（%d 字节，sha256 %s）', $result['version'], $result['bytes'], $result['sha256'] !== '' ? substr($result['sha256'], 0, 16) : '-')
    : sprintf('无需更新，当前已是 %s（%d 字节）', $result['version'], $result['bytes']);
fwrite(STDOUT, $line . ($dryRun ? "（预览，未写入）\n" : "\n"));
exit(0);
