<?php
// EXPECT: smoke=hello from Tphp\Teb
require __DIR__ . "/../vendor/autoload.php";

use Tphp\Teb\Web;
use Tphp\Teb\Response;
use Tphp\Teb\Request;

$web = new Web();
$web->options(__DIR__);
$web->route("/hello", function (Request $request): Response {
    var_dump($request->remoteAddr());
    return new Response("hello from Tphp\Teb");
})
    ->route("/", function (Request $request): Response {
        return new Response("hello world");
    })
    ->run()
    ->stop();
