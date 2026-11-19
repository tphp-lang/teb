#!/usr/bin/env bash
# tphp/teb Session + cookie opts 冒烟驱动（Git Bash）
# 用法：bash tests/run_session.sh
# 可用环境变量：
#   TPHP_BIN      tphp 可执行文件路径（默认 ../TinyPHP.rt/build/tphp.exe）
#   TEB_TEST_PORT 端口（默认 18126，需与 smoke_session.php 内一致）
set -u
cd "$(dirname "$0")/.."   # teb 仓根

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
PORT="${TEB_TEST_PORT:-18126}"
BASE="http://127.0.0.1:${PORT}"
LOG=/tmp/teb_smoke_session.log
JAR=/tmp/teb_sess_jar.txt
SESSDIR=tests/_sess
ASSETDIR=tests/_sess_assets
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

hdr_of() { # hdr_of "$response" "Header-Name" —— 第一条匹配行
    echo "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r'
}

has_hdr() { # has_hdr "$response" "Header-Name" → yes/no
    if echo "$1" | grep -qi "^$2:"; then echo yes; else echo no; fi
}

code_of() { # code_of "$response"
    echo "$1" | head -1 | grep -o ' [0-9][0-9][0-9] ' | tr -d ' '
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

echo "== tphp/teb session smoke tests =="
echo "tphp: $TPHP_BIN"

rm -rf "$SESSDIR" "$ASSETDIR" "$JAR"
mkdir -p "$ASSETDIR"
printf 'sess-asset-123' > "$ASSETDIR/probe.txt"

rm -f "$LOG"
"$TPHP_BIN" run tests/smoke_session.php >"$LOG" 2>&1 &
PID=$!

if wait_ready "$BASE/ok" 15; then
    # ---- 阶段 A：cookie jar 会话链（建立 → 读回 → 计数 → 异常 → 换 id → 过期 → 登出）----
    resp=$(curlp -i -c "$JAR" "$BASE/s/set")
    ck=$(hdr_of "$resp" "Set-Cookie")
    if echo "$ck" | grep -Eq '^tphp_sid=[0-9a-f]{64}; Path=/; HttpOnly; SameSite=Lax; Max-Age=5$'; then
        check "first Set-Cookie format" "ok" "ok"
    else
        check "first Set-Cookie format" "tphp_sid=<64hex>; Path=/; HttpOnly; SameSite=Lax; Max-Age=5" "$ck"
    fi
    body=$(echo "$resp" | tail -1)
    if echo "$body" | grep -Eq '^id=[0-9a-f]{64} n=1$'; then
        check "first /s/set body id+n" "ok" "ok"
    else
        check "first /s/set body id+n" "id=<64hex> n=1" "$body"
    fi

    check "jar roundtrip read-back" "user=alice n=1" "$(curlp -b "$JAR" "$BASE/s/get")"
    check "cross-request persist inc" "n=2" "$(curlp -b "$JAR" -c "$JAR" "$BASE/s/inc")"
    check "cross-request persist inc 2" "n=3" "$(curlp -b "$JAR" -c "$JAR" "$BASE/s/inc")"

    # 异常路径：set 后 throw → 500 且带 Set-Cookie；数据已落盘
    resp=$(curlp -i -b "$JAR" -c "$JAR" "$BASE/s/throw")
    check "throw -> 500" "500" "$(code_of "$resp")"
    check "throw 500 carries Set-Cookie" "yes" "$(has_hdr "$resp" "Set-Cookie")"
    check "exception-path flush persisted" "after_throw=yes" "$(curlp -b "$JAR" "$BASE/s/throw-chk")"

    # regenerateId：新旧 sid 不同 + 数据仍在
    body=$(curlp -b "$JAR" -c "$JAR" "$BASE/s/regen")
    old=$(echo "$body" | sed 's/.*old=\([0-9a-f]*\) .*/\1/')
    new=$(echo "$body" | sed 's/.*new=\([0-9a-f]*\) .*/\1/')
    check "regen sid changed" "changed" "$([ -n "$old" ] && [ -n "$new" ] && [ "$old" != "$new" ] && echo changed || echo same)"
    check "regen keeps data" "alice" "$(echo "$body" | sed 's/.*user=//')"
    check "regen new sid works" "user=alice n=3" "$(curlp -b "$JAR" -c "$JAR" "$BASE/s/get")"

    # 过期：显式带 NEWSID（绕过 jar 过期）→ 服务端 mtime TTL → 空数据 + 发新 sid
    sleep 6
    resp=$(curlp -i -H "Cookie: tphp_sid=$new" "$BASE/s/get")
    check "expired session data empty" "user=- n=-" "$(echo "$resp" | tail -1)"
    ck=$(hdr_of "$resp" "Set-Cookie")
    check "expired session issues new sid" "new" \
        "$(echo "$ck" | grep -q "^tphp_sid=$new;" && echo same || echo new)"

    # 登出：重新建立会话 → destroy → Max-Age=0 → 后续空数据
    curlp -b "$JAR" -c "$JAR" "$BASE/s/set" >/dev/null
    resp=$(curlp -i -b "$JAR" -c "$JAR" "$BASE/s/logout")
    check "destroy clear cookie" "tphp_sid=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0" \
        "$(hdr_of "$resp" "Set-Cookie")"
    check "after destroy data empty" "user=- n=-" "$(curlp -b "$JAR" -c "$JAR" "$BASE/s/get")"
else
    check "server ready" "ready" "timeout"
fi

# ---- 阶段 B：无状态用例（opts / 惰性 / 流式 / 静态 / before / 伪造 sid / GC）----
resp=$(curlp -i "$BASE/s/cookie-opts")
ck=$(hdr_of "$resp" "Set-Cookie")
check "cookie opts Path" "/app" "$(echo "$ck" | grep -o 'Path=[^;]*' | cut -d= -f2-)"
check "cookie opts Domain" "ex.com" "$(echo "$ck" | grep -o 'Domain=[^;]*' | cut -d= -f2)"
check "cookie opts Max-Age" "60" "$(echo "$ck" | grep -o 'Max-Age=[0-9]*' | cut -d= -f2)"
check "cookie opts Secure" "1" "$(echo "$ck" | grep -c '; Secure')"
check "cookie opts HttpOnly" "1" "$(echo "$ck" | grep -c '; HttpOnly')"
check "cookie opts SameSite" "Strict" "$(echo "$ck" | grep -o 'SameSite=[A-Za-z]*' | cut -d= -f2)"

check "invalid cookie opts caught" "caught=ValueError" "$(curlp "$BASE/s/cookie-bad")"

resp=$(curlp -i "$BASE/s/nosess")
check "lazy: no session no Set-Cookie" "no" "$(has_hdr "$resp" "Set-Cookie")"

resp=$(curlp -i "$BASE/s/stream")
check "stream: Set-Cookie injected" "yes" "$(has_hdr "$resp" "Set-Cookie")"
check "stream: body" "stream-data" "$(echo "$resp" | tail -1)"

resp=$(curlp -i "$BASE/s/stream-late")
check "stream-late: no Set-Cookie (documented)" "no" "$(has_hdr "$resp" "Set-Cookie")"

resp=$(curlp -i "$BASE/assets/probe.txt")
check "static: Set-Cookie injected" "yes" "$(has_hdr "$resp" "Set-Cookie")"
check "static: body passthrough" "sess-asset-123" "$(echo "$resp" | tail -1)"

resp=$(curlp -i "$BASE/s/before")
check "before short-circuit: Set-Cookie" "yes" "$(has_hdr "$resp" "Set-Cookie")"
check "before short-circuit: body" "before-short=before" "$(echo "$resp" | tail -1)"

# 伪造 sid → 拒绝并新发合法 64hex
resp=$(curlp -i -H "Cookie: tphp_sid=zzz" "$BASE/s/id")
sid=$(echo "$resp" | tail -1 | sed 's/^id=//')
if echo "$sid" | grep -Eq '^[0-9a-f]{64}$'; then
    check "forged sid rejected -> new 64hex" "valid" "valid"
else
    check "forged sid rejected -> new 64hex" "64hex" "$sid"
fi
check "forged sid not echoed" "diff" "$([ "$sid" = "zzz" ] && echo same || echo diff)"

# GC：造过期文件（mtime 早于 lifetime=5s）→ gc_interval=1 后一次带会话请求即清
mkdir -p "$SESSDIR"
touch -d '2 hours ago' "$SESSDIR/gc_stale_file" 2>/dev/null || touch -t 202401010000 "$SESSDIR/gc_stale_file"
sleep 1
curlp "$BASE/s/set" >/dev/null
check "gc removes stale file" "gone" "$([ -e "$SESSDIR/gc_stale_file" ] && echo exists || echo gone)"

kill $PID 2>/dev/null
sleep 1
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null

rm -rf "$SESSDIR" "$ASSETDIR" "$JAR"

# ---------- 摘要 ----------
echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "-- server log --"; tail -15 "$LOG" 2>/dev/null; }
[ "$FAIL" -eq 0 ]
