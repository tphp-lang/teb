<?php
// WebSocket 聊天室：连接注册表 + 广播 + ping/close 指令
// 运行：tphp run example/07-websocket.php
// 试（浏览器控制台）：
//   const ws = new WebSocket("ws://127.0.0.1:8087/ws");
//   ws.onmessage = e => console.log(e.data);
//   ws.onopen = () => ws.send("hello");
//   ws.send("ping");
// 或用命令行客户端（仓库自带）：
//   WS_PORT=8087 WS_PATH=ws WS_MSG=hello tphp run tests/wsclient.php
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;
use Tphp\Teb\WebSocket;

// 连接注册表：remoteAddr => WebSocket
// 注意：引擎不支持闭包 use (&$x) 引用捕获，跨回调共享状态用 global
$rooms = [];

// 广播：逐个发送；对端已断开时 send 会抛异常 -> 顺手清理死连接
function broadcast($from, $text) {
    global $rooms;
    foreach ($rooms as $addr => $ws) {
        try {
            $ws->send("[" . $from . "] " . $text);
        } catch (Error $e) {
            unset($rooms[$addr]);
        }
    }
}

$web = new Web();

$web->websocket(
    "/ws",
    // onOpen：握手完成（此时还没读到任何客户端数据）
    function (WebSocket $s, Request $r) {
        global $rooms;
        $rooms[$r->remoteAddr()] = $s;
        broadcast("system", $r->remoteAddr() . " joined");
    },
    // onMessage：每条完整消息一次；$bits 是消息 opcode（1=文本 2=二进制）
    function (WebSocket $s, string $data, int $bits) {
        if ($data === "ping") {        // 应用层心跳，直接回
            $s->send("pong");
            return;
        }
        if ($data === "close") {       // 服务端主动断开（会触发 onClose）
            $s->close("server-bye");
            return;
        }
        broadcast($s->remoteAddr(), $data . " (bits=" . $bits . ")");
    },
    // onClose：连接已断；死连接由广播失败的 catch 逐个剔除
    function (WebSocket $s) {
        global $rooms;
        echo "ws closed, online=" . count($rooms) . "\n";
    }
);

// HTTP 路由与 WS 可共用同一端口
$web->route("/", function (Request $r) {
    global $rooms;
    return new Response("ws server online: " . count($rooms) . "\n");
});

echo "07-websocket listening on ws://127.0.0.1:8087/ws\n";
$web->start(["listening_ports" => "8087"]);
$web->run();
