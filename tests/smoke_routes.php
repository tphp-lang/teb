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

// 全部路由注册（stop() 释放路由表，重启后需重新注册 —— 契约见 doc/API.md）
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

    // ---- body 上限（upload_max_bytes=1024，见 start options）----
    // 声明 Content-Length 超限 → 入口 413，本回调不应执行
    $web->route("/limit-cl", fn(Request $r) => new Response("limit-cl-reached"));

    // chunked（未声明长度）读取超限 → body() 抛 Error，可捕获且重复抛
    $web->route("/limit-ch", function (Request $r) {
        try {
            $r->body();
        } catch (Error $e) {
            try {
                $r->body();   // 第二次：重复抛同一异常
            } catch (Error $e2) {
                return new Response("caught-twice");
            }
            return new Response("caught-once");
        }
        return new Response("no-throw");
    });

    // chunked 读取超限且不 catch → 统一路径 500
    $web->route("/limit-raw", function (Request $r) {
        return new Response("raw len=" . strlen($r->body()));
    });

    // ---- multipart 文本字段超限（upload_max_bytes=1024）----
    // form() 抛 Error（可捕获、重复抛）；完整接收的文件仍可 file()/files()
    $web->route("/limit-form", function (Request $r) {
        try {
            $r->form();
        } catch (Error $e) {
            try {
                $r->form();   // 第二次：重复抛同一 Error
            } catch (Error $e2) {
                return new Response("form-twice files=" . count($r->files()));
            }
            return new Response("form-once");
        }
        return new Response("form-nothrow");
    });

    // multipart 文本字段超限未捕获 → 500
    $web->route("/limit-form-raw", function (Request $r) {
        $r->form();
        return new Response("form-ok");
    });

    // stats：入口 413 计入 requests_total 与 responses["413"]
    $web->route("/stat2", function (Request $r) use ($web): Response {
        $s = $web->stats();
        return new Response(
            "req=" . $s["requests_total"] .
            " r413=" . ($s["responses"]["413"] ?? 0)
        );
    });
}

$web = new Web();
register_routes($web);

$web->start([
    "listening_ports" => "18123",
    "num_threads" => "8",
    "upload_max_bytes" => "1024",   // body 上限测试用（/limit-* 路由）
    "max_request_size" => "4096",   // civetweb 头缓冲（透传键）；>4096B 头 → 413 用例
]);

// 窗口 1：驱动侧跑完全部断言
$t0 = time();
while (time() - $t0 < 20) { $web->drain(); }

// stop 幂等（第二次 ctx 已空，应静默返回）+ 同进程同端口重启
$web->stop();
$web->stop();
register_routes($web);   // stop 已释放路由表，重启前重新注册
$web->start(["listening_ports" => "18123", "num_threads" => "8",
    "upload_max_bytes" => "1024", "max_request_size" => "4096"]);
echo "restart-ok idempotent-stop-ok\n";

// 窗口 2：驱动侧断言重启后仍可服务
$t0 = time();
while (time() - $t0 < 15) { $web->drain(); }
$web->stop();
echo "phase1-done\n";
