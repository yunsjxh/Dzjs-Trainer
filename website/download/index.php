<?php
/**
 * 多 URL 批量「直接写入文件夹」下载站
 *
 * 机制：前端 showDirectoryPicker() 让用户选择任意文件夹，
 *       再通过本文件的 ?proxy= 接口流式取回文件，直接写入该文件夹。
 *
 * 用法：
 *   批量页：download/index.php?url=<文件1>&url1=<文件2>&url2=...
 *   代理流：download/index.php?proxy=<文件或URL>   （由批量页自动调用，带进度）
 *   附件流：download/index.php?dl=0&url=<文件1>    （不支持文件夹选择器时的回退）
 *
 * 支持两类地址：
 *   1. 本地相对路径（相对站点根，即本文件的上一级目录，如 Release-v1.0.0/DzjsTrainer.exe）
 *   2. 外部完整 URL（须在 $ALLOW_HOSTS 白名单内）
 */

// ============ 配置 ============
$ALLOW_REMOTE = true;                      // 是否允许代理外部 URL
$ALLOW_HOSTS  = [                          // 允许代理的外部域名
];
// 本地文件根目录（相对路径的越界检查与解析基准），默认为站点根（download/ 的上一级）。
$LOCAL_ROOT = realpath(__DIR__ . '/..') ?: __DIR__ . '/..';
// ==============================

// —— 收集 url / url1 / url2 ... 参数（兼容前导空格与重复值）——
$urls = [];
foreach ($_GET as $k => $v) {
    if (is_string($v) && preg_match('/^url\d*$/i', $k)) {
        $v = trim($v);
        if ($v !== '') $urls[] = $v;
    }
}
$urls = array_values(array_unique($urls));

/**
 * 解析本地相对路径为绝对路径（含越界检查），外部 URL 原样返回
 * 返回 null 表示非法或不存在
 */
function resolve_source(string $url): ?array
{
    global $ALLOW_REMOTE, $ALLOW_HOSTS, $LOCAL_ROOT;

    if (!preg_match('#^https?://#i', $url)) {
        // —— 本地文件（相对站点根解析）——
        $path = realpath($LOCAL_ROOT . '/' . ltrim($url, '/'));
        $base = realpath($LOCAL_ROOT);
        if ($path === false || $base === false || strpos($path, $base) !== 0 || !is_file($path)) {
            return null;
        }
        return ['type' => 'local', 'path' => $path, 'name' => basename($path), 'size' => filesize($path)];
    }

    // —— 外部 URL：校验域名白名单 ——
    $host = strtolower((string)parse_url($url, PHP_URL_HOST));
    if (!$ALLOW_REMOTE || !in_array($host, array_map('strtolower', $ALLOW_HOSTS), true)) {
        return null;
    }
    $name = basename(parse_url($url, PHP_URL_PATH) ?: 'download');
    return ['type' => 'remote', 'url' => $url, 'name' => $name, 'size' => null];
}

/**
 * 输出公共响应头
 */
function send_headers(string $name, ?int $size, bool $attachment): void
{
    $ascii = preg_replace('/[^A-Za-z0-9._-]/', '_', $name) ?: 'download';
    header('Content-Type: application/octet-stream');
    header(($attachment ? 'Content-Disposition: attachment; ' : 'Content-Disposition: inline; ')
        . "filename=\"{$ascii}\"; filename*=UTF-8''" . rawurlencode($name));
    header('Cache-Control: no-store');
    header('X-Content-Type-Options: nosniff');
    header('Access-Control-Expose-Headers: X-Total-Size');
    if ($size !== null) header('X-Total-Size: ' . $size);
}

// ============ 代理流模式（前端 fetch 拉取并写入用户选择的文件夹）============
if (isset($_GET['proxy'])) {
    $src = resolve_source(trim((string)$_GET['proxy']));
    if ($src === null) { http_response_code(404); exit('文件不存在或不在允许列表'); }

    if ($src['type'] === 'local') {
        send_headers($src['name'], (int)$src['size'], false);
        header('Content-Length: ' . $src['size']);
        readfile($src['path']);
        exit;
    }

    @set_time_limit(0);
    $ch = curl_init($src['url']);
    if ($ch === false) { http_response_code(502); exit('无法初始化下载'); }

    // 先取远端大小用于进度条
    $head = curl_init($src['url']);
    $size = null;
    if ($head !== false) {
        curl_setopt_array($head, [
            CURLOPT_NOBODY         => true,
            CURLOPT_FOLLOWLOCATION => true,
            CURLOPT_TIMEOUT        => 15,
            CURLOPT_USERAGENT      => 'DzjsTrainer-BatchDownloader/1.0',
        ]);
        curl_exec($head);
        $cl = curl_getinfo($head, CURLINFO_CONTENT_LENGTH_DOWNLOAD);
        if ($cl > 0) $size = (int)$cl;
        curl_close($head);
    }
    send_headers($src['name'], $size, false);

    $out = fopen('php://output', 'wb');
    curl_setopt_array($ch, [
        CURLOPT_FILE           => $out,
        CURLOPT_FOLLOWLOCATION => true,
        CURLOPT_MAXREDIRS      => 5,
        CURLOPT_TIMEOUT        => 600,
        CURLOPT_USERAGENT      => 'DzjsTrainer-BatchDownloader/1.0',
        CURLOPT_HEADER         => false,
    ]);
    curl_exec($ch);
    $err = curl_errno($ch);
    curl_close($ch);
    fclose($out);
    if ($err !== 0) { http_response_code(502); exit('远端下载失败'); }
    exit;
}

// ============ 附件直下模式（回退用）============
if (isset($_GET['dl'])) {
    $i = (int)$_GET['dl'];
    $url = $urls[$i] ?? '';
    if ($url === '') { http_response_code(400); exit('缺少下载地址'); }
    $src = resolve_source($url);
    if ($src === null) { http_response_code(404); exit('文件不存在或不在允许列表'); }

    if ($src['type'] === 'local') {
        send_headers($src['name'], (int)$src['size'], true);
        header('Content-Length: ' . $src['size']);
        readfile($src['path']);
        exit;
    }
    // 远端附件模式：302 跳转即可（浏览器直接下载）
    header('Location: ' . $src['url'], true, 302);
    exit;
}

// ============ 页面渲染：带参数 = 批量写入页；无参数 = 主界面（工具 + 文档）============
$payload = json_encode(
    array_map(fn($u, $i) => ['dl' => '?dl=' . $i . '&url=' . rawurlencode($u), 'url' => $u], $urls, array_keys($urls)),
    JSON_UNESCAPED_SLASHES | JSON_HEX_TAG | JSON_HEX_AMP | JSON_HEX_APOS | JSON_HEX_QUOT
) ?: '[]';
header('Content-Type: text/html; charset=utf-8');
?>
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="robots" content="noindex">
  <title><?= $urls ? '批量写入文件夹' : '批量写入 API' ?> · Dzjs Trainer</title>
  <style>
    :root{--ink:#142522;--muted:#6a7975;--paper:#f4f7f5;--teal:#087d70;--teal-dark:#075e58;--line:#d3dfdb;--ok:#24a98a;--err:#c25b4a;--display:'Manrope','Noto Sans SC','Segoe UI',sans-serif;--mono:'DM Mono',Consolas,monospace}
    *{box-sizing:border-box}
    body{margin:0;min-height:100vh;background:var(--paper);color:var(--ink);font-family:var(--display);line-height:1.6}
    a{color:var(--teal-dark)}
    .brand{display:flex;align-items:center;gap:11px}
    .brand-mark{display:grid;place-items:center;width:36px;height:36px;background:var(--teal);color:#fff;border-radius:10px;font-weight:800;font-size:19px;flex:none}
    .brand strong{display:block;font-size:15px;letter-spacing:-.02em}
    .brand small{display:block;color:var(--muted);font-size:11px}
    .button{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:13px 20px;border-radius:8px;border:0;font-size:13px;font-weight:700;cursor:pointer;text-decoration:none;transition:.2s;font-family:var(--display)}
    .button-primary{background:var(--teal);color:#fff}
    .button-primary:hover{background:var(--teal-dark)}
    .button-primary:disabled{opacity:.5;cursor:not-allowed}
    .button-ghost{background:transparent;border:1px solid var(--line);color:var(--ink)}
    .button-ghost:hover{border-color:var(--teal);color:var(--teal)}

    /* ===== 批量写入页 ===== */
    .batch-body{display:grid;place-items:center;padding:24px}
    .card{background:#f7faf8;border:1px solid var(--line);border-radius:14px;padding:38px;max-width:600px;width:100%;box-shadow:0 20px 50px #1f60591c}
    .card .brand{margin-bottom:26px}
    h1{font-size:23px;letter-spacing:-.05em;margin:0 0 8px}
    .sub{color:var(--muted);font-size:13px;line-height:1.8;margin:0 0 22px}
    .notice{display:none;background:#fdf3e3;border:1px solid #ecd9ad;border-radius:9px;padding:12px 14px;font-size:12px;line-height:1.7;color:#7a6428;margin:0 0 18px}
    .notice.show{display:block}
    .file-list{margin:0 0 20px;padding:0;list-style:none;border-top:1px solid var(--line)}
    .file-item{padding:13px 4px;border-bottom:1px solid var(--line)}
    .file-top{display:flex;align-items:center;gap:12px;font-size:13px}
    .file-top .idx{font:10px var(--mono);color:#9baaa5;width:22px}
    .file-top .name{flex:1;word-break:break-all}
    .file-top .st{font:10px var(--mono);color:#9baaa5;white-space:nowrap}
    .file-item.active .st{color:var(--teal-dark)}
    .file-item.done .st{color:var(--ok)}
    .file-item.fail .st{color:var(--err)}
    .bar-track{height:4px;background:#e2ebe7;border-radius:3px;margin-top:9px;overflow:hidden}
    .bar{height:100%;width:0;background:var(--teal);border-radius:3px;transition:width .2s}
    .overall{display:none;margin:0 0 20px;padding:14px 16px;background:#edf3f0;border-radius:10px}
    .overall.show{display:block}
    .overall-top{display:flex;justify-content:space-between;font:11px var(--mono);color:var(--muted);margin-bottom:8px}
    .overall .bar-track{margin-top:0}
    .actions{display:flex;gap:10px;flex-wrap:wrap}
    .empty{color:var(--muted);font-size:13px;background:#edf3f0;border-radius:9px;padding:18px;margin:0 0 22px;line-height:1.8}
    a.text{color:var(--teal-dark);font-size:12px;text-decoration:none}
    a.text:hover{color:var(--teal)}
    .back{display:block;margin-top:20px;text-align:center}

    /* ===== 主界面（文档页）===== */
    .docs-body{padding:0}
    .topbar{position:sticky;top:0;z-index:10;background:#f4f7f5e6;backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
    .topbar-inner{max-width:860px;margin:0 auto;padding:14px 28px;display:flex;align-items:center;justify-content:space-between}
    .topbar nav{display:flex;gap:22px}
    .topbar nav a{font-size:12px;color:var(--muted);text-decoration:none}
    .topbar nav a:hover{color:var(--teal)}
    .docs-wrap{max-width:860px;margin:0 auto;padding:56px 28px 90px}
    .docs-hero{margin-bottom:48px}
    .docs-hero .eyebrow{font:500 11px var(--mono);letter-spacing:.05em;color:var(--teal-dark);margin:0 0 14px}
    .docs-hero h1{font-size:clamp(30px,5vw,44px);letter-spacing:-.06em;line-height:1.15;margin:0 0 16px}
    .docs-hero h1 em{font-style:normal;color:var(--teal)}
    .docs-hero p{color:var(--muted);font-size:15px;line-height:1.9;max-width:640px;margin:0}
    .section{padding-top:52px}
    .section>h2{font-size:22px;letter-spacing:-.05em;margin:0 0 18px;display:flex;align-items:center;gap:12px}
    .section>h2 .num{font:11px var(--mono);color:var(--teal);background:#d9efe9;border-radius:6px;padding:4px 9px}
    .section p.desc{color:var(--muted);font-size:14px;line-height:1.9;margin:0 0 22px;max-width:640px}
    .panel{background:#f7faf8;border:1px solid var(--line);border-radius:12px;padding:26px}
    .builder label{display:block;font:600 12px var(--display);margin-bottom:10px}
    .builder textarea{width:100%;min-height:130px;border:1px solid var(--line);border-radius:9px;background:#fff;color:var(--ink);font:12px/1.8 var(--mono);padding:13px 15px;resize:vertical;outline:none}
    .builder textarea:focus{border-color:var(--teal)}
    .builder .hint{color:var(--muted);font-size:11px;line-height:1.8;margin:9px 0 16px}
    .builder-result{display:none;margin-top:16px;padding:16px;background:#edf3f0;border-radius:9px}
    .builder-result.show{display:block}
    .builder-result .link-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
    .builder-result code{flex:1;min-width:200px;font:11px var(--mono);background:#fff;border:1px solid var(--line);border-radius:7px;padding:10px 12px;word-break:break-all;display:block}
    .builder-result .actions{margin-top:12px;display:flex;gap:10px;flex-wrap:wrap}
    .builder-error{display:none;margin-top:14px;background:#fbeae6;border:1px solid #eac4ba;border-radius:9px;padding:11px 14px;font-size:12px;color:var(--err)}
    .builder-error.show{display:block}
    pre.code{background:#0d1716;color:#cdeae2;border-radius:10px;padding:18px 20px;font:12px/1.85 var(--mono);overflow-x:auto;margin:0 0 14px}
    pre.code .c{color:#5f8378}
    table.params{width:100%;border-collapse:collapse;font-size:13px}
    table.params th{text-align:left;font:600 11px var(--display);color:var(--muted);border-bottom:2px solid var(--line);padding:9px 12px}
    table.params td{border-bottom:1px solid var(--line);padding:11px 12px;vertical-align:top}
    table.params td:first-child{font:12px var(--mono);color:var(--teal-dark);white-space:nowrap}
    table.params tr:last-child td{border-bottom:0}
    .mode-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
    .mode-card{background:#f7faf8;border:1px solid var(--line);border-radius:10px;padding:18px}
    .mode-card b{display:block;font:11px var(--mono);color:var(--teal-dark);margin-bottom:7px;word-break:break-all}
    .mode-card h4{margin:0 0 6px;font-size:14px}
    .mode-card p{margin:0;color:var(--muted);font-size:12px;line-height:1.7}
    ul.plain{margin:0;padding-left:18px;color:var(--muted);font-size:13px;line-height:2.1}
    ul.plain b{color:var(--ink)}
    ul.plain code{font:11px var(--mono);background:#edf3f0;border-radius:5px;padding:2px 6px;color:var(--teal-dark)}
    .browser-row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
    .browser-card{background:#f7faf8;border:1px solid var(--line);border-radius:10px;padding:18px}
    .browser-card h4{margin:0 0 6px;font-size:14px}
    .browser-card p{margin:0;color:var(--muted);font-size:12px;line-height:1.75}
    .browser-card .icon{font-size:20px;display:block;margin-bottom:8px}
    .try-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:4px}
    .docs-footer{border-top:1px solid var(--line);margin-top:70px;padding-top:26px;display:flex;justify-content:space-between;flex-wrap:wrap;gap:12px;color:var(--muted);font-size:11px}
    @media(max-width:720px){.mode-grid,.browser-row{grid-template-columns:1fr}.topbar-inner{padding:12px 20px}.docs-wrap{padding:40px 20px 70px}.topbar nav{display:none}}
  </style>
</head>
<body class="<?= $urls ? 'batch-body' : 'docs-body' ?>">
<?php if ($urls): ?>
  <!-- ==================== 批量写入页 ==================== -->
  <main class="card">
    <div class="brand">
      <span class="brand-mark">J</span>
      <span><strong>Dzjs Trainer</strong><small>批量写入文件夹</small></span>
    </div>
    <h1 id="title">选择文件夹后直接写入</h1>
    <p class="sub">点击下方按钮选择一个文件夹（可新建），文件将<strong>直接写入该文件夹</strong>，无需下载对话框、无需解压。需要 Chrome / Edge 最新版。</p>
    <p class="notice" id="notice"></p>
    <p class="empty" id="empty" hidden>没有检测到文件链接。请从 <a class="text" href="./">主界面</a> 生成链接。</p>
    <ul class="file-list" id="list"></ul>
    <div class="overall" id="overall">
      <div class="overall-top"><span>总进度 <b id="overallCount">0</b>/<b id="overallTotal">0</b></span><span id="overallPercent">0%</span></div>
      <div class="bar-track"><div class="bar" id="overallBar"></div></div>
    </div>
    <div class="actions">
      <button class="button button-primary" type="button" id="start">选择文件夹并写入 <span>↓</span></button>
      <a class="button button-ghost" href="./">返回主界面</a>
    </div>
    <a class="text back" href="../index.html">← 返回 Dzjs Trainer 首页</a>
  </main>

  <script>
    const files = <?= $payload ?>;
    const list = document.getElementById('list');
    const title = document.getElementById('title');
    const empty = document.getElementById('empty');
    const notice = document.getElementById('notice');
    const start = document.getElementById('start');
    const overall = document.getElementById('overall');
    const overallBar = document.getElementById('overallBar');
    const overallCount = document.getElementById('overallCount');
    const overallPercent = document.getElementById('overallPercent');
    document.getElementById('overallTotal').textContent = files.length;

    const fileName = (url) => {
      try {
        const u = new URL(url, location.href);
        return decodeURIComponent(u.pathname.split('/').pop()) || url;
      } catch { return url.split('/').pop() || url; }
    };

    const rows = files.map((f, i) => {
      const li = document.createElement('li');
      li.className = 'file-item';
      li.innerHTML = `<div class="file-top"><span class="idx">${String(i + 1).padStart(2, '0')}</span><span class="name"></span><span class="st">等待中</span></div><div class="bar-track"><div class="bar"></div></div>`;
      li.querySelector('.name').textContent = fileName(f.url);
      list.appendChild(li);
      return { url: f.url, li, st: li.querySelector('.st'), bar: li.querySelector('.bar') };
    });

    if (!rows.length) {
      empty.hidden = false;
      title.textContent = '未指定文件';
      start.style.display = 'none';
    }

    const setState = (r, state, text) => { r.li.className = 'file-item ' + state; r.st.textContent = text; };

    // —— 单个文件：经代理流式取回并写入 dirHandle ——
    async function writeOne(dirHandle, r) {
      setState(r, 'active', '写入中');
      r.bar.style.width = '0';
      const proxyUrl = location.origin + location.pathname + '?proxy=' + encodeURIComponent(r.url);
      const response = await fetch(proxyUrl);
      if (!response.ok) throw new Error('HTTP ' + response.status);

      const totalSize = response.headers.get('X-Total-Size');
      const totalBytes = totalSize ? parseInt(totalSize, 10) : null;
      const reader = response.body.getReader();
      const chunks = [];
      let received = 0;
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;
        const pct = totalBytes ? Math.min(100, Math.round((received / totalBytes) * 100)) : null;
        r.bar.style.width = (pct !== null ? pct : 30) + '%';
        r.st.textContent = (pct !== null ? pct + '%' : '…') + ' 写入中';
      }

      const blob = new Blob(chunks);
      const fileHandle = await dirHandle.getFileHandle(fileName(r.url), { create: true });
      const writable = await fileHandle.createWritable();
      await writable.write(blob);
      await writable.close();
      r.bar.style.width = '100%';
      setState(r, 'done', '已写入 ✓');
    }

    // —— 回退：不支持文件夹选择器时逐个附件下载到「下载」文件夹 ——
    function fallbackDownload() {
      notice.textContent = '当前浏览器不支持文件夹选择器，已改为依次下载到浏览器「下载」文件夹（写入任意文件夹需要 Chrome / Edge 最新版）。';
      notice.classList.add('show');
      let i = 0;
      const step = () => {
        if (i >= rows.length) { title.textContent = '已全部触发下载 ✓'; return; }
        const r = rows[i];
        setState(r, 'active', '下载中');
        const a = document.createElement('a');
        a.href = location.pathname + files[i].dl;
        a.download = '';
        document.body.appendChild(a);
        a.click();
        a.remove();
        setState(r, 'done', '已触发');
        i += 1;
        setTimeout(step, 900);
      };
      step();
    }

    start.addEventListener('click', async () => {
      if (!rows.length) return;

      if (!window.showDirectoryPicker) { fallbackDownload(); return; }

      let dirHandle;
      try {
        dirHandle = await showDirectoryPicker({ mode: 'readwrite' });
      } catch { return; } // 用户取消选择

      start.disabled = true;
      notice.classList.remove('show');
      title.textContent = '正在写入所选文件夹…';
      overall.classList.add('show');
      overallBar.style.width = '0';
      overallCount.textContent = '0';
      overallPercent.textContent = '0%';
      rows.forEach((r) => { setState(r, '', '等待中'); r.bar.style.width = '0'; });

      let ok = 0;
      for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        try {
          await writeOne(dirHandle, r);
          ok += 1;
        } catch (e) {
          setState(r, 'fail', '失败 ✗');
          r.bar.style.width = '0';
        }
        const pct = Math.round(((i + 1) / rows.length) * 100);
        overallBar.style.width = pct + '%';
        overallCount.textContent = String(i + 1);
        overallPercent.textContent = pct + '%';
      }

      title.textContent = ok === rows.length ? '全部文件已写入 ✓' : `写入完成（成功 ${ok}/${rows.length}）`;
      start.disabled = false;
      if (ok < rows.length) {
        notice.textContent = '部分文件写入失败，可重试；若浏览器未弹出文件夹授权，请检查是否通过 HTTPS 访问。';
        notice.classList.add('show');
      }
    });
  </script>
<?php else: ?>
  <!-- ==================== 主界面：在线工具 + API 文档 ==================== -->
  <header class="topbar">
    <div class="topbar-inner">
      <div class="brand">
        <span class="brand-mark">J</span>
        <span><strong>Dzjs Trainer</strong><small>批量写入 API</small></span>
      </div>
      <nav>
        <a href="#builder">在线生成</a><a href="#api">接口参数</a><a href="#modes">工作模式</a><a href="#address">地址类型</a><a href="#browser">浏览器要求</a><a href="../index.html">返回主站</a>
      </nav>
    </div>
  </header>

  <main class="docs-wrap">
    <section class="docs-hero">
      <p class="eyebrow">BATCH WRITE API · v1</p>
      <h1>把一组文件<br><em>直接写进任意文件夹。</em></h1>
      <p>基于 File System Access API 的批量下发接口：用户打开链接、授权一个文件夹，多个文件即依次直接写入，无需下载对话框、无需解压。把下面的参数拼进链接即可分发。</p>
    </section>

    <section class="section" id="builder">
      <h2><span class="num">01</span>在线生成</h2>
      <p class="desc">每行填一个文件地址（站内相对路径或白名单内的完整 URL），生成可分享的批量写入链接。</p>
      <div class="panel builder">
        <label for="urls-input">文件地址（每行一个）</label>
        <textarea id="urls-input" placeholder="Release-v1.0.1/DzjsTrainer.exe&#10;Release-v1.0.1/其他文件.zip"></textarea>
        <p class="hint">支持站内相对路径（相对站点根，如 <code>Release-v1.0.1/DzjsTrainer.exe</code>）与白名单域名内的完整 URL；自动去重、自动忽略空行与前导空格；建议不超过 30 个。</p>
        <p class="builder-error" id="builder-error"></p>
        <div class="actions">
          <button class="button button-primary" type="button" id="build-btn">生成链接</button>
          <button class="button button-ghost" type="button" id="fill-example">填入示例</button>
        </div>
        <div class="builder-result" id="builder-result">
          <div class="link-row"><code id="builder-link"></code></div>
          <div class="actions">
            <button class="button button-primary" type="button" id="open-btn">打开并写入 <span>↓</span></button>
            <button class="button button-ghost" type="button" id="copy-btn">复制链接</button>
          </div>
        </div>
      </div>
    </section>

    <section class="section" id="api">
      <h2><span class="num">02</span>接口参数</h2>
      <p class="desc">只有一个端点：<code>GET /download/index.php</code>。参数名依次为 <code>url</code>、<code>url1</code>、<code>url2</code>…，顺序即写入顺序。</p>
      <div class="panel" style="padding:8px 6px">
        <table class="params">
          <tr><th>参数</th><th>必填</th><th>说明</th></tr>
          <tr><td>url</td><td>是</td><td>第 1 个文件地址（站内相对路径或白名单内完整 URL）</td></tr>
          <tr><td>url1 … url29</td><td>否</td><td>第 2 个及之后的文件地址，序号连续递增即可</td></tr>
          <tr><td>proxy</td><td>内部</td><td>代理流模式：流式返回单个文件，响应头 <code>X-Total-Size</code> 为字节数（批量页内部调用）</td></tr>
          <tr><td>dl</td><td>内部</td><td>附件回退模式：<code>?dl=N&amp;url=…</code> 以浏览器附件形式下载第 N 个文件</td></tr>
        </table>
      </div>
      <pre class="code"><span class="c"># 批量写入页（用户点开即用）</span>
https://dzjstrainer.xn--9kq396ceqaq4si9m.cn/download/index.php?url=Release-v1.0.1/DzjsTrainer.exe&amp;url1=path/第二个文件.zip

<span class="c"># 参数值带前导空格也能容忍，会自动忽略</span>
download/index.php?url=%20Release-v1.0.1/文件.lib&amp;url1=%20Release-v1.0.1/程序.exe</pre>
    </section>

    <section class="section" id="modes">
      <h2><span class="num">03</span>三种工作模式</h2>
      <div class="mode-grid">
        <div class="mode-card">
          <b>?url=…&amp;url1=…</b>
          <h4>批量写入页</h4>
          <p>渲染文件列表与进度条，用户选择文件夹后依次直接写入；不支持的浏览器自动回退为逐个下载。</p>
        </div>
        <div class="mode-card">
          <b>?proxy=地址</b>
          <h4>代理流</h4>
          <p>服务端流式返回文件内容（inline），供前端边下边写并显示百分比进度，外部 URL 由服务端代取。</p>
        </div>
        <div class="mode-card">
          <b>?dl=N&amp;url=…</b>
          <h4>附件直下</h4>
          <p>以 <code>Content-Disposition: attachment</code> 附件响应写出单个文件，作为旧浏览器的回退通道。</p>
        </div>
      </div>
    </section>

    <section class="section" id="address">
      <h2><span class="num">04</span>支持的地址类型</h2>
      <div class="panel">
        <ul class="plain">
          <li><b>站内相对路径</b> — 相对站点根目录解析，如 <code>Release-v1.0.1/DzjsTrainer.exe</code>；服务端做 <code>realpath</code> 越界检查，<code>../</code> 逃逸会被拒绝（404）。</li>
          <li><b>外部完整 URL</b> — <?= $ALLOW_REMOTE
              ? '服务端代理拉取，仅放行白名单域名' . ($ALLOW_HOSTS ? '：' . htmlspecialchars(implode('、', $ALLOW_HOSTS), ENT_QUOTES, 'UTF-8') : '（当前白名单为空，如需代理外链请在配置中添加）') . '。'
              : '当前已关闭外链代理，仅支持站内相对路径。' ?></li>
          <li><b>重复与空值</b> — 相同地址自动去重；空参数与前导空格（如 <code>url1=%20…</code>）自动忽略。</li>
        </ul>
      </div>
    </section>

    <section class="section" id="browser">
      <h2><span class="num">05</span>浏览器要求</h2>
      <div class="browser-row">
        <div class="browser-card">
          <span class="icon">✓</span>
          <h4>Chrome / Edge 86+</h4>
          <p>完整体验：弹出文件夹选择器，文件直接写入用户选择的任意文件夹（可新建），带逐文件与总进度。需要 HTTPS 环境。</p>
        </div>
        <div class="browser-card">
          <span class="icon">◐</span>
          <h4>Firefox / Safari 等</h4>
          <p>自动回退：文件依次以附件形式下载到浏览器「下载」文件夹，功能等价，只是无法自选目标文件夹。</p>
        </div>
      </div>
      <div class="try-row" style="margin-top:22px">
        <a class="button button-primary" href="?url=Release-v1.0.0/DzjsTrainer.exe">试一试：写入 DzjsTrainer.exe <span>↓</span></a>
        <a class="button button-ghost" href="../index.html#download">返回主站下载区</a>
      </div>
    </section>

    <footer class="docs-footer">
      <span>Dzjs Trainer · 批量写入 API</span>
      <span>MIT License · <?= date('Y') ?></span>
    </footer>
  </main>

  <script>
    const input = document.getElementById('urls-input');
    const buildBtn = document.getElementById('build-btn');
    const result = document.getElementById('builder-result');
    const linkEl = document.getElementById('builder-link');
    const errorEl = document.getElementById('builder-error');
    let builtHref = null;

    const parseUrls = () => input.value
      .split(/\r?\n/)
      .map((l) => l.trim())
      .filter(Boolean)
      .filter((u, i, arr) => arr.indexOf(u) === i);

    const showError = (msg) => { errorEl.textContent = msg; errorEl.classList.add('show'); };
    const hideError = () => errorEl.classList.remove('show');

    const build = () => {
      hideError();
      result.classList.remove('show');
      const urls = parseUrls();
      if (!urls.length) { showError('请先填写至少一个文件地址（每行一个）。'); return; }
      if (urls.length > 30) { showError('一次最多 30 个文件，请分批生成。'); return; }
      const query = urls.map((u, i) => (i === 0 ? 'url=' : 'url' + i + '=') + encodeURIComponent(u)).join('&');
      builtHref = location.pathname + '?' + query;
      linkEl.textContent = location.origin + builtHref;
      result.classList.add('show');
    };

    buildBtn.addEventListener('click', build);
    document.getElementById('open-btn').addEventListener('click', () => { if (builtHref) location.href = builtHref; });
    document.getElementById('copy-btn').addEventListener('click', async (e) => {
      if (!builtHref) return;
      try {
        await navigator.clipboard.writeText(location.origin + builtHref);
        e.target.textContent = '已复制 ✓';
        setTimeout(() => { e.target.textContent = '复制链接'; }, 1600);
      } catch { showError('复制失败，请手动选中链接复制。'); }
    });
    document.getElementById('fill-example').addEventListener('click', () => {
      input.value = 'Release-v1.0.0/DzjsTrainer.exe';
      hideError();
      result.classList.remove('show');
    });
  </script>
<?php endif; ?>
</body>
</html>
