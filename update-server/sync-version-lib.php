<?php
// 从 GitHub Releases 生成站点根目录的 version.txt。
//
// 首页 script.js 的解析规则（改了这里必须同步改前端）：
//   版本块之间用空行分隔；
//   块内第 1 行版本号、第 2 行日期、第 3 行标题、其余每行一条更新点；
//   第一块驱动全站版本号显示，所有块按顺序渲染进更新日志。
//
// 因此这里只做一件事：把 release 的 markdown 正文压成「标题 + 短条目」的纯文本。
// 不做 HTML 渲染，前端是用 textContent 赋值的。

if (!defined('DZJS_SYNC_REPO')) define('DZJS_SYNC_REPO', 'yunsjxh/Dzjs-Trainer');
if (!defined('DZJS_SYNC_MAX_RELEASES')) define('DZJS_SYNC_MAX_RELEASES', 20);
if (!defined('DZJS_SYNC_MAX_ITEMS')) define('DZJS_SYNC_MAX_ITEMS', 8);
if (!defined('DZJS_SYNC_ITEM_CHARS')) define('DZJS_SYNC_ITEM_CHARS', 180);

// 「发布包说明」一类的内容是给下载者看的元信息，放到首页更新日志里是噪音。
// 用「包含」匹配那几个固定说法，避免只出现在条目后半段时漏掉。
function dzjs_sync_blocked_fragments(): array
{
    return array(
        '仅包含主程序', '目标平台', '主程序版本', 'SHA-256 校验文件',
        'Windows 文件/产品版本', '主程序内嵌已签名主驱动',
        '程序内部版本', 'Windows 文件版本', 'Windows 产品版本', '产品名称',
    );
}

// 通用标题没有信息量（"修了什么" / "更新内容"），换成版本号本身。
function dzjs_sync_generic_titles(): array
{
    return array('修了什么', '更新内容', '更新说明', '变更', '说明', '概述', 'changes', "what's new", 'whats new');
}

// 取一段 markdown 内联文本的纯文本形式：去掉强调、行内代码、链接、HTML。
function dzjs_sync_inline(string $text): string
{
    $text = preg_replace('/```[^`]*```/s', '', $text);
    $text = preg_replace('/!\[([^\]]*)\]\([^)]*\)/u', '$1', $text);   // 图片
    $text = preg_replace('/\[([^\]]*)\]\([^)]*\)/u', '$1', $text);     // 链接
    $text = preg_replace('/`([^`]*)`/u', '$1', $text);                 // 行内代码
    $text = preg_replace('/\*\*([^*]*)\*\*/u', '$1', $text);           // 粗体
    $text = preg_replace('/(?<!\*)\*([^*]+)\*(?!\*)/u', '$1', $text);  // 斜体
    $text = preg_replace('/~~([^~]*)~~/u', '$1', $text);               // 删除线
    $text = strip_tags($text);
    $text = str_replace(array("\xC2\xA0", '&nbsp;'), ' ', $text);
    $text = preg_replace('/\s+/u', ' ', $text);
    return trim($text);
}

// 按字符数截断，不切断多字节字符，末尾补省略号。
function dzjs_sync_clip(string $text, int $limit = DZJS_SYNC_ITEM_CHARS): string
{
    if (preg_match('/^.{1,' . $limit . '}/us', $text, $m) && $m[0] !== $text) {
        return rtrim($m[0]) . '…';
    }
    return $text;
}

// 按字符类型拼接折行：中文与中文之间直接相连，中英文之间补一个空格。
// markdown 正文经常在一句话中间强制换行，统一用空格拼会得到「三段 （%u.%u.%u）」这种空洞。
function dzjs_sync_join(string $left, string $right): string
{
    $left = rtrim($left);
    $right = ltrim($right);
    if ($left === '') return $right;
    if ($right === '') return $left;

    $last = preg_match('/.$/us', $left, $m) ? $m[0] : '';
    $first = preg_match('/^./us', $right, $n) ? $n[0] : '';
    $isCjk = function ($ch) {
        return $ch !== ''
            && preg_match('/[\x{3000}-\x{303F}\x{4E00}-\x{9FFF}\x{FF00}-\x{FFEF}\x{2018}-\x{201D}]/u', $ch) === 1;
    };

    if ($isCjk($last) && $isCjk($first)) return $left . $right;
    if ($isCjk($last)) return $left . (preg_match('/^[A-Za-z0-9]/', $first) ? ' ' : '') . $right;
    if ($isCjk($first)) return $left . (preg_match('/[A-Za-z0-9]$/', $last) ? ' ' : '') . $right;
    return $left . ' ' . $right;
}

// 标题：跳过与版本号重复的套话（"Dzjs Trainer v1.0.4"），取第一个有信息量的标题。
function dzjs_sync_title(array $release): string
{
    $tag = dzjs_sync_normalize_tag((string)($release['tag_name'] ?? ''));
    $name = trim((string)($release['name'] ?? ''));
    $body = (string)($release['body'] ?? '');

    $candidates = array();
    if (preg_match_all('/^#{2,4}\s+(.+)$/mu', $body, $m)) {
        foreach ($m[1] as $line) {
            $candidates[] = dzjs_sync_inline($line);
        }
    }

    foreach ($candidates as $candidate) {
        if ($candidate === '') continue;
        $lower = mb_strtolower($candidate, 'UTF-8');
        $isBoilerplate = ($tag !== '' && mb_strpos($lower, mb_strtolower($tag, 'UTF-8')) !== false)
            || mb_strpos($lower, 'dzjs trainer') !== false;
        if ($isBoilerplate) continue;
        if (in_array($lower, dzjs_sync_generic_titles(), true)) {
            return $tag !== '' ? '版本 ' . $tag : '版本更新';
        }
        return dzjs_sync_clip($candidate, 60);
    }

    if ($name !== '' && mb_strtolower($name, 'UTF-8') !== mb_strtolower($tag, 'UTF-8')
        && mb_strpos(mb_strtolower($name, 'UTF-8'), 'dzjs trainer') === false) {
        return dzjs_sync_clip(dzjs_sync_inline($name), 60);
    }
    return $tag !== '' ? '版本 ' . $tag : '版本更新';
}

function dzjs_sync_normalize_tag(string $tag): string
{
    $tag = trim($tag);
    if ($tag === '') return '';
    return preg_match('/^v/i', $tag) ? $tag : 'v' . $tag;
}

// 正文 → 若干条更新点。
// 优先用列表项；列表太少时用段落首句补足，否则像 1.0.4.2 这种纯叙述的正文只剩一条。
// 条目按正文出现顺序排列，而不是「先列表后段落」——否则「顺带说明」里的次要内容
// 会排到本版重点前面。
function dzjs_sync_items(array $release): array
{
    $body = str_replace("\r\n", "\n", (string)($release['body'] ?? ''));
    $lines = explode("\n", $body);
    $blocked = dzjs_sync_blocked_fragments();

    $entries = array();   // 每项 array('kind' => 'item'|'para', 'text' => ...)
    $buffer = '';
    $inCode = false;
    $lastWasItem = false;
    $lastIndex = -1;

    foreach ($lines as $line) {
        $trimmed = trim($line);
        if (strncmp($trimmed, '```', 3) === 0) {
            $inCode = !$inCode;
            $lastWasItem = false;
            $lastIndex = -1;
            continue;
        }
        if ($inCode) continue;

        if ($trimmed === '') {
            if ($buffer !== '') { $entries[] = array('kind' => 'para', 'text' => $buffer); $buffer = ''; }
            $lastWasItem = false;
            $lastIndex = -1;
            continue;
        }
        if (preg_match('/^#{1,6}\s/', $trimmed)) {              // 标题结束当前段落与列表
            if ($buffer !== '') { $entries[] = array('kind' => 'para', 'text' => $buffer); $buffer = ''; }
            $lastWasItem = false;
            $lastIndex = -1;
            continue;
        }
        if (preg_match('/^\|/', $trimmed)) continue;            // 表格
        if (preg_match('/^[-=]{3,}$/', $trimmed)) continue;     // 分隔线

        if (preg_match('/^\s*(?:[-*+]|\d+\.)\s+(.*)$/u', $line, $m)) {
            if ($buffer !== '') { $entries[] = array('kind' => 'para', 'text' => $buffer); $buffer = ''; }
            $entries[] = array('kind' => 'item', 'text' => $m[1]);
            $lastWasItem = true;
            $lastIndex = count($entries) - 1;
            continue;
        }

        // 列表项折行：markdown 允许一条列表项跨多行，续行不带列表标记。
        // 不拼接就会把一句话从逗号处切断（1.0.4.1 与 1.0.4.2 的正文都是这种写法）。
        if ($lastWasItem && $lastIndex >= 0 && preg_match('/^\s{2,}\S/', $line)) {
            $entries[$lastIndex]['text'] = dzjs_sync_join($entries[$lastIndex]['text'], $trimmed);
            continue;
        }

        $buffer = dzjs_sync_join($buffer, $trimmed);
        $lastWasItem = false;
    }
    if ($buffer !== '') $entries[] = array('kind' => 'para', 'text' => $buffer);

    $itemCount = 0;
    foreach ($entries as $entry) {
        if ($entry['kind'] === 'item') $itemCount++;
    }

    $out = array();
    $seen = array();
    $usedParagraphs = 0;
    foreach ($entries as $entry) {
        if ($entry['kind'] === 'para') {
            if ($itemCount >= 3) continue;          // 列表够多就不拿段落凑数
            if ($usedParagraphs >= 4) continue;
            $plain = dzjs_sync_inline($entry['text']);
            if ($plain === '') continue;
            // 只取第一句。优先中文句号，避免把 "1.0.4" 里的点当成句末。
            $text = preg_match('/^(.*?[。！？])/u', $plain, $m) ? trim($m[1]) : $plain;
            if (mb_strlen($text, 'UTF-8') < 8) continue;
            $usedParagraphs++;
        } else {
            $text = dzjs_sync_inline($entry['text']);
        }
        if ($text === '') continue;

        $skip = false;
        foreach ($blocked as $fragment) {
            if (mb_strpos($text, $fragment, 0, 'UTF-8') !== false) { $skip = true; break; }
        }
        if ($skip) continue;

        $text = dzjs_sync_clip($text);
        $key = mb_strtolower($text, 'UTF-8');
        if (isset($seen[$key])) continue;
        $seen[$key] = true;
        $out[] = $text;
        if (count($out) >= DZJS_SYNC_MAX_ITEMS) break;
    }
    return $out;
}

// 现有文件里那些不是 release 生成的块（例如「项目翻新」）原样保留到末尾。
function dzjs_sync_preserved_blocks(string $current): string{
    if (trim($current) === '') return '';
    $blocks = preg_split('/\r?\n\s*\r?\n/', trim(str_replace("\r\n", "\n", $current)));
    $keep = array();
    foreach ($blocks as $block) {
        $first = trim(strtok($block, "\n"));
        if ($first === '') continue;
        // 以 v + 数字开头的块由 GitHub 重新生成，其余视为手写历史。
        if (preg_match('/^v\d/i', $first)) continue;
        $keep[] = trim($block);
    }
    return $keep ? implode("\n\n", $keep) : '';
}

// 现有文件里以 v 开头的块，按版本号建索引。
// 用途：某个 release 的正文只有发布包元信息（v1.0.0 就是这种），过滤完一条不剩，
// 直接覆盖会把站点上原本写得更详细的内容清空 —— 这种情况沿用现有块。
function dzjs_sync_existing_blocks(string $current): array
{
    $map = array();
    if (trim($current) === '') return $map;
    $blocks = preg_split('/\r?\n\s*\r?\n/', trim(str_replace("\r\n", "\n", $current)));
    foreach ($blocks as $block) {
        $lines = explode("\n", trim($block));
        $first = trim(isset($lines[0]) ? $lines[0] : '');
        if ($first === '' || !preg_match('/^v\d/i', $first)) continue;
        $map[mb_strtolower($first, 'UTF-8')] = trim($block);
    }
    return $map;
}

function dzjs_sync_render(array $releases, string $preservedTail = '', array $existingBlocks = array()): string
{
    $blocks = array();
    foreach ($releases as $release) {
        if (!empty($release['draft'])) continue;
        $tag = dzjs_sync_normalize_tag((string)($release['tag_name'] ?? ''));
        if ($tag === '') continue;

        $items = dzjs_sync_items($release);
        if (!$items) {
            $key = mb_strtolower($tag, 'UTF-8');
            if (isset($existingBlocks[$key])) {
                $blocks[] = $existingBlocks[$key];
                if (count($blocks) >= DZJS_SYNC_MAX_RELEASES) break;
                continue;
            }
        }

        $published = (string)($release['published_at'] ?? '');
        $timestamp = $published !== '' ? strtotime($published) : time();
        // GitHub 给的是 UTC，站点面向中文用户，按北京时间落日期。
        $date = $timestamp ? gmdate('Y.m.d', $timestamp + 8 * 3600) : gmdate('Y.m.d');

        $lines = array($tag, $date, dzjs_sync_title($release));
        foreach ($items as $item) {
            $lines[] = '- ' . $item;
        }
        $blocks[] = implode("\n", $lines);
        if (count($blocks) >= DZJS_SYNC_MAX_RELEASES) break;
    }

    if ($preservedTail !== '') $blocks[] = $preservedTail;
    return implode("\n\n", $blocks) . "\n";
}

// 拉 release 列表。公开仓库不需要凭证；配了 github-token.php 就用，能提高限额。
function dzjs_sync_fetch_releases(string $repo = DZJS_SYNC_REPO): array
{
    $token = '';
    $tokenFile = __DIR__ . '/github-token.php';
    if (is_readable($tokenFile)) {
        $loaded = include $tokenFile;
        if (is_string($loaded)) $token = trim($loaded);
    }
    if ($token === '' && getenv('GITHUB_TOKEN')) $token = trim((string)getenv('GITHUB_TOKEN'));

    $url = 'https://api.github.com/repos/' . $repo . '/releases?per_page=' . DZJS_SYNC_MAX_RELEASES;
    $headers = array(
        'Accept: application/vnd.github+json',
        'User-Agent: DzjsTrainer-SiteSync',
        'X-GitHub-Api-Version: 2022-11-28',
    );
    if ($token !== '') $headers[] = 'Authorization: Bearer ' . $token;

    $ch = curl_init($url);
    curl_setopt_array($ch, array(
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_HTTPHEADER => $headers,
        CURLOPT_CONNECTTIMEOUT => 10,
        CURLOPT_TIMEOUT => 25,
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

// 原子写入：先写临时文件再 rename，避免前端读到半截文件。
function dzjs_sync_write(string $path, string $text): bool
{
    $dir = dirname($path);
    $tmp = tempnam($dir, '.version-');
    if ($tmp === false) throw new RuntimeException('无法在 ' . $dir . ' 创建临时文件');
    if (file_put_contents($tmp, $text, LOCK_EX) === false) {
        @unlink($tmp);
        throw new RuntimeException('写入临时文件失败');
    }
    @chmod($tmp, 0644);
    if (!rename($tmp, $path)) {
        @unlink($tmp);
        throw new RuntimeException('替换 ' . basename($path) . ' 失败');
    }
    @chmod($path, 0644);
    return true;
}

function dzjs_sync_log(string $message): void
{
    $line = '[' . date('Y-m-d H:i:s') . '] ' . $message . "\n";
    @file_put_contents(__DIR__ . '/sync-version.log', $line, FILE_APPEND | LOCK_EX);
}

/**
 * 完整流程：取 releases → 渲染 → 与现有文件比较 → 有变化才落盘。
 * 返回 array(ok, changed, version, text, error)。
 */
function dzjs_sync_run(string $targetFile, bool $force = false, bool $dryRun = false, string $repo = DZJS_SYNC_REPO): array
{
    $result = array('ok' => false, 'changed' => false, 'version' => '', 'text' => '', 'error' => '');

    try {
        $releases = dzjs_sync_fetch_releases($repo);
        $current = is_readable($targetFile) ? (string)file_get_contents($targetFile) : '';
        $text = dzjs_sync_render(
            $releases,
            dzjs_sync_preserved_blocks($current),
            dzjs_sync_existing_blocks($current)
        );

        $result['text'] = $text;
        $result['version'] = isset($releases[0]['tag_name']) ? dzjs_sync_normalize_tag((string)$releases[0]['tag_name']) : '';

        $same = ($current !== '' && rtrim($current) === rtrim($text));
        if ($same && !$force) {
            $result['ok'] = true;
            return $result;
        }
        if ($dryRun) {
            $result['ok'] = true;
            $result['changed'] = true;
            return $result;
        }

        if ($current !== '') {
            // 备份带点前缀：它和 version.txt 在同一个站点根，不加点会被当成静态文件
            // 直接下载走（虽然内容本身是公开的，但没必要多一个可访问路径）。
            @copy($targetFile, dirname($targetFile) . '/.version.txt.bak');
        }
        dzjs_sync_write($targetFile, $text);
        $result['ok'] = true;
        $result['changed'] = true;
        dzjs_sync_log('同步成功：' . $result['version'] . '，' . count($releases) . ' 个 release，' . strlen($text) . ' 字节');
        return $result;
    } catch (Throwable $e) {
        $result['error'] = $e->getMessage();
        dzjs_sync_log('同步失败：' . $e->getMessage());
        return $result;
    }
}
