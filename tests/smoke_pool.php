<?php
// 执行池冒烟服务端（callback_threads=4）：由 tests/run_pool.sh 驱动。
// 覆盖：并发度（4×500ms 慢回调并行）、同会话并发写不丢更新（条带锁）、
// 池回调 stop() 信号链、restart×3（每轮重建类表克隆与闭包本地副本）。
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

function register_routes($web) {
    // 慢回调 ×4：池内 usleep 走 uv timer 挂起（非阻塞 Sleep）
    for ($i = 0; $i < 4; $i++) {
        $web->route("/slow/$i", function (Request $r) {
            usleep(500000);
            return new Response("slow");
        });
    }
    $web->route("/ok", fn(Request $r) => new Response("ok"));

    // 同会话并发自增（验证 per-sid 条带锁：8 路并发读-改-写零丢更新）
    $web->route("/sess-init", function (Request $r): Response {
        $r->session()->set("n", 0);
        return new Response("init");
    });
    $web->route("/sess-inc", function (Request $r): Response {
        $s = $r->session();
        $s->set("n", (int)$s->get("n", 0) + 1);
        return new Response("inc");
    });
    $web->route("/sess-val", function (Request $r): Response {
        return new Response("n=" . $r->session()->get("n", "-"));
    });

    // 池回调内 stop()：非主线程 → stop_requested 信号 → 主线程 run() 轮询真停
    $web->route("/stop", function (Request $r) use ($web) {
        $web->stop();
        return new Response("stopped");
    });
}

$web = new Web();

// 批B：spl_autoload_register 必须在 start() 前（start 一次性快照给池线程）。
// autoloader echo 线程标记——「-pool」证快照在池线程真实执行，跨线程合计
// ≥2 证每个消费者线程各自首 miss 各跑一次（类表按线程隔离 + 动态 new
// 绕开共享字节码 ic 快路，字面量 new 首个消费者填 ic 后其余线程不再
// resolve）；类不可被主脚本提前使用（否则编译期进 master 测不到池路径）。
spl_autoload_register(function (string $cls) {
    if ($cls === "PoolLazyCls") {
        echo (\Tphp\Teb\Internal::inPoolThread() ? "auto-loaded-pool"
                                                 : "auto-loaded-main") . "\n";
        require __DIR__ . "/_pool_lazy_cls.php";
    }
});

register_routes($web);
// 懒加载路由：池回调里动态 new 未加载类 → class miss → autoload 快照
$web->route("/lazy", function (Request $r): Response {
    $cls = "PoolLazyCls";
    $o = new $cls();
    return new Response($o->tag());
});
$web->start([
    "listening_ports" => "18135",
    "num_threads" => "8",
    "callback_threads" => "4",
    "session_save_path" => __DIR__ . "/_pool_sess",
]);

$web->run();   // 阻塞至 /stop（可能来自池回调 → 信号链退出）
echo "run1-done\n";

// restart×3：stop 清路由 → 重新注册 → 新池（类表克隆/闭包副本重建）
for ($i = 1; $i <= 3; $i++) {
    register_routes($web);
    $web->route("/lazy", function (Request $r): Response {
        $cls = "PoolLazyCls";
        $o = new $cls();
        return new Response($o->tag());
    });
    $web->start([
        "listening_ports" => "18135",
        "num_threads" => "8",
        "callback_threads" => "4",
        "session_save_path" => __DIR__ . "/_pool_sess",
    ]);
    $t = time();
    while (time() - $t < 1) { $web->drain(); }
    $web->stop();
    echo "restart$i-ok\n";
}
echo "pool-phase-done\n";
