<?php
// 流式响应（SSE 风格）：beginStream / write / end
// 运行：tphp run example/05-stream.php
// 试：curl -N http://127.0.0.1:8085/events
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

// 流式回调不要声明 ": Response" 返回类型 —— 返回 null 表示已手动输出
$web->route("/events", function (Request $r) {
    $s = $r->beginStream();
    $s->header("Content-Type", "text/event-stream");
    $s->header("Cache-Control", "no-cache");

    for ($i = 1; $i <= 5; $i++) {
        $s->write("data: tick " . $i . "\n\n");
        usleep(300000);   // 每 300ms 推一条
    }

    $s->write("data: done\n\n");
    $s->end();            // 不调用 end 也会在回调返回后自动收尾
    return null;
});

// 非流式对照：一次性大 body
$web->route("/big", fn(Request $r) =>
    new Response(str_repeat("x", 100000) . "\n"));

echo "05-stream listening on http://127.0.0.1:8085\n";
$web->start(["listening_ports" => "8085"]);
$web->run();
