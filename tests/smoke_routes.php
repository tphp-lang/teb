<?php
// tphp/teb 冒烟测试 —— 阶段 1：路由 API 全覆盖
// 由 tests/run.sh 驱动（自动起停进程与 curl 断言），不要直接长驻运行。
//
// 时间线：
//   窗口 1（20s）：run.sh 跑完全部路由断言
//   stop() 连调两次（幂等）→ 同进程同端口重启（验证 stop 后可再 start）
//   窗口 2（15s）：run.sh 断言重启后 /ok 仍 200，随后 kill

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

// 全部路由注册（stop() 释放路由表，重启后需重新注册 —— 契约见 doc/README.md）
function register_routes($web) {
    // 就绪探测 + 重启验证端点
    $web->route("/ok", fn(Request $r) => new Response("ok"));

    // 精确匹配
    $web->route("/", fn(Request $r) => new Response("hello world"));

    // 前缀匹配 + remoteAddr/remotePort
    $web->route("/hello", function (Request $r): Response {
        return new Response("ip=" . $r->remoteAddr() . " port=" . $r->remotePort());
    });

    // 请求信息：method/uri/path/query/httpVersion/isSsl
    $web->route("/info", function (Request $r): Response {
        $ssl = $r->isSsl() ? "ssl" : "plain";
        return new Response(
            $r->method() . "|" . $r->uri() . "|" . $r->path() . "|" .
            $r->query() . "|" . $r->httpVersion() . "|" . $ssl
        );
    });

    // POST body 回显（body() 读取）
    $web->route("/echo", function (Request $r): Response {
        return new Response("m=" . $r->method() . " body=" . $r->body());
    });

    // header() 大小写不敏感 + 不存在返回 null + headers() 计数
    $web->route("/hdr", function (Request $r): Response {
        $lo = $r->header("x-test-header");
        $up = $r->header("X-TEST-HEADER");
        $miss = $r->header("X-Not-Exist");
        $cnt = count($r->headers());
        $missTxt = $miss === null ? "null" : "set";
        return new Response("lo=" . $lo . " up=" . $up . " miss=" . $missTxt . " n=" . $cnt);
    });

    // 链式 Response：status/header/cookie/withHeaders（覆盖默认 Content-Type）
    $web->route("/chain", function (Request $r): Response {
        return (new Response("chained", 200))
            ->status(201)
            ->header("X-Req-Id", "r1")
            ->cookie("sid", "s1")
            ->withHeaders(["X-Extra" => "e1", "Content-Type" => "text/x-chain"]);
    });

    // Response::body() 读取自检
    $web->route("/respbody", function (Request $r): Response {
        $resp = new Response("abc-def", 200);
        $read = $resp->body();
        $ok = $read === "abc-def" ? "body-ok" : "body-bad";
        return new Response($ok);
    });

    // 回调返回非 Response → 500
    $web->route("/nonresp", fn(Request $r) => "not-a-response");

    // 回调抛异常 → 500
    $web->route("/throw", function (Request $r): Response {
        throw new Error("boom-smoke");
    });

    // Request 手动构造必须被拒
    $web->route("/reqnew", function (Request $r): Response {
        try {
            new Request();
            return new Response("req-ctor-allowed");
        } catch (Error $e) {
            return new Response("req-ctor-blocked");
        }
    });

    // 匹配优先级：/fb 为前缀路由，/fb/longest-route-here 为精确路由
    $web->route("/fb", fn(Request $r) => new Response("fb-short"));
    $web->route("/fb/longest-route-here", fn(Request $r) => new Response("fb-longest"));
}

$web = new Web();
register_routes($web);

$web->start([
    "listening_ports" => "18123",
    "num_threads" => "8",
]);

// 窗口 1：驱动侧跑完全部断言
$t0 = time();
while (time() - $t0 < 20) { $web->drain(); }

// stop 幂等（第二次 ctx 已空，应静默返回）+ 同进程同端口重启
$web->stop();
$web->stop();
register_routes($web);   // stop 已释放路由表，重启前重新注册
$web->start(["listening_ports" => "18123", "num_threads" => "8"]);
echo "restart-ok idempotent-stop-ok\n";

// 窗口 2：驱动侧断言重启后仍可服务
$t0 = time();
while (time() - $t0 < 15) { $web->drain(); }
$web->stop();
echo "phase1-done\n";
