<?php
// tphp/teb P0 冒烟测试 —— 流式 / 上传 / notFound / 工厂 / 解析助手 / 安全 stop
// 由 tests/run_p0.sh 驱动。端口 18125。
//
// 时间线：
//   窗口 1（25s，drain 泵）：P0 功能断言（流式/上传/工厂/助手/nf）
//   phase1-done → stop → 同进程重启 → run() 常驻
//   /stop-in-callback：回调内 stop() → pending_stop → 批末真停 → run() 退出
//   run-exited + 两次幂等 stop → p0-done

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

function register_routes($web) {
    // 就绪探测端点（供驱动 wait_ready 用；只读无副作用）
    $web->route("/ping", fn(Request $r) => new Response("pong"));

    // ---- 流式 ----
    $web->route("/stream-chunks", function (Request $r) {
        $s = $r->beginStream(200, ["Content-Type" => "text/plain"]);
        $s->write("a");
        usleep(200000);
        $s->write("b");
        usleep(200000);
        $s->write("c");
        $s->end();
        return null;
    });

    $web->route("/stream-auto", function (Request $r) {
        $s = $r->beginStream();
        $s->write("xyz");
        return null;   // 不 end → drain 自动收尾
    });

    $web->route("/stream-throw-after", function (Request $r) {
        $s = $r->beginStream();
        $s->write("visible");
        throw new Error("boom-after-header");
    });

    $web->route("/stream-throw-before", function (Request $r) {
        $s = $r->beginStream();
        throw new Error("boom-before-header");
    });

    $web->route("/stream-status-header", function (Request $r) {
        $s = $r->beginStream(206, ["X-S" => "s1"]);
        $s->status(207);
        $s->header("X-S2", "s2");
        $s->write("part");
        $s->end();
        return null;
    });

    // ---- 上传 ----
    $web->route("/upload", function (Request $r): Response {
        $form = $r->form();
        $up = $r->file("up");
        $dest = __DIR__ . "/_up_moved.bin";
        $moved = $up->moveTo($dest);
        return new Response(
            "form-name=" . $form["name"] .
            "|up-name=" . $up->name() .
            "|up-size=" . $up->size() .
            "|up-error=" . $up->error() .
            "|moved=" . ($moved ? "true" : "false") .
            "|tmp-after=" . $up->tmpName()
        );
    });

    $web->route("/upload-tmp1", function (Request $r): Response {
        $fs = $r->files();
        $f = $fs["f2"];
        return new Response("tmp=" . $f->tmpName() . "|size=" . $f->size());
    });

    $web->route("/upload-tmp2", function (Request $r): Response {
        $prev = $r->queryParam("prev", "");
        return new Response("exists=" . (file_exists($prev) ? "yes" : "no"));
    });

    $web->route("/upload-limit", function (Request $r): Response {
        $f = $r->file("big");
        return new Response(
            "err=" . $f->error() .
            "|tmp=" . $f->tmpName() .
            "|size=" . $f->size() .
            "|name=" . $f->name()
        );
    });

    // ---- notFound 命中（nf 在外层注册） ----

    // ---- 工厂 ----
    $web->route("/f-json", fn(Request $r) =>
        Response::json(["a" => 1, "b" => "x"]));
    $web->route("/f-text", fn(Request $r) => Response::text("plain-here"));
    $web->route("/f-html", fn(Request $r) => Response::html("<p>h</p>"));
    $web->route("/f-redirect", fn(Request $r) => Response::redirect("/there"));
    $web->route("/f-json-bad", function (Request $r): Response {
        return Response::json(new Response("unencodable"));
    });

    // ---- 解析助手 ----
    $web->route("/assist", function (Request $r): Response {
        $qp = $r->queryParams();
        return new Response(
            "a=" . $qp["a"] .
            "|b=" . $qp["b"] .
            "|c=" . ($qp["c"] === "" ? "empty" : "other") .
            "|b2=" . $r->queryParam("b") .
            "|z=" . $r->queryParam("z", "dft")
        );
    });

    $web->route("/assist-cookie", function (Request $r): Response {
        return new Response(
            "sid=" . $r->cookie("sid") .
            "|theme=" . $r->cookie("theme") .
            "|nope=" . $r->cookie("nope", "dft2")
        );
    });

    $web->route("/assist-json", function (Request $r): Response {
        $j = $r->json();
        $j2 = $r->json();   // body 缓存幂等
        $ok = ($j2["x"] === $j["x"]) ? "same" : "diff";
        return new Response(
            "x=" . $j["x"] . "|y=" . count($j["y"]) . "|again=" . $ok .
            "|isjson=" . ($r->isJson() ? "yes" : "no")
        );
    });

    $web->route("/assist-body", function (Request $r): Response {
        $b1 = $r->body();
        $b2 = $r->body();   // 缓存幂等
        return new Response(
            "b1=" . $b1 . "|same=" . ($b1 === $b2 ? "yes" : "no") .
            "|len=" . $r->contentLength()
        );
    });

    $web->route("/assist-ip", function (Request $r): Response {
        return new Response("ip=" . $r->clientIp());
    });

    // POST urlencoded：form() 与 body() 共用缓存
    $web->route("/assist-form-body", function (Request $r): Response {
        $form = $r->form();
        $b = $r->body();
        return new Response("k1=" . $form["k1"] . "|body=" . $b);
    });

    // ---- 回调内 stop（phase 2） ----
    $web->route("/stop-in-callback", function (Request $r) use ($web): Response {
        $web->stop();   // draining 中 → pending_stop，批末真停
        return new Response("stopping");
    });
}

$web = new Web();
register_routes($web);
$web->notFound(function (Request $r): Response {
    return new Response("nf:" . $r->uri());
});

$web->start([
    "listening_ports" => "18125",
    "num_threads" => "8",
    "upload_max_bytes" => "200",
]);

// 窗口 1：P0 功能断言
$t0 = time();
while (time() - $t0 < 25) { $web->drain(); }
echo "phase1-done\n";

// stop → 同进程重启（nf/routes 均被清空，需重注册）
$web->stop();
register_routes($web);
$web->notFound(function (Request $r): Response {
    return new Response("nf:" . $r->uri());
});
$web->start([
    "listening_ports" => "18125",
    "num_threads" => "8",
    "upload_max_bytes" => "200",
]);
echo "phase2-started\n";

// 常驻：/stop-in-callback 触发 pending_stop → 批末 real_stop → run 退出
$web->run();
echo "run-exited\n";

// stop 幂等（ctx 已空 → no-op）
$web->stop();
$web->stop();
echo "p0-done\n";
