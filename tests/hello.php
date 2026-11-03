<?php
// EXPECT: smoke=hello from Tphp\Teb
require __DIR__ . "/../vendor/autoload.php";

use Tphp\Teb\Web;
use Tphp\Teb\Response;
use Tphp\Teb\Request;

$web = new Web();
$web->start(8080);
$web->route("/hello", function (Request $request) {
    // var_dump($request->method());
    return new Response("hello from Tphp\Teb");
})
    ->run()
    ->stop();
