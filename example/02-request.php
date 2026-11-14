<?php
// Request 解析助手：把请求信息回显成 JSON，方便逐项观察
// 运行：tphp run example/02-request.php
// 试：curl "http://127.0.0.1:8082/?page=2&tag=php" -H "X-Token: abc" -b "sid=42"
//     curl -X POST http://127.0.0.1:8082/echo -d "hello=world&x=1"
//     curl -X POST http://127.0.0.1:8082/json -H "Content-Type: application/json" -d '{"n":1}'
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

function j($v): Response {
    return new Response(json_encode($v) . "\n", 200,
        ["Content-Type" => "application/json"]);
}

$web = new Web();

// 全量回显：query / header / cookie / 客户端信息
$web->route("/", function (Request $r) {
    return j([
        "method"       => $r->method(),
        "uri"          => $r->uri(),          // 原始 URI（含 query）
        "path"         => $r->path(),         // 去 query 的路径
        "query"        => $r->query(),        // 原始 query 串
        "queryParams"  => $r->queryParams(),  // 解析后的 k=>v 表
        "queryParam"   => $r->queryParam("page", "默认值"),
        "header"       => $r->header("X-Token"),
        "userAgent"    => $r->header("User-Agent"),
        "cookies"      => $r->cookies(),
        "cookie"       => $r->cookie("sid"),
        "headers"      => $r->headers(),      // 全部请求头名字列表
        "clientIp"     => $r->clientIp(),     // X-Forwarded-For 优先
        "remoteAddr"   => $r->remoteAddr(),
        "remotePort"   => $r->remotePort(),
        "httpVersion"  => $r->httpVersion(),
        "isSsl"        => $r->isSsl(),
        "contentLength"=> $r->contentLength(),
    ]);
});

// 原始 body 回显（不解析）
$web->route("/echo", function (Request $r) {
    return new Response($r->body(), 200, ["Content-Type" => $r->header("Content-Type") ?: "text/plain"]);
});

// POST body 按 JSON 解析
$web->route("/json", function (Request $r) {
    $obj = $r->json();          // 解析失败返回 null
    return j(["got" => $obj]);
});

// application/x-www-form-urlencoded 解析
$web->route("/form", function (Request $r) {
    return j(["form" => $r->form()]);   // 注意：multipart 请走 files()，form() 会消费 body
});

echo "02-request listening on http://127.0.0.1:8082\n";
$web->start(["listening_ports" => "8082"]);
$web->run();
