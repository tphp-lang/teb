#!/bin/bash
# example/ 冒烟测试：逐个起服 -> curl 断言 -> 杀进程
export PATH="/c/env/msys2/clang64/bin:$PATH"
cd /c/project/tphp/teb || exit 1
TPHP_BIN="${TPHP_BIN:-/c/project/tphp/TinyPHP.rt/build/tphp.exe}"
PASS=0; FAIL=0
ok(){ if [ "$2" = "1" ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
wait_port(){ for i in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$1" 2>/dev/null && return 0; sleep 0.2; done; return 1; }
run_case(){ # $1=script $2=port  (starts server, runs body fn, kills)
  "$TPHP_BIN" run "example/$1" > /tmp/teb_ex_$1.log 2>&1 &
  SPID=$!
  wait_port "$2" || { echo "  server $1 not up"; FAIL=$((FAIL+1)); kill $SPID 2>/dev/null; return 1; }
}

echo "== 01-routes =="
run_case 01-routes.php 8081 && {
  R=$(curl -s http://127.0.0.1:8081/exact); [ "$R" = "exact match" ] && ok exact 1 || ok "exact [$R]" 0
  R=$(curl -s http://127.0.0.1:8081/api/user); echo "$R" | grep -q "prefix match, uri=/api/user" && ok prefix 1 || ok "prefix [$R]" 0
  C=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8081/no/such/path); [ "$C" = "404" ] && ok nf404 1 || ok "nf404 [$C]" 0
  R=$(curl -s http://127.0.0.1:8081/no/such/path); echo "$R" | grep -q "404 not found: /no/such/path" && ok nfbody 1 || ok "nfbody [$R]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 02-request =="
run_case 02-request.php 8082 && {
  R=$(curl -s "http://127.0.0.1:8082/?page=2&tag=php" -H "X-Token: abc" -b "sid=42")
  echo "$R" | grep -q '"queryParam":"2"' && ok qparam 1 || ok "qparam [$R]" 0
  echo "$R" | grep -q '"header":"abc"' && ok header 1 || ok "header [$R]" 0
  echo "$R" | grep -q '"cookie":"42"' && ok cookie 1 || ok "cookie [$R]" 0
  echo "$R" | grep -q '"isSsl":false' && ok isssl 1 || ok "isssl [$R]" 0
  R=$(curl -s -X POST http://127.0.0.1:8082/echo -d "hello=world")
  [ "$R" = "hello=world" ] && ok echo 1 || ok "echo [$R]" 0
  R=$(curl -s -X POST http://127.0.0.1:8082/json -H "Content-Type: application/json" -d '{"n":1}')
  echo "$R" | grep -q '"got":{"n":1}' && ok json 1 || ok "json [$R]" 0
  R=$(curl -s -X POST http://127.0.0.1:8082/form -d "a=1&b=x")
  echo "$R" | grep -q '"b":"x"' && ok form 1 || ok "form [$R]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 03-response =="
run_case 03-response.php 8083 && {
  H=$(curl -s -i http://127.0.0.1:8083/chain)
  echo "$H" | head -1 | grep -q "201" && ok chain201 1 || ok "chain201 [$(echo "$H"|head -1)]" 0
  echo "$H" | grep -qi "X-Engine: tphp/teb" && ok chainhdr 1 || ok chainhdr 0
  echo "$H" | grep -qi "Set-Cookie: seen=yes" && ok chaincookie 1 || ok chaincookie 0
  R=$(curl -s http://127.0.0.1:8083/json); echo "$R" | grep -q '"ok":true' && ok factory_json 1 || ok "factory_json [$R]" 0
  C=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8083/redirect); [ "$C" = "302" ] && ok redir302 1 || ok "redir302 [$C]" 0
  R=$(curl -sL http://127.0.0.1:8083/redirect); [ "$R" = "链式构造完成" ] && ok redirfollow 1 || ok "redirfollow [$R]" 0
  C=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8083/moved); [ "$C" = "301" ] && ok redir301 1 || ok "redir301 [$C]" 0
  R=$(curl -s http://127.0.0.1:8083/file); echo "$R" | grep -q "note.txt" && ok resfile 1 || ok "resfile [$R]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 04-upload =="
run_case 04-upload.php 8084 && {
  R=$(curl -s -F "f=@README.md" -F "note=manually-typed" http://127.0.0.1:8084/upload)
  echo "$R" | grep -q '"clientName":"README.md"' && ok upname 1 || ok "upname [$R]" 0
  echo "$R" | grep -q '"note":"manually-typed"' && ok upform 1 || ok "upform [$R]" 0
  echo "$R" | grep -q '"isMoved":true' && ok upmoved 1 || ok "upmoved [$R]" 0
  echo "$R" | grep -q '"totalFiles":1' && ok upcount 1 || ok "upcount [$R]" 0
  LS=$(ls /c/project/tphp/teb/example/_up_tmp/ 2>/dev/null | grep -c "saved-")
  [ "$LS" -ge 1 ] && ok upsaved 1 || ok "upsaved [$LS]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 05-stream =="
run_case 05-stream.php 8085 && {
  R=$(curl -s -N --max-time 10 http://127.0.0.1:8085/events)
  echo "$R" | grep -q "data: tick 5" && ok stream5 1 || ok "stream5" 0
  echo "$R" | grep -q "data: done" && ok streamdone 1 || ok "streamdone" 0
  C=$(curl -s -o /dev/null -w '%{size_download}' http://127.0.0.1:8085/big); [ "$C" = "100001" ] && ok bigbody 1 || ok "bigbody [$C]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 06-middleware-static =="
run_case 06-middleware-static.php 8086 && {
  R=$(curl -s http://127.0.0.1:8086/assets/); echo "$R" | grep -q "static file demo" && ok stindex 1 || ok "stindex [$R]" 0
  R=$(curl -s http://127.0.0.1:8086/assets/note.txt); echo "$R" | grep -q "text/plain" && ok sttxt 1 || ok "sttxt [$R]" 0
  C=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8086/assets/secret.txt); [ "$C" = "403" ] && ok stsecret403 1 || ok "stsecret403 [$C]" 0
  R=$(curl -s http://127.0.0.1:8086/api); echo "$R" | grep -q "api ok" && ok api 1 || ok "api [$R]" 0
  H=$(curl -si http://127.0.0.1:8086/api); echo "$H" | grep -qi "X-Middleware: passed-before" && ok apihdr 1 || ok "apihdr" 0
  R=$(curl -s http://127.0.0.1:8086/stats); echo "$R" | grep -q '"requests_total"' && ok stats 1 || ok "stats [$R]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 07-websocket =="
run_case 07-websocket.php 8087 && {
  R=$(WS_PORT=8087 WS_PATH=ws WS_MSG=hello-room "$TPHP_BIN" run tests/wsclient.php 2>&1)
  echo "$R" | grep -q "HANDSHAKE=1" && ok wshs 1 || ok wshs 0
  echo "$R" | grep -q "RECV2=\[127.0.0.1\] hello-room (bits=1)" && ok wsecho 1 || ok "wsecho [$R]" 0
  R2=$(WS_PORT=8087 WS_PATH=ws WS_MSG=ping "$TPHP_BIN" run tests/wsclient.php 2>&1)
  echo "$R2" | grep -q "pong" && ok wsping 1 || ok "wsping [$R2]" 0
  R3=$(WS_PORT=8087 WS_PATH=ws WS_MSG=close "$TPHP_BIN" run tests/wsclient.php 2>&1)
  echo "$R3" | grep -q "CLOSE_EARLY=server-bye" && ok wsclose 1 || ok "wsclose [$R3]" 0
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

echo "== 08-drain =="
"$TPHP_BIN" run example/08-drain.php > /tmp/teb_ex_08.log 2>&1 &
SPID=$!
wait_port 8088 && {
  R=$(curl -s http://127.0.0.1:8088/); [ "$R" = "drain demo" ] && ok drainroot 1 || ok "drainroot [$R]" 0
  wait $SPID   # 15s 自然退出
  grep -q "STATS=" /tmp/teb_ex_08.log && ok drainstats 1 || ok drainstats 0
  grep -q "drain demo exit" /tmp/teb_ex_08.log && ok drainexit 1 || ok drainexit 0
} || { echo "  server 08 not up"; FAIL=$((FAIL+1)); kill $SPID 2>/dev/null; }

echo
echo "RESULT: PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ]
