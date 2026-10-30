<?php
// EXPECT: smoke=hello from Tphp\Teb
require __DIR__ . "/../vendor/autoload.php";

use Tphp\Teb\Web;

$web = new Web();
$web->start(8080);
$web->request("/hello", function ($conn) {
    var_dump($conn);
    return "hello from Tphp\Teb";
})
    ->run()
    ->stop();
