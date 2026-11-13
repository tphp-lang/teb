<?php
// Tphp\Teb 对象 API 最小用例
// 运行：tphp run tests/hello.php
// 然后浏览器访问 http://localhost:8080/hello 或 http://localhost:8080/

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

$web->route("/", fn(Request $r) => new Response("hello world\n"));

$web->route("/hello", function (Request $r): Response {
    return new Response(
        "hello from Tphp\\Teb, your ip is " . $r->remoteAddr() . "\n",
        200,
        ["Content-Type" => "text/plain; charset=utf-8"]
    );
});

$web->start([
    "document_root" => __DIR__,
    "listening_ports" => "8080",
    "num_threads" => "50",
    "request_timeout_ms" => "30000",
    "authentication_domain" => "mydomain.com",
]);

$web->run();   // 阻塞常驻（Ctrl+C 停止，析构自动 stop）
