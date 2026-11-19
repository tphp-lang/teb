#!/usr/bin/env bash
# tphp/teb 冒烟测试驱动（Git Bash）
# 用法：bash tests/run.sh
# 可用环境变量：
#   TPHP_BIN      tphp 可执行文件路径（默认 ../TinyPHP.rt/build/tphp.exe）
#   TEB_TEST_PORT 阶段 1 端口（默认 18123，需与 smoke_routes.php 内一致）
set -u
cd "$(dirname "$0")/.."   # teb 仓根

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
PORT_R="${TEB_TEST_PORT:-18123}"
PORT_S="18124"
BASE_R="http://127.0.0.1:${PORT_R}"
BASE_S="http://127.0.0.1:${PORT_S}"
LOG1=/tmp/teb_smoke_routes.log
LOG2=/tmp/teb_smoke_static.log
PROBE=tests/_static_probe.txt
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

# 提取响应头（curl -i 输出）：hdr_of "$response" "Header-Name"
hdr_of() {
    echo "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r'
}

curlp() { curl -s --noproxy '*' -m 5 "$@"; }

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

echo "== tphp/teb smoke tests =="
echo "tphp: $TPHP_BIN"

# ---------- 阶段 1：路由 API ----------
rm -f "$LOG1"
"$TPHP_BIN" run tests/smoke_routes.php >"$LOG1" 2>&1 &
PID1=$!

if wait_ready "$BASE_R/ok" 15; then
    # 精确匹配
    check "route exact /" "hello world" "$(curlp "$BASE_R/")"

    # ---- keep-alive（批1）：HTTP/1.1 缺省持久 + 同进程连接复用 + 1.0 降级 ----
    if curl -s --noproxy '*' -m 5 -v -o /dev/null "$BASE_R/ok" "$BASE_R/ok" 2>&1 \
        | grep -q "Re-using existing connection"; then
        check "keep-alive: same-conn reuse" "reused" "reused"
    else
        check "keep-alive: same-conn reuse" "reused" "new-conn"
    fi
    if curl -s --noproxy '*' -m 5 -v -o /dev/null --http1.0 "$BASE_R/ok" "$BASE_R/ok" 2>&1 \
        | grep -q "Re-using existing connection"; then
        check "http/1.0 downgrade: no reuse" "no-reuse" "reused"
    else
        check "http/1.0 downgrade: no reuse" "no-reuse" "no-reuse"
    fi
    ckclose=$(curlp -i "$BASE_R/ok" | grep -ci "^Connection: close")
    check "response omits Connection: close" "0" "$ckclose"

    # 前缀匹配 + remoteAddr
    body=$(curlp "$BASE_R/hello")
    case "$body" in
        ip=127.0.0.1\ port=*) check "route prefix /hello + remoteAddr" "ok" "ok" ;;
        *) check "route prefix /hello + remoteAddr" "ip=127.0.0.1 port=N" "$body" ;;
    esac

    # 请求信息
    check "request info" \
        "GET|/info?a=1&b=xyz|/info|a=1&b=xyz|1.1|plain" \
        "$(curlp "$BASE_R/info?a=1&b=xyz")"

    # POST body 回显
    check "request body echo" "m=POST body=payload123" \
        "$(curlp -X POST -d "payload123" "$BASE_R/echo")"

    # header 大小写不敏感 + 缺失返回 null + headers() 计数
    body=$(curlp -H "X-Test-Header: abc123" "$BASE_R/hdr")
    case "$body" in
        lo=abc123\ up=abc123\ miss=null\ n=[0-9]*)
            check "request header case-insensitive" "ok" "ok" ;;
        *) check "request header case-insensitive" "lo=abc123 up=abc123 miss=null n=N" "$body" ;;
    esac

    # 链式 Response：状态码 / 头 / Cookie / 覆盖 Content-Type
    resp=$(curlp -i "$BASE_R/chain")
    check "response chain status 201" "201" \
        "$(echo "$resp" | head -1 | grep -o ' 201 ' | tr -d ' ')"
    check "response chain X-Req-Id" "r1" "$(hdr_of "$resp" "X-Req-Id")"
    check "response chain Set-Cookie" "sid=s1" "$(hdr_of "$resp" "Set-Cookie")"
    check "response chain X-Extra" "e1" "$(hdr_of "$resp" "X-Extra")"
    check "response chain Content-Type override" "text/x-chain" "$(hdr_of "$resp" "Content-Type")"
    check "response chain body" "chained" "$(echo "$resp" | tail -1)"

    # Response::body() 读取自检
    check "response body() readback" "body-ok" "$(curlp "$BASE_R/respbody")"

    # 非 Response 返回 → 500
    check "non-Response return -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE_R/nonresp")"

    # 回调抛异常 → 500
    check "callback throw -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE_R/throw")"

    # Request 手动构造被拒
    check "Request ctor blocked" "req-ctor-blocked" "$(curlp "$BASE_R/reqnew")"

    # 匹配优先级：前缀命中优先于最长兜底；仅兜底可达时取最长路由
    check "prefix match beats fallback" "fb-short" "$(curlp "$BASE_R/fb/other")"
    check "longest fallback route" "fb-longest" "$(curlp "$BASE_R/fbxyz")"
    check "exact beats prefix" "fb-longest" "$(curlp "$BASE_R/fb/longest-route-here")"

    # ---- body 上限（smoke_routes.php upload_max_bytes=1024）----
    BIGF=/tmp/teb_big_body.bin
    head -c 2048 /dev/zero | tr '\0' 'x' > "$BIGF"

    # stats 基线（其后紧跟一次入口 413，用于验证计数）
    s1=$(curlp "$BASE_R/stat2")
    r1=$(echo "$s1" | sed 's/^req=\([0-9]*\).*/\1/')

    # 声明 Content-Length 超限 → 入口 413（回调不执行）
    check "declared body > limit -> 413" "413" \
        "$(curlp -o /dev/null -w '%{http_code}' -X POST --data-binary "@$BIGF" "$BASE_R/limit-cl")"

    # 入口 413 计入 stats：s2 = 基线 + 413 请求 + s2 自身（漏计则 r1+1）
    s2=$(curlp "$BASE_R/stat2")
    check "413 counted in requests_total" "$((r1 + 2))" \
        "$(echo "$s2" | sed 's/^req=\([0-9]*\).*/\1/')"
    check "413 counted in responses" "r413=1" \
        "$(echo "$s2" | sed 's/.*\(r413=[0-9]*\).*/\1/')"

    # 请求头超 max_request_size（4096）→ civetweb 层拒绝（回调不进；
    # 实测本版 civetweb：缓冲填满返 -2 → 400 Malformed message，非 413——
    # 其 413 分支要求 request_len==0 且缓冲满，read_message 满即返 -2 不可达；
    # 该错误响应由 civetweb 自出，不经 teb，不计入 stats）
    check "header within max_request_size -> 200" "200" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE_R/ok")"
    BIGH=$(head -c 5000 /dev/zero | tr '\0' 'h')
    check "header > max_request_size -> 400" "400" \
        "$(curlp -o /dev/null -w '%{http_code}' -H "X-Big: $BIGH" "$BASE_R/ok")"

    # chunked（未声明长度）读取超限 → body() 抛 Error，两次 catch 均命中
    check "chunked over-limit caught twice" "caught-twice" \
        "$(curlp -X POST -H "Transfer-Encoding: chunked" --data-binary "@$BIGF" "$BASE_R/limit-ch")"

    # chunked 读取超限且不 catch → 统一路径 500
    check "chunked over-limit uncaught -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' -X POST -H "Transfer-Encoding: chunked" --data-binary "@$BIGF" "$BASE_R/limit-raw")"

    # chunked 未超限照常工作（回归：上限内 body 正常读取）
    check "chunked within limit" "m=POST body=hello-chunked" \
        "$(curlp -X POST -H "Transfer-Encoding: chunked" --data-binary "hello-chunked" "$BASE_R/echo")"

    # 恰好等于上限（1024B 声明）→ 不算超限，正常进回调
    head -c 1024 /dev/zero | tr '\0' 'y' > "$BIGF"
    check "body exactly at limit -> 200" "200" \
        "$(curlp -o /dev/null -w '%{http_code}' -X POST --data-binary "@$BIGF" "$BASE_R/echo")"
    rm -f "$BIGF"

    # multipart 文本字段超限（2048B > 1024B）→ form() 抛 Error 且重复抛
    BIGT=$(head -c 2048 /dev/zero | tr '\0' 'z')
    check "multipart text over-limit caught twice + files ok" "form-twice files=0" \
        "$(curlp -F "t=$BIGT" "$BASE_R/limit-form")"
    check "multipart text over-limit uncaught -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' -F "t=$BIGT" "$BASE_R/limit-form-raw")"
else
    check "server phase1 ready" "ready" "timeout"
fi

# 窗口 1（20s）结束后脚本自动 stop x2 + 同端口重启
REMAIN=$((24 - SECONDS))
[ $REMAIN -gt 0 ] && sleep $REMAIN
check "restart after stop (window2 /ok)" "ok" "$(curlp "$BASE_R/ok")"
check "restart marker in log" "1" \
    "$(grep -c "restart-ok idempotent-stop-ok" "$LOG1")"

kill $PID1 2>/dev/null
sleep 1
kill -9 $PID1 2>/dev/null
wait $PID1 2>/dev/null

# ---------- 阶段 2：无路由回退（静态文件 / 404）----------
printf 'static-probe-123' > "$PROBE"
rm -f "$LOG2"
"$TPHP_BIN" run tests/smoke_static.php >"$LOG2" 2>&1 &
PID2=$!

if wait_ready "$BASE_S/_static_probe.txt" 15; then
    check "static file passthrough" "static-probe-123" \
        "$(curlp "$BASE_S/_static_probe.txt")"
    check "no route no file -> 404" "404" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE_S/definitely-not-here")"
else
    check "server phase2 ready" "ready" "timeout"
fi

kill $PID2 2>/dev/null
sleep 1
kill -9 $PID2 2>/dev/null
wait $PID2 2>/dev/null
rm -f "$PROBE"

# ---------- 摘要 ----------
echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || {
    echo "-- server log (routes) --"; tail -5 "$LOG1" 2>/dev/null
    echo "-- server log (static) --"; tail -5 "$LOG2" 2>/dev/null
}
[ "$FAIL" -eq 0 ]
