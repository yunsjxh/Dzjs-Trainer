<?php
declare(strict_types=1);

// 时效票据（DZJS-TICKET/1）的服务端一半。
//
// 与客户端 DzjsTrainerUpdater/UpdaterTicket.h 成对使用，任何一侧改动都要同步。
//
// 协议：
//   请求  ?manifest=1&n=<32hex nonce>&t=<unix>&v=<客户端代次>
//   响应  X-Dzjs-Ticket: 1|n|issued|expires|manifest|<sha256(body)>|<sig>
//         sig = hex(openssl_sign("DZJS-TICKET/1|n|issued|expires|manifest|<sha256(body)>"))
//
// 签名用 RSA-2048 私钥（ticket-sign.pem），客户端只内置公钥。
// 私钥一旦泄漏就等于更新通道失守，所以它只能留在服务器上、不进版本库；
// 反过来公钥泄漏不产生任何签发能力，这就是不用对称 HMAC 的原因 ——
// HMAC 需要把密钥编进客户端，开源即失效，拿到密钥的人能伪造整张票据。
//
// 请求侧不需要签名：清单本来就是公开内容，需要自证的是响应方向。
//
// 注意：票据的 bodyHash 必须对"最终输出的字节"计算。任何在 echo 之前的
// 输出（PHP 警告、BOM、debug 打印）都会让客户端算出不同的哈希而整体拒绝，
// 排查时先看有没有多输出的字节。

const DZJS_TICKET_TTL = 300;    // 票据有效期，秒
const DZJS_TICKET_SKEW = 600;   // 容忍的客户端时钟偏差，秒

/**
 * 读取签名私钥。拿不到返回 null。
 *
 * 三级回退，三种形式存的是同一把 PEM：
 *
 *   1) 环境变量 DZJS_TICKET_SIGN_KEY —— 直接放 PEM 文本，容器/多站点部署用。
 *   2) ticket-sign.pem —— 推荐。权限 600、属主与 php-fpm 同用户。
 *      注意 nginx 默认不拦 .pem，必须靠文件权限或 deny 规则挡住直接下载。
 *   3) ticket-sign.php —— <?php return '-----BEGIN ...'; 用于 open_basedir
 *      把 PHP 锁在站点根目录内（宝塔默认如此）、而 .pem 又无法保证不被当
 *      静态文件下载的环境。宝塔线上用的就是这一种。
 *
 * 无论哪种，私钥文件都不进版本库（见 .gitignore）。
 *
 * @return mixed OpenSSLAsymmetricKey|null
 */
function dzjs_signing_key()
{
    static $cached = false;
    if ($cached !== false) {
        return $cached;
    }
    $cached = null;

    $env = getenv('DZJS_TICKET_SIGN_KEY');
    if (is_string($env) && strpos($env, 'BEGIN') !== false) {
        $key = openssl_pkey_get_private($env);
        if ($key !== false) {
            $cached = $key;
            return $cached;
        }
    }

    foreach (['ticket-sign.pem', 'ticket-sign.php'] as $name) {
        $path = __DIR__ . DIRECTORY_SEPARATOR . $name;
        if (!is_file($path)) {
            continue;
        }
        $pem = $name === 'ticket-sign.php' ? include $path : file_get_contents($path);
        if (!is_string($pem) || strpos($pem, 'BEGIN') === false) {
            error_log('[dzjs] ' . $name . ' 存在但内容不是 PEM');
            continue;
        }
        $key = openssl_pkey_get_private($pem);
        if ($key !== false) {
            $cached = $key;
            return $cached;
        }
        error_log('[dzjs] ' . $name . ' 无法作为私钥加载');
    }
    return $cached;
}

// 票据签名覆盖面（不含 sig 本身）。客户端算的是同一个串。
function dzjs_ticket_message(string $nonce, int $issued, int $expires, string $scope, string $bodyHash): string
{
    return 'DZJS-TICKET/1|' . $nonce . '|' . $issued . '|' . $expires . '|' . $scope . '|' . $bodyHash;
}

/**
 * 对一段消息签名，返回小写十六进制；失败返回空串。
 *
 * 输出的是标准 PKCS#1 v1.5 大端签名。客户端那边交给 CryptoAPI 之前会反转成
 * 小端 —— 那是 Windows CSP 的历史包袱，不是协议的一部分。
 */
function dzjs_sign(string $message): string
{
    $key = dzjs_signing_key();
    if ($key === null) {
        return '';
    }
    $signature = '';
    if (!openssl_sign($message, $signature, $key, OPENSSL_ALGO_SHA256)) {
        error_log('[dzjs] openssl_sign failed: ' . (string)openssl_error_string());
        return '';
    }
    return bin2hex($signature);
}

/**
 * 校验请求参数。通过返回 null，失败返回 HTTP 状态码和原因。
 *
 * 这里刻意不验任何"客户端身份"：客户端不持有签名材料，请求侧无法自证。
 * 格式和时钟检查的作用只是让垃圾请求早点离开，不能当凭据用。
 *
 * @return array{0:int,1:string}|null
 */
function dzjs_check_challenge(): ?array
{
    if (dzjs_signing_key() === null) {
        // 没装私钥就签不出票据。直接 500 比静默放行诚实：客户端会以
        // "服务器未提供时效票据"拒绝，而这里能留下真正的原因。
        error_log('[dzjs] ticket-sign.pem missing or not a usable private key');
        return [500, 'signing key not installed'];
    }

    $nonce = isset($_GET['n']) ? (string)$_GET['n'] : '';
    $timestampText = isset($_GET['t']) ? (string)$_GET['t'] : '';
    $clientVersion = isset($_GET['v']) ? (string)$_GET['v'] : '';

    if (!preg_match('/^[0-9a-f]{32}$/', $nonce)) {
        return [403, 'bad nonce'];
    }
    if (!preg_match('/^[0-9]{1,12}$/', $timestampText)) {
        return [403, 'bad timestamp'];
    }
    if (!preg_match('/^[0-9a-z.\-]{1,16}$/', $clientVersion)) {
        return [403, 'bad client version'];
    }

    $timestamp = (int)$timestampText;
    // 时钟偏差太大说明对方的机器时间乱得厉害：这种请求既可能是重放，
    // 也可能只是用户改了系统时间。两种情况都不给票，让他去对时。
    if (abs(time() - $timestamp) > DZJS_TICKET_SKEW) {
        return [403, 'clock skew'];
    }
    return null;
}

/**
 * 给一段正文签票据并写进响应头。
 *
 * 调用方必须保证 $body 就是随后 echo 出去的那串字节 —— 中间不能再拼接、
 * 不能有额外输出，否则客户端算出的哈希对不上。
 *
 * 返回票据原文，方便测试脚本拿到同一份值做交叉验证；签名失败时返回空串，
 * 此时响应头里不会有票据，客户端会明确拒绝这份清单。
 */
function dzjs_issue_ticket(string $body, string $nonce, string $scope = 'manifest'): string
{
    $issued = time();
    $expires = $issued + DZJS_TICKET_TTL;
    $bodyHash = hash('sha256', $body);
    $signature = dzjs_sign(dzjs_ticket_message($nonce, $issued, $expires, $scope, $bodyHash));
    if ($signature === '') {
        return '';
    }
    $ticket = '1|' . $nonce . '|' . $issued . '|' . $expires . '|' . $scope . '|'
        . $bodyHash . '|' . $signature;
    header('X-Dzjs-Ticket: ' . $ticket);
    return $ticket;
}

function dzjs_client_nonce(): string
{
    return isset($_GET['n']) ? (string)$_GET['n'] : '';
}
