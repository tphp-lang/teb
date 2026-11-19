#!/usr/bin/env bash
# tphp/teb 执行池冒烟驱动（Git Bash）：callback_threads=4
# 用法：bash tests/run_pool.sh
# 覆盖：并行度实测（4×500ms 慢回调墙钟 < 1500ms，串行≈2000ms）、同会话
# 并发写零丢更新（条带锁）、池回调 stop() 信号链、restart×3、进程自然退出。
set -u
cd "$(dirname "$0")/.."   # teb 仓根

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
PORT="${TEB_TEST_PORT:-18135}"
BASE="http://127.0.0.1:${PORT}"
LOG=/tmp/teb_smoke_pool.log
JAR=/tmp/teb_pool_jar.txt
SESSDIR=tests/_pool_sess
PASS=0
FAIL=0
RESULTS=""

check() { # check 名称 期望 实际
    local name="$1" want="$2" got="$3"
    if [ "$want" = "$got" ]; then
        PASS=$((PASS + 1))
        RESULTS+="PASS  $name"$'\n'
    else
        FAIL=$((FAIL + 1))
        RESULTS+="FAIL  $name  want=[$want] got=[$got]"$'\n'
    fi
}

curlp() { curl -s --noproxy '*' -m 10 "$@"; }

wait_ready() { # wait_ready url 超时秒
    local url="$1" tmo="$2" t0=$SECONDS code
    code=$(curlp -o /dev/null -w '%{http_code}' "$url" 2>/dev/null)
    while [ "$code" != "200" ]; do
        [ $((SECONDS - t0)) -ge "$tmo" ] && return 1
        sleep 0.3
        code=$(curlp -o /dev/null -w '%{http_code}' "$url" 2>/dev/null)
    done
    return 0
}

echo "== tphp/teb pool smoke tests (callback_threads=4) =="
echo "tphp: $TPHP_BIN"

rm -rf "$SESSDIR" "$JAR"
rm -f "$LOG"
"$TPHP_BIN" run tests/smoke_pool.php >"$LOG" 2>&1 &
PID=$!

if wait_ready "$BASE/ok" 15; then
    # ---- 并行度：4×500ms 慢回调并发 ----
    # 100ms 错峰发射：避免 4 请求同批入队被单消费者整批取走（批语义下
    # 那是合法的串行处理，非并行失效）；错峰串行仍≈2000ms，池并行≈800ms。
    # 阈值 1500ms：决定性判别 + 冷启动余量。
    t0=$(date +%s%3N)
    curlp -o /dev/null "$BASE/slow/0" & p1=$!
    sleep 0.1
    curlp -o /dev/null "$BASE/slow/1" & p2=$!
    sleep 0.1
    curlp -o /dev/null "$BASE/slow/2" & p3=$!
    sleep 0.1
    curlp -o /dev/null "$BASE/slow/3" & p4=$!
    wait $p1 $p2 $p3 $p4
    t1=$(date +%s%3N)
    wall=$((t1 - t0))
    if [ "$wall" -lt 1500 ]; then
        check "4 slow callbacks parallel (wall=${wall}ms<1500)" "ok" "ok"
    else
        check "4 slow callbacks parallel (wall<1500)" "<1500" "${wall}ms"
    fi

    # ---- 同会话并发写：8 路并发自增，终值必须 = 8（条带锁防丢更新）。
    # 显式 Cookie 头（不用 jar）：8 个 curl 并发写同一 jar 文件会撕坏 sid。 ----
    sid=$(curlp -i "$BASE/sess-init" \
          | grep -i '^Set-Cookie:' | head -1 \
          | sed 's/.*tphp_sid=\([0-9a-f]*\).*/\1/')
    check "sess-init issues 64hex sid" "64" "${#sid}"
    inc_pids=""
    for i in 1 2 3 4 5 6 7 8; do
        curlp -H "Cookie: tphp_sid=$sid" "$BASE/sess-inc" >/dev/null &
        inc_pids="$inc_pids $!"
    done
    wait $inc_pids   # 显式 PID：裸 wait 会连后台服务器一起等（死锁）
    val=$(curlp -H "Cookie: tphp_sid=$sid" "$BASE/sess-val")
    check "concurrent same-session writes lose nothing" "n=8" "$val"
else
    check "server ready" "ready" "timeout"
fi

# ---- 池回调 stop() 信号链：run() 退出 + 自然退出码 0 ----
resp=$(curlp -i "$BASE/stop")
if echo "$resp" | head -1 | grep -q "200"; then
    check "pool-side stop responds" "200" "200"
else
    check "pool-side stop responds" "200" "$(echo "$resp" | head -1)"
fi

# stop 后 run() 返回 → restart×3 → 进程自然退出（不许 kill）
t0=$SECONDS
while kill -0 $PID 2>/dev/null && [ $((SECONDS - t0)) -lt 30 ]; do
    sleep 0.3
done
if kill -0 $PID 2>/dev/null; then
    check "server exits naturally after restart cycle" "exit" "hang"
    kill -9 $PID 2>/dev/null
else
    wait $PID 2>/dev/null
    check "server exits naturally after restart cycle" "exit" "exit"
fi

grep -q "run1-done" "$LOG" \
    && check "run() exits via pool stop signal" "ok" "ok" \
    || check "run() exits via pool stop signal" "ok" "missing"
for i in 1 2 3; do
    grep -q "restart$i-ok" "$LOG" \
        && check "restart $i (fresh pool clones)" "ok" "ok" \
        || check "restart $i (fresh pool clones)" "ok" "missing"
done
grep -q "pool-phase-done" "$LOG" \
    && check "pool phase marker" "ok" "ok" \
    || check "pool phase marker" "ok" "missing"
# 池线程 bootstrap / 闭包重建失败会打 Warning/Fatal
if grep -qE "Fatal|pool thread failed to rebuild|failed to resolve teb classes" "$LOG"; then
    check "no fatal/pool warnings in log" "clean" "dirty"
else
    check "no fatal/pool warnings in log" "clean" "clean"
fi

rm -rf "$SESSDIR" "$JAR"

# ---------- 摘要 ----------
echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "-- server log --"; tail -20 "$LOG" 2>/dev/null; }
[ "$FAIL" -eq 0 ]
