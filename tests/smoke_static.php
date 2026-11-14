<?php
// tphp/teb 冒烟测试 —— 阶段 2：无路由回退（静态文件直通 / 404）
// 由 tests/run.sh 驱动。故意不注册任何路由：
// 全部请求交回 civetweb 默认处理 —— document_root 下存在的文件直接返回，否则 404。

use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

$web->start([
    "listening_ports" => "18124",
    "num_threads" => "4",
    "document_root" => __DIR__,
]);

$t0 = time();
while (time() - $t0 < 12) { $web->drain(); }
$web->stop();
echo "phase2-done\n";
