<?php
// Seeed MR60BHA2 Bridge API
// GET  ?action=status      → last status as JSON
// POST ?action=push&key=K  → receive status from bridge

define('API_KEY',     'mr60_bridge_2026');
define('DATA_DIR',    __DIR__ . '/data/');
define('STATUS_FILE', DATA_DIR . 'status.json');

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');

$action = $_GET['action'] ?? $_POST['action'] ?? 'status';
$key    = $_GET['key'] ?? $_POST['key'] ?? '';

if ($action === 'status') {
    if (!file_exists(STATUS_FILE)) {
        echo json_encode(['error' => 'no data yet']);
        exit;
    }
    echo file_get_contents(STATUS_FILE);
    exit;
}

if ($action === 'push') {
    if ($key !== API_KEY) { http_response_code(403); echo json_encode(['error'=>'forbidden']); exit; }
    $body = file_get_contents('php://input');
    $d = json_decode($body, true);
    if (!$d) { http_response_code(400); echo json_encode(['error'=>'bad json']); exit; }
    $d['_received'] = date('c');
    file_put_contents(STATUS_FILE, json_encode($d));
    echo json_encode(['ok' => true]);
    exit;
}

http_response_code(400);
echo json_encode(['error' => 'unknown action']);
