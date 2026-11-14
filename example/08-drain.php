<?php
// drain() 事件泵：主线程自控行为（定时退出、tick 统计）
// run() = drain() 死循环；想自己掌握节奏（定时任务、优雅退出）就用 drain()
// 运行：tphp run example/08-drain.php （15 秒后自然退出并打印统计）
// 试：curl http://127.0.0.1:8088/  多打几次再看退出时的 STATS
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

$web->route("/", fn(Request $r) => new Response("drain demo\n"));

echo "08-drain listening on http://127.0.0.1:8088 (exits in 15s)\n";
$web->start(["listening_ports" => "8088"]);

$t0 = time();
$n = 0;
while (time() - $t0 < 15) {
    // drain()：处理已就绪的请求/WS 事件，立即返回（非阻塞）
    $web->drain();

    $n = $n + 1;
    if ($n % 1000000 == 0) {
        $st = $web->stats();
        echo "tick " . $n . ", requests=" . $st["requests"] . "\n";
        usleep(1000);   // 空闲时让出 CPU；忙时去掉这行
    }
}

$web->stop();   // 清空连接池与路由表；析构也会自动 stop
echo "STATS=" . json_encode($web->stats()) . "\n";
echo "drain demo exit\n";
