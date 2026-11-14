<?php
// 命令行入口：把 GitHub Releases 同步成站点根的 version.txt。
//
//   php sync-version.php               正常同步（内容没变就不动文件）
//   php sync-version.php --dry-run     只打印将要写入的内容
//   php sync-version.php --force       内容相同也重写一次
//   php sync-version.php --repo=owner/name --target=/path/version.txt
//
// 只允许命令行运行：这个脚本会覆盖站点首页数据，不能挂到公网 URL 上。
// 后台按钮不调这里，而是直接 include sync-version-lib.php（PHP 的 exec 被禁用）。

if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "CLI only\n";
    exit(1);
}

require __DIR__ . '/sync-version-lib.php';

$options = getopt('', array('dry-run', 'force', 'repo:', 'target:', 'help'));
if (isset($options['help'])) {
    fwrite(STDOUT, "用法：php sync-version.php [--dry-run] [--force] [--repo=owner/name] [--target=/path/version.txt]\n");
    exit(0);
}

$repo = isset($options['repo']) ? (string)$options['repo'] : DZJS_SYNC_REPO;
$target = isset($options['target']) ? (string)$options['target'] : dirname(__DIR__) . '/version.txt';
$dryRun = isset($options['dry-run']);
$force = isset($options['force']);

$result = dzjs_sync_run($target, $force, $dryRun, $repo);

if ($dryRun && $result['ok']) {
    fwrite(STDOUT, $result['text']);
    fwrite(STDERR, "\n--- 以上为预览，未写入 " . $target . " ---\n");
    exit(0);
}

if (!$result['ok']) {
    fwrite(STDERR, '同步失败：' . $result['error'] . "\n");
    exit(2);
}

if ($result['changed']) {
    fwrite(STDOUT, '已更新 ' . $target . ' → ' . $result['version'] . "\n");
} else {
    fwrite(STDOUT, '无需更新，当前已是 ' . $result['version'] . "\n");
}
exit(0);
