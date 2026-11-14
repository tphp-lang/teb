<?php
// 路由匹配规则：精确 / 前缀 / notFound / 优先级
// 运行：tphp run example/01-routes.php
// 试：curl http://127.0.0.1:8081/exact
//     curl http://127.0.0.1:8081/api/user        （精确命中）
//     curl http://127.0.0.1:8081/api/anything    （前缀命中 /api）
//     curl http://127.0.0.1:8081/no/such/path    （进 notFound）
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// 1. 精确匹配：只命中 URI 本身
$web->route("/exact", fn(Request $r) => new Response("exact match\n"));

// 2. 前缀匹配："/api" 命中 /api 本身及 /api/xxx 任意子路径
$web->route("/api", fn(Request $r) =>
    new Response("prefix match, uri=" . $r->uri() . "\n"));

// 3. notFound：未命中任何路由时进入（注册后覆盖静态回退与最长兜底）。
//    回调内 $r->uri() 是原始未匹配路径。
$web->notFound(function (Request $r) {
    return new Response(
        "404 not found: " . $r->uri() . " (method=" . $r->method() . ")\n",
        404
    );
});

// 优先级：精确 > 前缀 > notFound > 最长注册路由兜底 > civetweb 默认（静态/404）
// 路由可在 start() 前后任意注册；stop() 会清空路由表，重启后需重新注册。

echo "01-routes listening on http://127.0.0.1:8081\n";
$web->start(["listening_ports" => "8081"]);
$web->run();
