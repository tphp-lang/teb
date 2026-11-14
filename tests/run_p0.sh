#!/usr/bin/env bash
# tphp/teb P0 冒烟测试驱动（Git Bash）
# 用法：bash tests/run_p0.sh
# 前置：tests/run.sh（基础冒烟）先过；teb 已 ext --add 链入。
set -u
cd "$(dirname "$0")/.."

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
BASE="http://127.0.0.1:18125"
LOG=/tmp/teb_smoke_p0.log
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

curlp() { curl -s --noproxy '*' -m 8 "$@"; }
hdr_of() {
    echo "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r'
}
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

echo "== tphp/teb P0 smoke tests =="

# 上传探针文件
printf 'small-upload-content-1234567890' > tests/_up_small.txt   # 32B
head -c 300 /dev/urandom > tests/_up_probe.bin                    # 300B（> 200B 上限）

rm -f tests/_up_moved.bin "$LOG"
"$TPHP_BIN" run tests/smoke_p0.php >"$LOG" 2>&1 &
PID=$!

if wait_ready "$BASE/f-text" 15; then
    # ---------- 流式 ----------
    body=$(curlp -N "$BASE/stream-chunks")
    check "stream chunks body" "abc" "$body"
    # 逐块可见性：starttransfer（首块即时）与 time_total（usleep 生效）已单测
    # 验证（0.001s / 0.41s）；此处仅打印供人工复核，不作硬断言（负载敏感）
    tt=$(curlp -N -o /dev/null -w '%{time_total}' "$BASE/stream-chunks")
    st=$(curlp -N -o /dev/null -w '%{time_starttransfer}' "$BASE/stream-chunks")
    RESULTS+="INFO  stream timing total=${tt}s first-chunk=${st}s"$'\n'

    check "stream auto-end (no end call)" "xyz" "$(curlp "$BASE/stream-auto")"

    resp=$(curlp -i "$BASE/stream-throw-after")
    code=$(echo "$resp" | head -1 | grep -o ' [0-9][0-9][0-9] ' | tr -d ' ')
    check "stream throw after header: not 500" "200" "$code"
    body=$(echo "$resp" | tail -1)
    case "$body" in *visible*) check "stream throw after header: body intact" "ok" "ok" ;;
        *) check "stream throw after header: body intact" "*visible*" "$body" ;; esac

    check "stream throw before header: 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE/stream-throw-before")"

    resp=$(curlp -i "$BASE/stream-status-header")
    check "stream status mutate 207" "207" \
        "$(echo "$resp" | head -1 | grep -o ' 207 ' | tr -d ' ')"
    check "stream header X-S" "s1" "$(hdr_of "$resp" "X-S")"
    check "stream header X-S2" "s2" "$(hdr_of "$resp" "X-S2")"
    check "stream body part" "part" "$(echo "$resp" | tail -1)"

    # ---------- 上传 ----------
    resp=$(curlp -F "name=hello-field" -F "up=@tests/_up_small.txt" "$BASE/upload")
    check "upload form field" "form-name=hello-field" "$(echo "$resp" | grep -o 'form-name=[^|]*')"
    check "upload client filename" "up-name=_up_small.txt" "$(echo "$resp" | grep -o 'up-name=[^|]*')"
    check "upload size" "up-size=31" "$(echo "$resp" | grep -o 'up-size=[^|]*')"
    check "upload error 0" "up-error=0" "$(echo "$resp" | grep -o 'up-error=[^|]*')"
    check "upload moved" "moved=true" "$(echo "$resp" | grep -o 'moved=[^|]*')"
    check "upload tmp cleared after moveTo" "tmp-after=" "$(echo "$resp" | grep -o 'tmp-after=[^|]*')"
    check "upload moveTo file content" "small-upload-content-1234567890" "$(cat tests/_up_moved.bin)"
    rm -f tests/_up_moved.bin

    tmp=$(curlp -F "f2=@tests/_up_small.txt" "$BASE/upload-tmp1" | sed 's/^tmp=//; s/|size=.*//')
    case "$tmp" in
        *tphp_up_*) check "upload tmp path generated" "ok" "ok" ;;
        *) check "upload tmp path generated" "*tphp_up_*" "$tmp" ;;
    esac
    # tmp 回收在回调结束（drain 批尾）后发生 → 下一请求验证已删
    sleep 0.5
    check "upload tmp reclaimed by drain" "exists=no" \
        "$(curlp "$BASE/upload-tmp2?prev=$(echo "$tmp" | sed 's|/|%2F|g; s|\\|%5C|g; s|:|%3A|g')")"

    resp=$(curlp -F "big=@tests/_up_probe.bin" "$BASE/upload-limit")
    check "upload over limit: error=1" "err=1" "$(echo "$resp" | grep -o 'err=[^|]*')"
    check "upload over limit: tmp empty" "tmp=" "$(echo "$resp" | grep -o 'tmp=[^|]*')"
    check "upload over limit: size reported" "size=300" "$(echo "$resp" | grep -o 'size=[^|]*')"

    # ---------- notFound ----------
    check "notFound hit" "nf:/no-such-uri-xyz" "$(curlp "$BASE/no-such-uri-xyz")"
    check "notFound does not shadow exact" "plain-here" "$(curlp "$BASE/f-text")"

    # ---------- 工厂 ----------
    resp=$(curlp -i "$BASE/f-json")
    check "factory json ctype" "application/json" "$(hdr_of "$resp" "Content-Type")"
    check "factory json body" '{"a":1,"b":"x"}' "$(echo "$resp" | tail -1)"
    resp=$(curlp -i "$BASE/f-text")
    check "factory text ctype" "text/plain" "$(hdr_of "$resp" "Content-Type")"
    check "factory text body" "plain-here" "$(echo "$resp" | tail -1)"
    resp=$(curlp -i "$BASE/f-html")
    check "factory html ctype" "text/html" "$(hdr_of "$resp" "Content-Type")"
    check "factory html body" "<p>h</p>" "$(echo "$resp" | tail -1)"
    resp=$(curlp -i "$BASE/f-redirect")
    check "factory redirect status" "302" \
        "$(echo "$resp" | head -1 | grep -o ' 302 ' | tr -d ' ')"
    check "factory redirect location" "/there" "$(hdr_of "$resp" "Location")"
    check "factory json unencodable -> 500" "500" \
        "$(curlp -o /dev/null -w '%{http_code}' "$BASE/f-json-bad")"

    # ---------- 解析助手 ----------
    check "queryParams/urlencode plus" \
        "a=1|b=two words|c=empty|b2=two words|z=dft" \
        "$(curlp "$BASE/assist?a=1&b=two+words&c=")"
    check "cookies parse" "sid=abc|theme=dark|nope=dft2" \
        "$(curlp -H "Cookie: sid=abc; theme=dark; empty=" "$BASE/assist-cookie")"
    check "json body + cache idempotent" \
        'x=10|y=2|again=same|isjson=yes' \
        "$(curlp -X POST -H "Content-Type: application/json" -d '{"x":10,"y":[1,2]}' "$BASE/assist-json")"
    check "body twice + contentLength" "b1=hello-body|same=yes|len=10" \
        "$(curlp -X POST -d "hello-body" "$BASE/assist-body")"
    check "clientIp XFF first segment" "ip=9.9.9.9" \
        "$(curlp -H "X-Forwarded-For: 9.9.9.9, 10.0.0.1" "$BASE/assist-ip")"
    check "clientIp fallback remote" "ip=127.0.0.1" "$(curlp "$BASE/assist-ip")"
    check "urlencoded form + body shared cache" "k1=v1|body=k1=v1&k2=v2" \
        "$(curlp -X POST -d "k1=v1&k2=v2" "$BASE/assist-form-body")"
else
    check "server ready" "ready" "timeout"
fi

# 窗口 1（25s）结束 → 重启 phase2 → run() 常驻
REMAIN=$((28 - SECONDS))
[ $REMAIN -gt 0 ] && sleep $REMAIN
grep -q "phase1-done" "$LOG" && check "phase1 window done" "ok" "ok" || check "phase1 window done" "ok" "missing"
grep -q "phase2-started" "$LOG" && check "phase2 restart ok" "ok" "ok" || check "phase2 restart ok" "ok" "missing"

# 回调内 stop：响应正常 + run() 随之退出
check "stop in callback response" "stopping" "$(curlp "$BASE/stop-in-callback")"
sleep 2
grep -q "run-exited" "$LOG" && check "run() exited after pending_stop" "ok" "ok" || check "run() exited after pending_stop" "ok" "missing"
grep -q "p0-done" "$LOG" && check "idempotent stop after run exit" "ok" "ok" || check "idempotent stop after run exit" "ok" "missing"

# 自然退出验证（引擎 bug #2 已修：teb.c start 双 addref，2026-11-14）——
# 脚本应自行退出且 EXIT=0；kill 仅作超时保险（进程已退出时为 no-op）
wait $PID 2>/dev/null
NEXIT=$?
check "natural exit code 0" "0" "$NEXIT"

kill $PID 2>/dev/null
sleep 1
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null
rm -f tests/_up_small.txt tests/_up_probe.bin tests/_up_moved.bin

echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "-- server log --"; tail -8 "$LOG" 2>/dev/null; }
[ "$FAIL" -eq 0 ]
