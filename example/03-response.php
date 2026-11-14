<?php
// Response：链式设置 + 静态工厂 + redirect / file
// 运行：tphp run example/03-response.php
// 试：curl -i http://127.0.0.1:8083/chain          （看状态码与响应头）
//     curl -i http://127.0.0.1:8083/json           （工厂函数）
//     curl -i http://127.0.0.1:8083/redirect       （302，跟随：curl -L）
//     curl    http://127.0.0.1:8083/file           （Response::file 直接吐文件）
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// 链式：status / header / cookie 都返回 $self；body() 是读取器（无 setter），
// 正文与状态码走构造参数
$web->route("/chain", function (Request $r) {
    return (new Response("链式构造完成\n", 201,
            ["Content-Type" => "text/plain; charset=utf-8"]))
        ->header("X-Engine", "tphp/teb")
        ->cookie("seen", "yes");          // 输出 Set-Cookie: seen=yes
});

// 工厂函数：json / text / html；withHeaders 批量加响应头
$web->route("/json", fn(Request $r) => Response::json(["ok" => true, "n" => 3]));
$web->route("/text", fn(Request $r) => Response::text("plain text\n"));
$web->route("/html", fn(Request $r) =>
    Response::html("<h1>teb</h1><p>html factory</p>")->withHeaders(["X-F" => "1"]));

// 重定向（默认 302；第二参可给 301/307 等）
$web->route("/redirect", fn(Request $r) => Response::redirect("/chain"));
$web->route("/moved",   fn(Request $r) => Response::redirect("/chain", 301));

// 直接把磁盘文件作为响应（Content-Type 按扩展名推断）
$web->route("/file", fn(Request $r) =>
    Response::file(__DIR__ . "/assets/note.txt"));

echo "03-response listening on http://127.0.0.1:8083\n";
$web->start(["listening_ports" => "8083"]);
$web->run();
