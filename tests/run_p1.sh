#!/usr/bin/env bash
# tphp/teb P1 冒烟测试驱动（Git Bash）
# 用法：bash tests/run_p1.sh
# 覆盖：before 链 / 静态文件 / stats / WebSocket（握手+回显+close）
set -u
cd "$(dirname "$0")/.."

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
BASE="http://127.0.0.1:18165"
LOG=/tmp/teb_smoke_p1.log
PASS=0
FAIL=0
RESULTS=""

check() {
    local name="$1" want="$2" got="$3"
    if [ "$want" = "$got" ]; then
        PASS=$((PASS + 1))
        RESULTS+="PASS  $name"$'\n'
    else
        FAIL=$((FAIL + 1))
        RESULTS+="FAIL  $name  want=[$want] got=[$got]"$'\n'
    fi
}

curlp() { curl -s --noproxy '*' -m 5 "$@"; }
hdr_of() {
    echo "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r'
}
num() { echo "$1" | grep -o "\"$2\":[0-9]*" | grep -o "[0-9]*$" | head -1; }
wait_ready() {
    local url="$1" tmo="$2" t0=$SECONDS code
    code=$(curlp -o /dev/null -w '%{http_code}' "$url" 2>/dev/null)
    while [ "$code" != "200" ]; do
        [ $((SECONDS - t0)) -ge "$tmo" ] && return 1
        sleep 0.3
        code=$(curlp -o /dev/null -w '%{http_code}' "$url" 2>/dev/null)
    done
    return 0
}

echo "== tphp/teb P1 smoke tests =="

# 静态探针文件
mkdir -p tests/_p1_static/sub
printf 'static-p1-index' > tests/_p1_static/index.html
printf 'hello-p1-static' > tests/_p1_static/probe.txt
printf 'nested-static' > tests/_p1_static/sub/deep.txt

rm -f "$LOG"
"$TPHP_BIN" run tests/smoke_p1.php >"$LOG" 2>&1 &
PID=$!

if wait_ready "$BASE/ping" 15; then
    # ---------- before 链 ----------
    check "before order b1->b2" "trace=12" "$(curlp "$BASE/b-order")"

    resp=$(curlp -i "$BASE/b-shortcut")
    check "before shortcut status 202" "202" \
        "$(echo "$resp" | head -1 | grep -o ' 202 ' | tr -d ' ')"
    check "before shortcut header" "yes" "$(hdr_of "$resp" "X-Short")"
    check "before shortcut body" "short-circuited" "$(echo "$resp" | tail -1)"

    check "before exception -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE/b-throw")"

    # before 内流式
    check "before beginStream" "stream-via-before" "$(curlp "$BASE/b-stream")"

    # ---------- 静态文件 ----------
    check "static file passthrough" "hello-p1-static" "$(curlp "$BASE/assets/probe.txt")"
    check "static index.html on dir" "static-p1-index" "$(curlp "$BASE/assets")"
    check "static nested dir" "nested-static" "$(curlp "$BASE/assets/sub/deep.txt")"
    check "static blocked by before" "blocked-by-before" "$(curlp "$BASE/assets/block.txt")"
    # 穿越：静态匹配拒绝 ".."（不落静态内容）→ 兜底路由接住（契约：无 nf 时最长兜底）
    trav=$(curlp "$BASE/assets/../teb.c")
    case "$trav" in
        *"<?php"*|*"static-p1-index"*|*"hello-p1-static"*)
            check "static traversal rejected" "no static content" "STATIC LEAK" ;;
        *) check "static traversal rejected" "no static content" "no static content" ;;
    esac
    # 缺失文件 → 未命中静态 → 兜底路由接住（nf/兜底不被静态遮蔽的契约面）
    check "static missing falls to fallback" "NEVER-SEE" "$(curlp "$BASE/assets/no-such-file.txt")"
    check "static mime type text/html" "text/html" \
        "$(curlp -o /dev/null -D - "$BASE/assets" | grep -i "^Content-Type" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r')"

    # ---------- stats 采样请求 ----------
    curlp "$BASE/st-200" > /dev/null
    curlp "$BASE/st-200" > /dev/null
    curlp "$BASE/st-201" > /dev/null

    # ---------- WebSocket（握手 + onOpen + 回显 + PING/PONG + 分片 + 服务端 close） ----------
    wsout=$(WS_PORT=18165 WS_PATH="ws" WS_MSG="hello-ws" WS_PING=1 WS_FRAG=1 "$TPHP_BIN" run tests/wsclient.php 2>&1)
    check "ws handshake 101" "1" "$(echo "$wsout" | grep -c 'HANDSHAKE=101')"
    check "ws accept header (RFC6455 sample)" "1" \
        "$(echo "$wsout" | grep -c 'ACCEPT=1')"
    check "ws onOpen hello (remoteAddr)" "1" \
        "$(echo "$wsout" | grep -c 'RECV=hello:127.0.0.1')"
    check "ws onMessage echo + bits opcode(text=1)" "1" \
        "$(echo "$wsout" | grep -c 'RECV2=echo:hello-ws b=1')"
    check "ws ping auto-pong (op=10 payload pp)" "1" \
        "$(echo "$wsout" | grep -c 'PONG_OP=10 PONG_DATA=pp')"
    check "ws fragment assembly echo" "1" \
        "$(echo "$wsout" | grep -c 'RECV3=echo:hello-frag b=1')"

    wsout2=$(WS_PORT=18165 WS_PATH="ws" WS_MSG="close-me" "$TPHP_BIN" run tests/wsclient.php 2>&1)
    check "ws server close frame (close-me → CLOSE bye)" "1" \
        "$(echo "$wsout2" | grep -c 'CLOSE_EARLY=bye')"
    sleep 1   # 等 WS_CLOSE 事件经 drain 处理（onClose 计数）
    grep -q "ws-close-marker" "$LOG" \
        && check "ws onClose fired" "ok" "ok" \
        || check "ws onClose fired" "ok" "missing"

    # ---------- 窗口结束：自然退出 + stats 断言 ----------
    sleep 20   # 窗口 1（22s）收尾 → stop → STATS 行 → 自然退出
    wait $PID 2>/dev/null
    NEXIT=$?
    check "natural exit code 0" "0" "$NEXIT"
    grep -q "p1-done" "$LOG" && check "natural exit marker" "ok" "ok" \
        || check "natural exit marker" "ok" "missing"

    statsline=$(grep "^STATS=" "$LOG" | tail -1 | sed 's/^STATS=//')
    rtotal=$(num "$statsline" "requests_total")
    check "stats requests_total >= 10" "1" \
        "$([ "$rtotal" -ge 10 ] 2>/dev/null && echo 1 || echo 0)"
    r200=$(echo "$statsline" | grep -o '"200":[0-9]*' | grep -o '[0-9]*$' | head -1)
    check "stats responses 200 >= 8" "1" \
        "$([ "$r200" -ge 8 ] 2>/dev/null && echo 1 || echo 0)"
    r201=$(num "$statsline" "201")
    check "stats responses 201 == 1" "1" \
        "$([ "$r201" -eq 1 ] 2>/dev/null && echo 1 || echo 0)"
    r500=$(num "$statsline" "500")
    check "stats responses 500 >= 1" "1" \
        "$([ "$r500" -ge 1 ] 2>/dev/null && echo 1 || echo 0)"
    wsc=$(num "$statsline" "ws_closed")
    wsf=$(num "$statsline" "ws_frames")
    check "stats ws_frames >= 3 (echo+frag+close-me)" "1" \
        "$([ "$wsf" -ge 3 ] 2>/dev/null && echo 1 || echo 0)"
    check "stats ws_closed >= 2" "1" \
        "$([ "$wsc" -ge 2 ] 2>/dev/null && echo 1 || echo 0)"
    bts=$(num "$statsline" "bytes_sent")
    check "stats bytes_sent > 0" "1" \
        "$([ "$bts" -gt 0 ] 2>/dev/null && echo 1 || echo 0)"
else
    check "server ready" "ready" "timeout"
    kill $PID 2>/dev/null
    sleep 1
    kill -9 $PID 2>/dev/null
fi

rm -rf tests/_p1_static

echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "-- server log --"; tail -8 "$LOG" 2>/dev/null; }
[ "$FAIL" -eq 0 ]
