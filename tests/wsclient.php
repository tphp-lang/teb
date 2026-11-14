<?php
// 极简 WebSocket 客户端（RFC6455 子集）：握手 + masked 帧收发 + 控制帧/分片
// 用法：WS_PORT=18165 WS_PATH=/ws WS_MSG=hi [WS_PING=1] [WS_FRAG=1] [WS_CLOSE=close]
//       tphp run tests/wsclient.php
// 输出（每帧一行，供 run_p1.sh grep 断言）：
//   HANDSHAKE=<code> ACCEPT=<0|1>
//   RECV=<text>            首帧（onOpen 发来）
//   RECV2=<text>           次帧（onMessage 回显）
//   CLOSE_EARLY=<text>     次帧即服务端 CLOSE 帧（payload 原文）
//   PONG_OP=<10> PONG_DATA=<text>     PING 自动应答
//   RECV3=<text>           分片装配回显
//   CLOSE_FRAME=<0|1> CLOSE_DATA=<text>   客户端 CLOSE 的服务端回应
//
// 注意：引擎 ^ 对字符串返回 int（数值 XOR），掩码必须走 chr(ord()^ord())。

$host = "127.0.0.1";
$port = (int)(getenv("WS_PORT") ? getenv("WS_PORT") : "0");
$path = "/" . ltrim(getenv("WS_PATH") ? getenv("WS_PATH") : "ws", "/");   // MSYS 环境变量路径转换规避：环境变量传无前导斜杠值
$msg  = getenv("WS_MSG") ? getenv("WS_MSG") : "ping";
$doPing = getenv("WS_PING") === "1";
$doFrag = getenv("WS_FRAG") === "1";
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
$code = 0;
$m = [];
if (preg_match("/HTTP\/1\.[01] (\d\d\d)/", $resp, $m)) $code = (int)$m[1];
$hasAccept = (strpos($resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !== false) ? 1 : 0;
echo "HANDSHAKE=" . $code . " ACCEPT=" . $hasAccept . "\n";
if ($code !== 101) { fclose($fp); exit(1); }

// 发 masked 帧（客户端必须掩码）：opcodeByte 含 FIN|opcode
function wsSendFrame($fp, string $opcodeByte, string $payload): void {
    $len = strlen($payload);
    $maskKey = "\x11\x22\x33\x44";
    $masked = "";
    for ($i = 0; $i < $len; $i++) {
        $masked = $masked . chr(ord($payload[$i]) ^ ord($maskKey[$i % 4]));
    }
    $head = $opcodeByte;
    if ($len < 126) {
        $head = $head . chr(0x80 | $len);
    } elseif ($len < 65536) {
        $head = $head . chr(0x80 | 126) . pack("n", $len);
    } else {
        $head = $head . chr(0x80 | 127) . pack("J", $len);
    }
    fwrite($fp, $head . $maskKey . $masked);
}

// 读一帧（服务端帧不掩码）：byte[0]=FIN|opcode，byte[1]=len
function wsRecvFrame($fp): array {
    $b = fread($fp, 2);
    if (strlen($b) < 2) return ["op" => -1, "data" => ""];
    $op = ord($b[0]) & 0x0F;
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

wsSendFrame($fp, "\x81", $msg);           // FIN TEXT
$f1 = wsRecvFrame($fp);                   // onOpen 发来
echo "RECV=" . $f1["data"] . "\n";

$f2 = wsRecvFrame($fp);                   // 回显 或 服务端 CLOSE
if ($f2["op"] === 8) {
    echo "CLOSE_EARLY=" . $f2["data"] . "\n";
    fclose($fp);
    echo "CLI-DONE\n";
    exit(0);   // 服务端已关，后续写必失败
}
echo "RECV2=" . $f2["data"] . "\n";

if ($doPing) {
    wsSendFrame($fp, "\x89", "pp");       // masked PING
    $f3 = wsRecvFrame($fp);
    echo "PONG_OP=" . $f3["op"] . " PONG_DATA=" . $f3["data"] . "\n";
}

if ($doFrag) {
    wsSendFrame($fp, "\x01", "hel");      // TEXT FIN=0
    wsSendFrame($fp, "\x00", "lo-");      // CONT FIN=0
    wsSendFrame($fp, "\x80", "frag");     // CONT FIN=1
    $f4 = wsRecvFrame($fp);
    echo "RECV3=" . $f4["data"] . "\n";
}

if ($closeAfter) {
    // 发 CLOSE 帧（masked，带 payload）→ 服务端应回 CLOSE 帧后断开
    wsSendFrame($fp, "\x88", "client-bye");
    $f5 = wsRecvFrame($fp);
    echo "CLOSE_FRAME=" . ($f5["op"] === 8 ? "1" : "0")
       . " CLOSE_DATA=" . $f5["data"] . "\n";
}
fclose($fp);
echo "CLI-DONE\n";
