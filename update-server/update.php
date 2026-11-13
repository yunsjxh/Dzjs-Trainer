<?php
declare(strict_types=1);

// Place this file beside manifest.json and expose the releases directory.
$root = __DIR__;

// 时效票据支持。文件不在也不致命（纯文本接口还能用），但清单接口会明确报错，
// 而不是悄悄退化成"谁都能取清单"。
$ticketLibrary = $root . DIRECTORY_SEPARATOR . 'update-ticket.php';
if (is_file($ticketLibrary)) {
    require $ticketLibrary;
}

$manifestPath = $root . DIRECTORY_SEPARATOR . 'manifest.json';
$admin = isset($_GET['admin']);
if ($admin) {
    header('Location: admin.php');
    exit;
}
$manifest = [
    'version' => '1.0.2',
    'notes' => 'No release notes supplied.',
    'url' => 'releases/DzjsTrainerUpdater.exe',
    'sha256' => '',
];
if (is_file($manifestPath)) {
    $raw = (string)file_get_contents($manifestPath);
    // PowerShell 的 Out-File / Set-Content 默认写 UTF-8 BOM，而 json_decode 不认
    // BOM，遇到就返回 null。原代码用 is_array 兜住了，于是"改了 manifest.json
    // 但客户端一直看到默认值"，且没有任何报错。这里顺手剥掉。
    if (strncmp($raw, "\xEF\xBB\xBF", 3) === 0) {
        $raw = substr($raw, 3);
    }
    $decoded = json_decode($raw, true);
    if (is_array($decoded)) {
        $manifest = array_merge($manifest, $decoded);
    }
}

header('Cache-Control: no-store, no-cache, must-revalidate');
$action = (string)($_GET['action'] ?? '');
if (isset($_GET['manifest']) || $action === 'manifest') {
    // 清单决定"下哪个包"和"这个包应该是什么哈希"，是整条更新链的根，
    // 所以它要求时效票据：先验请求格式，再回一张签名覆盖正文的票据。
    //
    // 早期的 ?checkupdate / ?getupdateinfo / ?getnewver / ?getupdate 四个纯文本
    // 接口已删除。它们不验任何凭证就交出"有没有新版"和下载地址，等于给票据校验
    // 开了一道后门：谁都能轮询版本、直接拿包地址，票据签得再严也没用。
    // 客户端自 1.0.3 起不再调用它们，删除不影响任何在用的版本。
    if (!function_exists('dzjs_check_challenge')) {
        header('Content-Type: text/plain; charset=utf-8', true, 500);
        echo 'ticket library missing';
        exit;
    }
    $failure = dzjs_check_challenge();
    if ($failure !== null) {
        header('Content-Type: text/plain; charset=utf-8', true, $failure[0]);
        echo $failure[1];
        exit;
    }

    // 先把正文算出来，再对这份字节签票据 —— 签完就不能再改 $body，
    // 否则客户端算出的哈希和票据里的对不上，整份清单会被拒。
    $body = json_encode($manifest, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    if (!is_string($body)) {
        $body = '{}';
    }
    header('Content-Type: application/json; charset=utf-8');
    dzjs_issue_ticket($body, dzjs_client_nonce());
    echo $body;
    exit;
}
header('Content-Type: text/plain; charset=utf-8', true, 400);
echo 'missing action';
