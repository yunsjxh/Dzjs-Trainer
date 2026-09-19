<?php
declare(strict_types=1);

// Place this file beside manifest.json and expose the releases directory.
$root = __DIR__;
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
    $decoded = json_decode((string)file_get_contents($manifestPath), true);
    if (is_array($decoded)) {
        $manifest = array_merge($manifest, $decoded);
    }
}

header('Cache-Control: no-store, no-cache, must-revalidate');
$action = (string)($_GET['action'] ?? '');
if (isset($_GET['manifest']) || $action === 'manifest') {
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($manifest, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}
if (isset($_GET['checkupdate'])) {
    header('Content-Type: text/plain; charset=utf-8');
    $client = (string)$_GET['checkupdate'];
    echo version_compare($client, (string)$manifest['version'], '<') ? 'newupdate' : 'latest';
    exit;
}
if (isset($_GET['getupdateinfo'])) {
    header('Content-Type: text/plain; charset=utf-8');
    echo (string)$manifest['notes'];
    exit;
}
if (isset($_GET['getnewver'])) {
    header('Content-Type: text/plain; charset=utf-8');
    echo (string)$manifest['version'];
    exit;
}
if (isset($_GET['getupdate'])) {
    header('Content-Type: text/plain; charset=utf-8');
    echo (string)$manifest['url'];
    exit;
}

header('Content-Type: text/plain; charset=utf-8', true, 400);
echo 'missing action';
