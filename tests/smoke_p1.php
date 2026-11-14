<?php
// tphp/teb P1 冒烟测试 —— before / 静态文件 / stats / WebSocket
// 由 tests/run_p1.sh 驱动。端口 18165（WS 客户端亦连此端口）。
//
// 时间线：
//   窗口 1（22s，drain 泵）：before/静态/stats 断言 + WS 客户端回显/close
//   自然退出 → p1-done

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;
use Tphp\Teb\WebSocket;

$__ws_closed = 0;

function register_routes($web) {
    $web->route("/ping", fn(Request $r) => new Response("pong"));

    // ---- stats 采样点 ----
    $web->route("/st-200", fn(Request $r) => new Response("ok-200"));
    $web->route("/st-201", fn(Request $r) => new Response("made", 201));

    // ---- before 顺序断言（global trace：b-order 命中时 b1→b2） ----
    $web->route("/b-order", function (Request $r): Response {
        global $__order;
        return new Response("trace=" . $__order);
    });
    $web->route("/b-shortcut", fn(Request $r) => new Response("NEVER-SEE"));
    $web->route("/b-throw", fn(Request $r) => new Response("NEVER-SEE"));
    $web->route("/b-static-blocked", fn(Request $r) => new Response("NEVER-SEE"));

    // ---- before 内流式 ----
    $web->route("/b-stream", function (Request $r) {
        $s = $r->beginStream();
        $s->write("stream-via-before");
        $s->end();
        return null;
    });

    // ---- WebSocket 回显 ----
    $web->websocket("/ws",
        function (WebSocket $s, Request $r) {
            $s->send("hello:" . $r->remoteAddr());
        },
        function (WebSocket $s, string $data, int $bits) {
            if ($data === "close-me") {
                $s->close("bye");
                return;
            }
            $s->send("echo:" . $data);
        },
        function (WebSocket $s) {
            global $__ws_closed;
            $__ws_closed = $__ws_closed + 1;
            echo "ws-close-marker\n";
        });
}

$web = new Web();
register_routes($web);

// before 链（按注册序执行）
$web->before(function (Request $r) {
    global $__order;
    if ($r->path() === "/b-order") {
        $__order = "1";
    }
    return null;
});
$web->before(function (Request $r) {
    global $__order;
    if ($r->path() === "/b-order") {
        $__order = $__order . "2";
    }
    return null;
});
// 短路 before：/b-shortcut 直接拦截（202 + 自定义头，路由不执行）
$web->before(function (Request $r) {
    if ($r->path() === "/b-shortcut") {
        return new Response("short-circuited", 202, ["X-Short" => "yes"]);
    }
    return null;
});
// 异常 before：/b-throw 抛出 → 500
$web->before(function (Request $r) {
    if ($r->path() === "/b-throw") {
        throw new Error("boom-before");
    }
    return null;
});
// 拦静态 before：/assets/block.txt 直接 403（静态分发前短路）
$web->before(function (Request $r) {
    if ($r->path() === "/assets/block.txt") {
        return new Response("blocked-by-before", 403);
    }
    return null;
});

// 静态目录：tests/_p1_static 映射到 /assets
$web->file("/assets", __DIR__ . "/_p1_static");

$web->start([
    "listening_ports" => "18165",
    "num_threads" => "8",
    "upload_max_bytes" => "200000",
]);

$t0 = time();
while (time() - $t0 < 22) { $web->drain(); }

$web->stop();
echo "STATS=" . json_encode($web->stats()) . "\n";
echo "p1-done\n";
