<?php
// EXPECT: smoke=hello from Tphp\Teb
require __DIR__ . "/../vendor/autoload.php";

use Tphp\Teb\Web;
use Tphp\Teb\Response;
use Tphp\Teb\Request;

$web = new Web();
$web->start(8080);
$web->route("/hello", function (Request $request): Response {
    var_dump($request->method());
    return new Response("hello from Tphp\Teb");
})
    ->route("/", function (Request $request): Response {
        $html = <<<HTML
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0
">    <title>Welcome</title>
</head>
<body>
    <h1>Welcome to the homepage</h1>
</body>
</html>
HTML;
        return new Response($html, 200, ["Content-Type" => "text/html"]);
    })
    ->run()
    ->stop();
