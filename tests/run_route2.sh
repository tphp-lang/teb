#!/usr/bin/env bash
# 批A 路由冒烟驱动（Git Bash）：方法分发 + {param} 路径参数 + 405 + 五档
# 用法：bash tests/run_route2.sh
set -u
cd "$(dirname "$0")/.."   # teb 仓根

TPHP_BIN="${TPHP_BIN:-../TinyPHP.rt/build/tphp.exe}"
PORT="${TEB_TEST_PORT:-18136}"
BASE="http://127.0.0.1:${PORT}"
LOG=/tmp/teb_smoke_route2.log
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

hdr_of() { # hdr_of "$response" "Header-Name"
    echo "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r'
}

curlp() { curl -s --noproxy '*' -m 5 "$@"; }

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

echo "== tphp/teb route2 smoke (methods + {params} + 405) =="
echo "tphp: $TPHP_BIN"

rm -f "$LOG"
"$TPHP_BIN" run tests/smoke_route2.php >"$LOG" 2>&1 &
PID=$!

if wait_ready "$BASE/literal" 15; then
    # ---- 模板与参数 ----
    check "template hit" "id=42" "$(curlp "$BASE/user/42")"
    check "mixed literal+param" "post-view id=42" "$(curlp "$BASE/user/42/posts")"
    check "two params via routeParams" "name=a kind=view" \
        "$(curlp "$BASE/file/a/view")"
    check "param url-decoded" "name=hello world" "$(curlp "$BASE/encoded/hello%20world")"

    # ---- 五档优先级 ----
    check "T1 literal beats T2 template" "lit7-wins" "$(curlp "$BASE/lit/7")"
    check "T2 template when no literal" "tpl-wins id=8" "$(curlp "$BASE/lit/8")"
    check "T3 literal prefix still works" "pre-hit" "$(curlp "$BASE/pre/anything")"
    # 最长注册 = /file/{name}/{kind}(18) > /user/{id}/posts(17) → 落 file 模板，
    # 兜底档不产参数（routeParams 全空 → default）
    check "T5 longest fallback = template w/ empty params" "name=- kind=-" \
        "$(curlp "$BASE/unmatched-zzz")"
    check "literal route + routeParam default" "lit dv=dv" "$(curlp "$BASE/literal")"

    # ---- 方法分发 ----
    check "get route" "g" "$(curlp "$BASE/same")"
    check "post same path" "p" "$(curlp -X POST "$BASE/same")"
    check "post-only hit" "onlypost-ok" "$(curlp -X POST "$BASE/onlypost")"
    r=$(curlp -i -X DELETE "$BASE/anyroute")
    check "route() accepts any method" "any-ok m=DELETE" "$(echo "$r" | tail -1)"
    r=$(curlp -i -X PROPFIND "$BASE/anyroute")
    check "unknown method hits ANY route" "any-ok m=PROPFIND" "$(echo "$r" | tail -1)"

    # ---- 405 ----
    r=$(curlp -i -X POST "$BASE/onlyget")
    check "405 status" "405" "$(echo "$r" | head -1 | grep -o ' 405 ' | tr -d ' ')"
    check "405 Allow header" "GET" "$(hdr_of "$r" "Allow")"
    body405=$(echo "$r" | awk 'BEGIN{b=0} /^\r?$/{b=1;next} b{print}' | tr -d '\r')
    check "405 body empty (not nf/fallback)" "" "$body405"
    r=$(curlp -i "$BASE/onlypost")
    check "405 other method Allow" "POST" "$(hdr_of "$r" "Allow")"

    # ---- 非法模板（运行期注册 raise 可捕获） ----
    check "invalid templates all raise ValueError" "caught=3" "$(curlp "$BASE/badtpl")"
else
    check "server ready" "ready" "timeout"
fi

kill $PID 2>/dev/null
sleep 1
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null

# ---------- 摘要 ----------
echo "------------------------------------------------------------"
echo "$RESULTS"
echo "------------------------------------------------------------"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || { echo "-- server log --"; tail -15 "$LOG" 2>/dev/null; }
[ "$FAIL" -eq 0 ]
