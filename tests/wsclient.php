<?php
// 极简 WebSocket 客户端（RFC6455 子集）：握手 + 发 masked 文本帧 + 读一帧
// 用法：WS_PORT=18165 WS_PATH=/ws WS_MSG=hi WS_CLOSE=close tphp run tests/wsclient.php
// 输出（JSON 风格行）：HANDSHAKE=<code> RECV=<text> CLOSE_FRAME=<0|1>

// 参数经环境变量（引擎无 $argv）：WS_PORT / WS_PATH / WS_MSG / WS_CLOSE
$host = "127.0.0.1";
$port = (int)(getenv("WS_PORT") ? getenv("WS_PORT") : "0");
$path = "/" . ltrim(getenv("WS_PATH") ? getenv("WS_PATH") : "ws", "/");   // MSYS 环境变量路径转换规避：环境变量传无前导斜杠值
$msg  = getenv("WS_MSG") ? getenv("WS_MSG") : "ping";
$closeAfter = (getenv("WS_CLOSE") === "close") ? true : false;
if ($port <= 0) {
    echo "HANDSHAKE=0 RECV= NO_PORT\n";
    exit(1);
}

$errno = 0;
$errstr = "";
$fp = fsockopen($host, $port, $errno, $errstr, 3);
if (!$fp) {
    echo "HANDSHAKE=0 RECV= CONNECT_FAIL\n";
    exit(1);
}

// 握手（Sec-WebSocket-Key 固定样例 → 服务端应回 101 + Accept）
$key = "dGhlIHNhbXBsZSBub25jZQ==";
$req = "GET " . $path . " HTTP/1.1\r\n"
     . "Host: " . $host . "\r\n"
     . "Upgrade: websocket\r\n"
     . "Connection: Upgrade\r\n"
     . "Sec-WebSocket-Key: " . $key . "\r\n"
     . "Sec-WebSocket-Version: 13\r\n\r\n";
fwrite($fp, $req);

// 读握手响应头（至空行）
$resp = "";
while (($line = fgets($fp)) !== false) {
    $resp = $resp . $line;
    if ($line === "\r\n" || $line === "\n") break;
}
echo "RESP-LEN=" . strlen($resp) . "\n";
echo "RESP=" . str_replace("\r", "", str_replace("\n", "|", $resp)) . "\n";
$code = 0;
$m = [];
if (preg_match("/HTTP\/1\.[01] (\d\d\d)/", $resp, $m)) $code = (int)$m[1];
$hasAccept = (strpos($resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !== false) ? 1 : 0;
echo "HANDSHAKE=" . $code . " ACCEPT=" . $hasAccept . "\n";
if ($code !== 101) { fclose($fp); exit(1); }

// 发 masked 文本帧（客户端必须掩码）
function wsSendMasked($fp, string $payload): void {
    $len = strlen($payload);
    $maskKey = "\x11\x22\x33\x44";
    $masked = "";
    for ($i = 0; $i < $len; $i++) {
        $masked = $masked . ($payload[$i] ^ $maskKey[$i % 4]);
    }
    $head = "\x81";   // FIN + TEXT
    if ($len < 126) {
        $head = $head . chr(0x80 | $len);
    } elseif ($len < 65536) {
        $head = $head . chr(0x80 | 126) . pack("n", $len);
    } else {
        $head = $head . chr(0x80 | 127) . pack("J", $len);
    }
    fwrite($fp, $head . $maskKey . $masked);
}

// 读一帧（服务端帧不掩码）：FIN/opcode 头 + len + payload
function wsRecvFrame($fp): array {
    $b = fread($fp, 2);
    if (strlen($b) < 2) return ["op" => -1, "data" => ""];
    $op = ord($b[1]) & 0x0F;
    $ln = ord($b[1]) & 0x7F;
    if ($ln === 126) {
        $e = fread($fp, 2);
        $ln = (int)unpack("n", $e)[1];
    } elseif ($ln === 127) {
        $e = fread($fp, 8);
        $ln = (int)unpack("J", $e)[1];
    }
    $data = $ln > 0 ? fread($fp, $ln) : "";
    return ["op" => $op, "data" => $data];
}

wsSendMasked($fp, $msg);
$f1 = wsRecvFrame($fp);
echo "RECV=" . $f1["data"] . "\n";

if ($closeAfter) {
    // 发 CLOSE 帧（masked，空载荷）
    fwrite($fp, "\x88" . chr(0x80) . "\x11\x22\x33\x44");
    $f2 = wsRecvFrame($fp);
    echo "CLOSE_FRAME=" . ($f2["op"] === 8 ? "1" : "0") . "\n";
}
fclose($fp);
