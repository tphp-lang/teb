<?php
// Session：服务端会话（登录态）
// 运行：tphp run example/09-session.php
// 试：curl -i -c ck.txt http://127.0.0.1:8089/login      （Set-Cookie: tphp_sid=… 自动注入）
//     curl    -b ck.txt http://127.0.0.1:8089/whoami     （跨请求读回会话数据）
//     curl    -b ck.txt -c ck.txt http://127.0.0.1:8089/visits  （计数持久化，重放两次看 1→2）
//     curl    -b ck.txt -c ck.txt http://127.0.0.1:8089/regen   （换新 sid，数据保留）
//     curl -i -b ck.txt -c ck.txt http://127.0.0.1:8089/logout  （Max-Age=0 清 cookie）
//     curl -i http://127.0.0.1:8089/remember             （Response::cookie 三参 opts）
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// 登录：$r->session() 惰性打开（首次调用生成 64hex sid 并读文件），
// Set-Cookie(tphp_sid; Path=/; HttpOnly; SameSite=Lax; Max-Age=…) 由 teb
// 在响应写出时自动注入，无需手拼头
$web->route("/login", function (Request $r) {
    $s = $r->session();
    $s->set("user", "alice");
    $s->set("login_at", time());
    return Response::json(["ok" => true, "user" => $s->get("user")]);
});

// 读回：带 cookie 的请求自动取回同一会话（未登录返回 -）
$web->route("/whoami", function (Request $r) {
    return new Response("user=" . $r->session()->get("user", "-"));
});

// 跨请求持久化：计数每次 +1
$web->route("/visits", function (Request $r) {
    $s = $r->session();
    $s->set("visits", (int)$s->get("visits", 0) + 1);
    return new Response("visits=" . $s->get("visits"));
});

// 换会话 id（登录成功后防会话固定）：数据保留，旧 sid 服务端立即失效。
// 注意：简单插值 `"$old…"` 会把多字节字节吞进变量名（PHP 同款语义，
// 标签允许 \x7f-\xff），紧邻中文/符号时改用拼接或 {$old}
$web->route("/regen", function (Request $r) {
    $s = $r->session();
    $old = substr($s->id(), 0, 8);
    $s->regenerateId();
    $new = substr($s->id(), 0, 8);
    return new Response("sid " . $old . "… -> " . $new .
                        "… user=" . $s->get("user", "-"));
});

// 登出：删服务端数据 + 本响应下发清除 cookie（Max-Age=0）
$web->route("/logout", function (Request $r) {
    $r->session()->destroy();
    return new Response("bye");
});

// 不碰会话的路由：不产生 Set-Cookie、不读写会话文件（惰性保证）
$web->route("/ping", fn(Request $r) => new Response("pong"));

// Response::cookie 三参 opts：Path/Domain/Expires/Max-Age/Secure/HttpOnly/SameSite
$web->route("/remember", function (Request $r) {
    return new Response("cookie set\n")
        ->cookie("remember", "1", [
            "path" => "/", "max_age" => 2592000,
            "secure" => true, "httponly" => true, "sameSite" => "Lax",
        ]);
});

echo "09-session listening on http://127.0.0.1:8089\n";
$web->start(["listening_ports" => "8089"]);
$web->run();
