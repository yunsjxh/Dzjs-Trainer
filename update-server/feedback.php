<?php
declare(strict_types=1);

// 客户端「意见反馈」的接收端。
//
// 做四件事：拦下不是官方客户端发来的、限流、看魔数、原样落盘。zip 不解压，
// meta 解析成 JSON 存一份 —— 反馈本来就是给人看的，多一层解析只会多一个
// 出错的地方。
//
// 鉴权是双向的
// ------------
//   上行  客户端用内置私钥签  X-Dzjs-Feedback-Sig
//         服务端用 feedback-pub.php 里的公钥验
//   下行  服务端用 ticket-sign 私钥签  X-Dzjs-Reply
//         客户端用编进 exe 的公钥验
//
// 为什么必须验上行
// ----------------
// 这个接口在公网上，谁都能 POST。只靠限流的话，换 IP 就绕过去了；只靠
// User-Agent 更没意义，curl 也能写。签名是唯一能区分"官方客户端"和
// "脚本"的东西 —— 私钥嵌在 exe 里，逆向能挖出来，但那是真正的工程量，
// 不是随手写个 for 循环。
//
// 签名覆盖面
// ----------
//   canonical = "DZJS-FEEDBACK/1|<ts>|<nonce>|<metaSha>|<bundleSha>"
//   metaSha / bundleSha 是服务端实收字节的 SHA-256，不是客户端声称的值 ——
//   这样中途改一个字节就验不过，签名不能脱离内容被复用。
//
// 目录结构：
//   feedback/2026-11-21/164012-3f9a1c2b/bundle.zip
//   feedback/2026-11-21/164012-3f9a1c2b/meta.json
//   feedback/.rate/<sha1(ip)>.txt     限流记录
//   feedback/.nonce/<sha1(nonce)>     重放记录，10 分钟后清

// 与 ticket-sign 共用一把下行密钥，不必为反馈单独再养一对。
require_once __DIR__ . '/update-ticket.php';

const MAX_BUNDLE_BYTES = 8 * 1024 * 1024;
// 同一 IP：一小时内 1 次，24 小时内 2 次。两个都是滚动窗口，不按自然日重置 ——
// 跨零点清零没什么意义，能连着传的照样连着传。
const MAX_PER_HOUR = 1;
const MAX_PER_DAY = 2;
const HOUR_SECONDS = 3600;
const DAY_SECONDS = 86400;

// 客户端时间戳的容忍窗口。取 5 分钟，比票据的 10 分钟紧一档：反馈是即时动作，
// 不像取清单那样可能发生在刚唤醒的机器上。再宽就等于给了重放一个长窗口。
const SIGN_WINDOW_SECONDS = 300;
// nonce 保留多久。必须 ≥ 时间窗的两倍，否则窗口内还能重放。
const NONCE_TTL_SECONDS = 600;

function respond(int $status, string $text): void
{
    http_response_code($status);
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');

    // 下行签名。客户端只对 200 校验，错误响应签不签无所谓，但统一签上更省心，
    // 也不用在客户端写两套判断。签不出来（私钥缺失）不算致命：正文照发，
    // 客户端那边会以"服务器没有签署回复"拒绝，日志里能看到真正的原因。
    $stamp = time();
    $hash = hash('sha256', $text);
    $signature = '';
    $key = dzjs_signing_key();
    if ($key !== null
        && openssl_sign('DZJS-FEEDBACK-REPLY/1|' . $stamp . '|' . $hash, $signature, $key, OPENSSL_ALGO_SHA256)) {
        header('X-Dzjs-Reply: 1|' . $stamp . '|' . $hash . '|' . bin2hex($signature));
    } else {
        error_log('[dzjs] feedback reply signing unavailable');
    }

    echo $text;
    exit;
}

// 站点在 Cloudflare 后面，REMOTE_ADDR 是边缘节点的地址 —— 拿它限流，同一个
// 节点下的所有用户会被算成一台机器，第一次有人提交就把整片区域锁了。
// CF-Connecting-IP 是 CF 填的真实来源；X-Forwarded-For 兜没有 CF 的情况。
// 直连源站伪造这两个头只能绕过自己的限流，没什么可图的。
function client_ip(): string
{
    $candidates = [
        (string)($_SERVER['HTTP_CF_CONNECTING_IP'] ?? ''),
        (string)($_SERVER['HTTP_X_FORWARDED_FOR'] ?? ''),
        (string)($_SERVER['REMOTE_ADDR'] ?? ''),
    ];
    foreach ($candidates as $index => $raw) {
        $value = trim($index === 1 ? (string)explode(',', $raw)[0] : $raw);
        if (filter_var($value, FILTER_VALIDATE_IP)) return $value;
    }
    return '0.0.0.0';
}

// 限流键：IPv6 按 /64 归一。家用宽带拿的是 /64 前缀，隐私地址（RFC 4941）
// 一天换好几次，按 /128 记等于没限流。IPv4 原样用。
function rate_key(string $ip): string
{
    $packed = @inet_pton($ip);
    if ($packed === false || strlen($packed) !== 16) return $ip;
    return (string)inet_ntop(substr($packed, 0, 8) . str_repeat("\0", 8));
}

// 上行验签用的公钥。读不到就是配置问题，不是请求的问题，所以调用方给 500。
function feedback_public_key()
{
    static $cached = false;
    if ($cached !== false) return $cached;
    $cached = null;

    $path = __DIR__ . '/feedback-pub.php';
    if (!is_file($path)) {
        error_log('[dzjs] feedback-pub.php missing');
        return null;
    }
    $pem = include $path;
    if (!is_string($pem) || strpos($pem, 'BEGIN') === false) {
        error_log('[dzjs] feedback-pub.php 存在但内容不是 PEM');
        return null;
    }
    $key = openssl_pkey_get_public($pem);
    if ($key === false) {
        error_log('[dzjs] feedback-pub.php 无法作为公钥加载');
        return null;
    }
    $cached = $key;
    return $cached;
}

// 拆 "1|<ts>|<nonce>|<metaSha>|<bundleSha>|<sig>"。段数固定六段。
// 返回 null 表示格式不对 —— 这时候连字段名都不用看，直接拒。
function parse_signature_header(string $text): ?array
{
    $parts = explode('|', $text);
    if (count($parts) !== 6) return null;
    if ($parts[0] !== '1') return null;
    if (!preg_match('/^[0-9]{1,12}$/', $parts[1])) return null;
    if (!preg_match('/^[0-9a-f]{32}$/', $parts[2])) return null;
    if (!preg_match('/^[0-9a-f]{64}$/', $parts[3])) return null;
    if (!preg_match('/^[0-9a-f]{64}$/', $parts[4])) return null;
    if (!preg_match('/^[0-9a-f]{512}$/', $parts[5])) return null;
    return [
        'stamp' => (int)$parts[1],
        'nonce' => $parts[2],
        'meta' => $parts[3],
        'bundle' => $parts[4],
        'signature' => $parts[5],
    ];
}

// 清掉过期的 nonce 文件。按概率触发，不必每次请求都扫目录。
function prune_nonces(string $dir): void
{
    $deadline = time() - NONCE_TTL_SECONDS;
    $handle = @opendir($dir);
    if (!$handle) return;
    while (($name = readdir($handle)) !== false) {
        if ($name === '.' || $name === '..') continue;
        $path = $dir . '/' . $name;
        if (@filemtime($path) < $deadline) @unlink($path);
    }
    closedir($handle);
}

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    respond(405, '只接受 POST');
}

// Content-Length 早退。PHP 其实在脚本启动前就已经把 body 落进临时文件了，
// 真正拦得住大包的是 nginx 那条 client_max_body_size；这里只是让 PHP 少走
// 几层解析，顺手给个明确的 413。
$contentLength = (int)($_SERVER['CONTENT_LENGTH'] ?? 0);
if ($contentLength > MAX_BUNDLE_BYTES + 160 * 1024) {
    respond(413, '包太大');
}

$root = __DIR__;
$store = $root . '/feedback';
if (!is_dir($store) && !@mkdir($store, 0755, true) && !is_dir($store)) {
    respond(500, '无法创建存储目录');
}

// ---- 签名头：格式 → 时间窗，都在解析 body 之前，便宜的先做

$signatureText = (string)($_SERVER['HTTP_X_DZJS_FEEDBACK_SIG'] ?? '');
if ($signatureText === '') {
    respond(403, '缺少签名');
}
$claim = parse_signature_header($signatureText);
if ($claim === null) {
    respond(403, '签名格式不对');
}
$now = time();
if (abs($now - $claim['stamp']) > SIGN_WINDOW_SECONDS) {
    // 本机时间跑偏的人最需要能提交反馈，却正好被这一条挡住。文案要说清楚
    // 怎么办，别只丢一句"过期"。
    respond(403, '本机时间与服务器相差太大，请校准后重试');
}

// 公钥缺失是部署问题：直接 500，别让客户端以为是自己签名不对。
$publicKey = feedback_public_key();
if ($publicKey === null) {
    respond(500, '服务器未配置验签公钥');
}

// ---- 限流。读一次小文件，比验签便宜得多，放在前面。

$ip = client_ip();
// nginx 的 realip 会把 REMOTE_ADDR 换成真实客户端，边缘节点只剩在
// DZJS_EDGE_IP 里（那条 fastcgi_param 写在站点的 feedback location 上）。
// 没有 nginx 那一层时退回 REMOTE_ADDR。
$edgeIp = (string)($_SERVER['DZJS_EDGE_IP'] ?? $_SERVER['REMOTE_ADDR'] ?? '');
$rateDir = $store . '/.rate';
if (!is_dir($rateDir)) @mkdir($rateDir, 0755, true);
$rateFile = $rateDir . '/' . sha1(rate_key($ip)) . '.txt';
$hits = [];
if (is_file($rateFile)) {
    $raw = (string)file_get_contents($rateFile);
    foreach (explode("\n", $raw) as $line) {
        $stamp = (int)trim($line);
        // 只留 24 小时内的，文件不会越攒越长。
        if ($stamp > $now - DAY_SECONDS && $stamp <= $now) $hits[] = $stamp;
    }
    sort($hits);
}
$hourHits = array_values(array_filter($hits, static fn(int $t): bool => $t > $now - HOUR_SECONDS));
if (count($hourHits) >= MAX_PER_HOUR || count($hits) >= MAX_PER_DAY) {
    // 就回 429，不解释还剩多久：等多久是客户端该说的话，服务端报个数就够了。
    respond(429, '429');
}

// ---- 内容校验

$metaText = isset($_POST['meta']) ? (string)$_POST['meta'] : '';
if ($metaText === '') respond(400, '缺少 meta 字段');
if (strlen($metaText) > 65536) respond(413, 'meta 太大');

$meta = json_decode($metaText, true);
if (!is_array($meta)) respond(400, 'meta 不是合法 JSON');
$description = trim((string)($meta['description'] ?? ''));
$repro = trim((string)($meta['repro'] ?? ''));
if ($description === '' || $repro === '') respond(400, '描述和复现步骤不能为空');

if (!isset($_FILES['bundle'])) respond(400, '缺少 bundle 文件');
$file = $_FILES['bundle'];
$error = (int)($file['error'] ?? UPLOAD_ERR_NO_FILE);
if ($error !== UPLOAD_ERR_OK) respond(400, '上传失败（错误码 ' . $error . '）');
$size = (int)($file['size'] ?? 0);
if ($size <= 0) respond(400, '包是空的');
if ($size > MAX_BUNDLE_BYTES) respond(413, '包太大，最多 ' . (MAX_BUNDLE_BYTES / 1048576) . ' MB');

$tmpPath = (string)$file['tmp_name'];

// zip 的本地文件头固定是 PK\x03\x04。只看魔数：存下来的东西不会被站点执行，
// 真要解压也是在本地做，服务端没必要替它校验整个压缩包结构。
$handle = @fopen($tmpPath, 'rb');
$magic = $handle ? (string)fread($handle, 4) : '';
if ($handle) fclose($handle);
if ($magic !== "PK\x03\x04") respond(400, '包不是 zip');

// ---- 验签。两个摘要都对着服务端实收的字节算，不信 header 里声称的值。

$metaHash = hash('sha256', $metaText);
$bundleHash = hash_file('sha256', $tmpPath);
if (!hash_equals($claim['meta'], $metaHash) || !hash_equals($claim['bundle'], (string)$bundleHash)) {
    respond(403, '签名与内容不符');
}

$message = 'DZJS-FEEDBACK/1|' . $claim['stamp'] . '|' . $claim['nonce'] . '|'
    . $claim['meta'] . '|' . $claim['bundle'];
$signatureBinary = hex2bin($claim['signature']);
$verified = $signatureBinary !== false
    && openssl_verify($message, $signatureBinary, $publicKey, OPENSSL_ALGO_SHA256) === 1;
if (!$verified) {
    error_log('[dzjs] feedback signature rejected from ' . $ip);
    respond(403, '签名验证失败');
}

// ---- 重放。必须在验签之后：先认了身份再占坑，垃圾请求不会把目录灌满。
// 用 'x' 打开是原子的，两个并发的相同 nonce 只有一个能建成文件。

$nonceDir = $store . '/.nonce';
if (!is_dir($nonceDir)) @mkdir($nonceDir, 0755, true);
if (function_exists('random_int') && random_int(1, 40) === 1) {
    prune_nonces($nonceDir);
}
$noncePath = $nonceDir . '/' . sha1($claim['nonce']);
$nonceHandle = @fopen($noncePath, 'x');
if ($nonceHandle === false) {
    respond(403, '这个请求已经提交过了');
}
fwrite($nonceHandle, (string)$now);
fclose($nonceHandle);

// ---- 落盘

$day = date('Y-m-d');
$dayDir = $store . '/' . $day;
if (!is_dir($dayDir) && !@mkdir($dayDir, 0755, true) && !is_dir($dayDir)) {
    respond(500, '无法创建日期目录');
}

// 目录名由服务端生成，用户给的任何字符串都不参与路径。
$id = date('His') . '-' . bin2hex(random_bytes(4));
$target = $dayDir . '/' . $id;
for ($attempt = 0; is_dir($target) && $attempt < 5; ++$attempt) {
    $id = date('His') . '-' . bin2hex(random_bytes(4));
    $target = $dayDir . '/' . $id;
}
if (!@mkdir($target, 0755, true)) respond(500, '无法创建反馈目录');

if (!move_uploaded_file($tmpPath, $target . '/bundle.zip')) {
    respond(500, '无法保存反馈包');
}

$meta['_server'] = [
    'received_at' => date('c'),
    'ip' => $ip,
    // 走 CDN 时 $ip 是客户端，这一条是落地的边缘节点，排查链路对不上时有用。
    'edge_ip' => $edgeIp,
    'user_agent' => substr((string)($_SERVER['HTTP_USER_AGENT'] ?? ''), 0, 200),
    'bundle_bytes' => $size,
    // 客户端那边的时间。它与 received_at 差多少，能直接看出机器时钟偏了多少。
    'client_time' => date('c', $claim['stamp']),
    'signed' => true,
];
@file_put_contents($target . '/meta.json',
    json_encode($meta, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
// Apache 会列出没有索引文件的目录；nginx 默认不列，但两个都照顾一下。
@file_put_contents($target . '/index.html', '');
@file_put_contents($store . '/index.html', '');

$hits[] = $now;
@file_put_contents($rateFile, implode("\n", $hits), LOCK_EX);

respond(200, 'ok ' . $day . '/' . $id);
