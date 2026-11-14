<?php
// 命令行入口：把 GitHub 仓库 website/ 下的白名单文件部署到站点根。
//
//   php sync-site.php              正常部署（内容没变就不动文件）
//   php sync-site.php --dry-run    只列出会更新哪些文件
//   php sync-site.php --force      内容相同也重写一次
//   php sync-site.php --repo=owner/name
//
// 只允许命令行运行：它会写站点根目录，不能挂到公网 URL 上。
// 后台按钮不调这里，而是直接 include sync-site-lib.php（PHP 的 exec 被禁用）。
//
// 注意：index.html / styles.css / version.txt 不在白名单里，永远不会被这里覆盖。

if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    header('Content-Type: text/plain; charset=utf-8');
    echo "CLI only\n";
    exit(1);
}

define('DZJS_LIB_CONTEXT', 1);
require __DIR__ . '/sync-site-lib.php';

$options = getopt('', array('dry-run', 'force', 'repo:', 'help'));
if (isset($options['help'])) {
    fwrite(STDOUT, "用法：php sync-site.php [--dry-run] [--force] [--repo=owner/name]\n");
    exit(0);
}

$repo = isset($options['repo']) ? (string)$options['repo'] : DZJS_SITE_REPO;
$dryRun = isset($options['dry-run']);
$force = isset($options['force']);

$result = dzjs_site_run($dryRun, $force, $repo);

if (!$result['ok']) {
    fwrite(STDERR, '部署失败：' . $result['error'] . "\n");
    exit(2);
}

if ($dryRun) {
    fwrite(STDOUT, "将更新 " . count($result['changed']) . " 个文件：\n");
    foreach ($result['changed'] as $rel) fwrite(STDOUT, '  ' . $rel . "\n");
    fwrite(STDERR, "\n--- 以上为预览，未写入任何文件 ---\n");
    exit(0);
}

if ($result['changed']) {
    fwrite(STDOUT, '已更新 ' . count($result['changed']) . " 个文件：\n");
    foreach ($result['changed'] as $rel) fwrite(STDOUT, '  ' . $rel . "\n");
} else {
    fwrite(STDOUT, '无需更新，' . count($result['skipped']) . " 个文件都已是仓库里的版本\n");
}
exit(0);
