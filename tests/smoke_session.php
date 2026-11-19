<?php
// tphp/teb 冒烟测试 —— Session + Response::cookie opts
// 由 tests/run_session.sh 驱动（自动起停进程与 curl 断言），不要直接长驻运行。
//
// start 自留键：session_save_path=tests/_sess、session_lifetime=5（过期
// 用例 sleep 6 验证）、session_gc_interval=1（GC 用例 1s 节流可触发）。

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// before：仅特定路径开 session（全局开会破坏 /s/nosess 的惰性断言）
$web->before(function (Request $r) {
    $seg = explode("/", $r->path());
    $head = $seg[1] ?? "";
    if ($head === "s" && ($seg[2] ?? "") === "before") {
        $r->session()->set("via", "before");
        return new Response("before-short=" . $r->session()->get("via"));
    }
    if ($head === "assets") {
        $r->session();   // 静态路径经 before 打开会话 → 注入用例
    }
    return null;
});

// 就绪探测（不开 session）
$web->route("/ok", fn(Request $r) => new Response("ok"));

$web->route("/s/set", function (Request $r): Response {
    $s = $r->session();
    $s->set("user", "alice");
    $s->set("n", (int)$s->get("n", 0) + 1);
    return new Response("id=" . $s->id() . " n=" . $s->get("n"));
});

$web->route("/s/get", function (Request $r): Response {
    $s = $r->session();
    return new Response("user=" . $s->get("user", "-") . " n=" . $s->get("n", "-"));
});

$web->route("/s/inc", function (Request $r): Response {
    $s = $r->session();
    $s->set("n", (int)$s->get("n", 0) + 1);
    return new Response("n=" . $s->get("n"));
});

$web->route("/s/id", fn(Request $r) => new Response("id=" . $r->session()->id()));

$web->route("/s/regen", function (Request $r): Response {
    $s = $r->session();
    $old = $s->id();
    $s->regenerateId();
    return new Response("old=" . $old . " new=" . $s->id() .
                        " user=" . $s->get("user", "-"));
});

$web->route("/s/logout", function (Request $r): Response {
    $r->session()->destroy();
    return new Response("destroyed");
});

// set 后抛异常 → 500 仍带 Set-Cookie 且数据已落盘（异常路径 flush）
$web->route("/s/throw", function (Request $r): Response {
    $r->session()->set("after_throw", "yes");
    throw new Error("boom-session");
});

$web->route("/s/throw-chk", function (Request $r): Response {
    return new Response("after_throw=" . $r->session()->get("after_throw", "-"));
});

// 流式·先 session 后 write → 响应头含 Set-Cookie
$web->route("/s/stream", function (Request $r) {
    $r->session()->set("streamed", "1");
    $st = $r->beginStream(200, ["Content-Type" => "text/plain"]);
    $st->write("stream-data");
    $st->end();
});

// 流式·先 write 后 session → 头已发，本响应无 Set-Cookie（文档限制）
$web->route("/s/stream-late", function (Request $r) {
    $st = $r->beginStream(200, ["Content-Type" => "text/plain"]);
    $st->write("late");
    $r->session();
    $st->end();
});

// Response::cookie 三参 opts 全字段
$web->route("/s/cookie-opts", function (Request $r): Response {
    return new Response("opts")
        ->cookie("ck", "cv", [
            "path" => "/app", "domain" => "ex.com", "max_age" => 60,
            "secure" => true, "httponly" => true, "sameSite" => "Strict",
        ]);
});

// 非法 opts → ValueError 可捕获（非 500）
$web->route("/s/cookie-bad", function (Request $r): Response {
    try {
        new Response("x")->cookie("k", "v", ["httplnly" => true]);
        return new Response("no-raise");
    } catch (ValueError $e) {
        return new Response("caught=ValueError");
    } catch (Throwable $e) {
        return new Response("caught=" . get_class($e));
    }
});

// 不碰会话 → 无 Set-Cookie（惰性保证）
$web->route("/s/nosess", fn(Request $r) => new Response("plain"));

$web->file("/assets", __DIR__ . "/_sess_assets");

$web->start([
    "listening_ports" => "18126",
    "num_threads" => "8",
    "session_save_path" => __DIR__ . "/_sess",
    "session_lifetime" => "5",
    "session_gc_interval" => "1",
]);

$t0 = time();
while (time() - $t0 < 30) { $web->drain(); }
$web->stop();
echo "session-phase-done\n";
