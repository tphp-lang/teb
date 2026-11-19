<?php
// 批A 路由冒烟服务端：方法分发 + {param} 路径参数 + 405 + 五档优先级
// 由 tests/run_route2.sh 驱动。不注册 notFound（405 与兜底需在无 nf 下区分）。
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// T1 精确字面量
$web->route("/literal", function (Request $r): Response {
    return new Response("lit dv=" . $r->routeParam("nope", "dv"));
});

// T2 模板（get 限定）
$web->get("/user/{id}", function (Request $r): Response {
    return new Response("id=" . $r->routeParam("id"));
});
// 混合字面+参数
$web->get("/user/{id}/posts", function (Request $r): Response {
    return new Response("post-view id=" . $r->routeParam("id"));
});
// 双参数
$web->get("/file/{name}/{kind}", function (Request $r): Response {
    $p = $r->routeParams();
    return new Response("name=" . ($p["name"] ?? "-") .
                        " kind=" . ($p["kind"] ?? "-"));
});
// URL 解码
$web->get("/encoded/{name}", function (Request $r): Response {
    return new Response("name=" . $r->routeParam("name"));
});

// T1 vs T2 优先级：字面 /lit/7 胜模板 /lit/{id}
$web->route("/lit/7", fn(Request $r) => new Response("lit7-wins"));
$web->get("/lit/{id}", function (Request $r): Response {
    return new Response("tpl-wins id=" . $r->routeParam("id"));
});

// 同路径 get+post 共存
$web->get("/same", fn(Request $r) => new Response("g"));
$web->post("/same", fn(Request $r) => new Response("p"));

// 单方法路由（405 源）
$web->get("/onlyget", fn(Request $r) => new Response("onlyget-ok"));
$web->post("/onlypost", fn(Request $r) => new Response("onlypost-ok"));

// route() = 任意方法
$web->route("/anyroute", function (Request $r): Response {
    return new Response("any-ok m=" . $r->method());
});

// 非法模板：运行期注册（start 后 route() 合法）三条全 catch
$web->route("/badtpl", function (Request $r) use ($web) {
    $n = 0;
    foreach (["/bad/{", "/bad/{}", "/dup/{a}/{a}"] as $t) {
        try {
            $web->route($t, fn(Request $x) => new Response("nope"));
            return new Response("no-raise at " . $t);   // 任一条没抛即失败
        } catch (ValueError $e) { $n++; }
        catch (Throwable $e) { $n++; }
    }
    return new Response("caught=$n");
});

// T5 最长兜底（无 nf）：任意未命中 URI 落「最长注册路由」——模板参与，
// 且兜底档不产参数（routeParam 落 default）。当前最长 = /user/{id}/posts
$web->route("/pre", fn(Request $r) => new Response("pre-hit"));

$web->start(["listening_ports" => "18136", "num_threads" => "8"]);
$t0 = time();
while (time() - $t0 < 30) { $web->drain(); }
$web->stop();
echo "route2-phase-done\n";
