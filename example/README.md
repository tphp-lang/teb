# teb 使用示例

`Tphp\Teb`（civetweb HTTP 服务扩展）的分步示例。每个文件独立可跑，
都是 `start()` + `run()`（阻塞常驻，Ctrl+C 停止）的最小骨架。

## 准备

```bash
# 依赖引擎与已安装的 teb 扩展包（tphp ext list 可查）
tphp run example/01-routes.php
```

## 示例一览

| 文件 | 端口 | 内容 |
| --- | --- | --- |
| hello.php | 8080 | 最小起步：Web/Request/Response |
| 01-routes.php | 8081 | 精确/前缀/notFound 匹配规则与优先级 |
| 02-request.php | 8082 | Request 解析助手全量回显（query/header/cookie/json/form/body） |
| 03-response.php | 8083 | Response 链式构造、工厂函数、redirect、file |
| 04-upload.php | 8084 | multipart 上传 file()/files()/moveTo |
| 05-stream.php | 8085 | 流式响应（SSE 风格 beginStream/write/end） |
| 06-middleware-static.php | 8086 | before 中间件链、静态目录映射、stats |
| 07-websocket.php | 8087 | WebSocket 聊天室（广播/ping/close） |
| 08-drain.php | 8088 | drain() 事件泵：自控节奏、定时退出、stats |

## 快速验证

```bash
curl http://127.0.0.1:8081/api/anything          # 前缀路由
curl "http://127.0.0.1:8082/?page=2" -b "sid=42" # 请求解析
curl -i http://127.0.0.1:8083/chain              # 响应头/状态码/Cookie
curl -F "f=@README.md" http://127.0.0.1:8084/upload
curl -N http://127.0.0.1:8085/events             # 流式（-N 关闭缓冲）
curl http://127.0.0.1:8086/assets/note.txt       # 静态文件
curl http://127.0.0.1:8086/assets/secret.txt     # 被 before 拦 403
tphp run tests/wsclient.php   # WS 客户端（仓库自带；默认连 8087/ws）
# WS_PORT=8087 WS_PATH=ws WS_MSG=ping tphp run tests/wsclient.php
# 08-drain 无需操作，15 秒后自然退出并打印 STATS
```

## 常用坑位

- 所有 `start()` 选项值都是字符串：`"listening_ports" => "8080"`。
- 流式/WS 回调不要声明 `: Response` 返回类型；手动输出后 `return null`。
- `moveTo()` 必须在本次请求回调内完成，回调返回后临时文件会被回收。
- `$r->form()` 会消费 body；multipart 场景拿文本字段用 form()、拿文件用 file()/files()。
- WS `onMessage` 的 `$bits` 是消息 opcode（1=文本、2=二进制）；PING/PONG/CLOSE
  控制帧由帧层自动处理，不会进回调。
- 引擎字符串 `^` 按数值语义执行（见主仓 bug/ 记录），手写 WS 客户端做掩码请用
  `chr(ord($a) ^ ord($b))`，参考 tests/wsclient.php。
