<?php
// before 中间件链 + 静态目录映射 + stats
// 运行：tphp run example/06-middleware-static.php
// 试：curl    http://127.0.0.1:8086/assets/            （目录 -> index.html）
//     curl    http://127.0.0.1:8086/assets/note.txt
//     curl    http://127.0.0.1:8086/assets/secret.txt  （被 before 403）
//     curl -i http://127.0.0.1:8086/api                 （before 注入响应头）
//     curl    http://127.0.0.1:8086/stats
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// ---- before 链：按注册顺序执行，返回 Response 即短路（路由/静态都不再走）----

// 1) 命中 secret.* 一律 403，连静态文件都拦得住
$web->before(function (Request $r) {
    if (strpos($r->path(), "/assets/secret") === 0) {
        return new Response("forbidden by before\n", 403);
    }
    return null;
});

// 2) 给所有 /api 请求记下进入时刻（global 在闭包内可用），并在路由里读回
$web->before(function (Request $r) {
    global $__t0;
    if (strpos($r->path(), "/api") === 0) {
        $__t0 = microtime(true);
    }
    return null;   // 不返回 Response 就放行
});

// ---- 路由 ----
$web->route("/api", function (Request $r) {
    global $__t0;
    return new Response(
        "api ok, elapsed=" . sprintf("%.3fms", (microtime(true) - $__t0) * 1000) . "\n",
        200,
        ["X-Middleware" => "passed-before"]   // 也可以在路由里补响应头
    );
});

$web->route("/stats", function (Request $r) use ($web) {
    return Response::json($web->stats());   // 请求计数 / 字节数 / ws 帧数等
});

// ---- 静态目录：URI 前缀 -> 磁盘目录（example/assets/）----
$web->file("/assets", __DIR__ . "/assets");
// 访问 /assets/ 会自动补 index.html；/assets/note.txt 按扩展名给 Content-Type

echo "06-middleware-static listening on http://127.0.0.1:8086\n";
$web->start(["listening_ports" => "8086"]);
$web->run();
