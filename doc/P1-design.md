# teb 对象 API P1 设计稿（before / WebSocket / 静态文件 / stats）

> 状态：**提案稿**（P0 设计见 [P0-design.md](P0-design.md)，P0 实现先行，P1 随后）。
> P2 内容历史上没定过，见文末候选清单（未冻结）。
> 本文以代码形态说明契约；实现前先与用户确认本稿。

## 总览

| 能力 | API |
|---|---|
| 全局中间件 | `$web->before(callable): self` |
| WebSocket | `$web->websocket(string $uri, callable $onOpen, ?callable $onMessage = null, ?callable $onClose = null): self` + `WebSocket` 连接对象 |
| 静态文件 | `$web->file(string $uriPrefix, string $dir): self` + `Response::file(string $path, ...)` |
| 运行统计 | `$web->stats(): array` |

---

## 1. before —— 全局中间件链

```php
$web->before(fn(Request $r): ?Response => ...): self;   // 可注册多条，按注册序执行
```

- drain 中每个 HTTP 请求（路由 / 静态 / nf 命中的都要过）**先构造同一个 Request
  实例**，依次调 before：
  - 返回 `null` → 继续下一条；
  - 返回 `Response` → **短路**：写该响应，后续 before 与路由全部跳过；
  - 返回其它类型 → 与 route 契约一致：警告 + 500；
  - 抛异常 → 500（`tphp_vm_call_bounded` 照常打印）；
  - before 里调 `$r->beginStream()` → 立即停止后续 before/路由，走 P0 流式收尾契约
    （返回值忽略、未 end 自动收尾、已发头异常不写 500）。
- before 全部返回 null → 进入匹配执行（路由 → 静态文件 → nf → 兜底）。
- WebSocket 事件**不走** before（文档明示）。
- 与 `route()` 相同：start 前后均可注册；stop 时随 routes 一起释放。

实现要点：`TebGlobal` 增 `TebRoute *befores`（同 routes 链表结构复用）；
drain 中把"跑 before 链 + 分发"抽成共用函数，route/nf/static 三条路都走它。

## 2. WebSocket

```php
$web->websocket('/ws',
    fn(WebSocket $s, Request $r) => ...,          // 握手完成（ready）时调用
    ?fn(WebSocket $s, string $data, int $bits) => ...,  // 每帧；bits=首字节 opcode
    ?fn(WebSocket $s) => ...                      // 连接关闭时
): self;

$s->send(string $data, bool $binary = false): int;  // mg_websocket_write，返回字节数
$s->ping(string $data = ""): bool;
$s->close(string $data = "");                      // 发 close 帧 + 置位 → 本帧回调后断开
$s->remoteAddr(): string;
```

### 线程模型（沿用 P0 同步阻塞 thunk）

- civetweb WS 回调跑在 worker 线程：`ready/data/close` → **入队 + 阻塞**，
  PHP 主线程 drain 分发给用户回调 → 置 done → worker 醒来继续。
  与请求队列**统一**：`TebRequest.kind ∈ {REQ, STATIC, WS_OPEN, WS_MSG, WS_CLOSE}`，
  MSG 节点携带 data 的 memcpy 拷贝（civetweb 给的缓冲出回调即失效）与 bits。
- `connect`（握手前）回调 P1 不挂 PHP（一律放行，返回 0）；拒绝握手留 P2。
- `data_handler` 返回值：回调跑完后看 `WebSocket.close_requested` → 已 close 返回 0
  （civetweb 断开），否则返回 1 保持连接。
- 分发契约：onMessage 返回值忽略；onMessage 抛异常 → 打印 + **关闭该连接**（返回 0），
  不涉及 HTTP 500。onOpen/onClose 抛异常 → 打印 + 忽略（onOpen 异常不断连，文档注明）。
- 注册：`mg_set_websocket_handler(ctx, uri, connect=放行, ready, data, close, NULL)`；
  每条 websocket() 一条注册（start 时批量挂，运行中注册则立即挂）。升级请求由
  civetweb 交给 WS handler（优先于 "/*" request handler）；同一 URI 的普通 GET 仍走
  请求 thunk → 路由/nf。

### 对象生命周期（防 UAF，必须照做）

- 每个事件 drain 现场 `tphp_obj_alloc` 一个 `WebSocket`（props: 0=conn
  1=close_requested），回调结束后 release；用户捕获可延长存活。
- **登记表**：`g_teb.ws_socks` 单链（node: obj+conn+next），创建时入链；
  `WebSocket` 类挂 `native_dtor` 出链释放 node。
- 连接 close 事件处理完、以及 `teb_real_stop` 时：遍历登记表把匹配 conn（stop 时
  全部）的 `props[0]` 置 0 —— 此后 `send/ping/close` 抛 Error("connection closed")。
  时序安全：close 事件处理期间 worker 阻塞在队列上，conn 尚未被 civetweb 释放。
- `send` 内 `mg_lock_connection` / `mg_unlock_connection` 包住 `mg_websocket_write`
  （防 civetweb 侧并发写出）。

## 3. 静态文件

```php
$web->file(string $uriPrefix, string $dir): self;   // 可注册多条
Response::file(string $path, int $status = 200, array $headers = []): Response;
```

### Web::file（目录前缀，零 PHP 内存直通）

- thunk 匹配顺序（P0 顺序上的插队）：
  **精确/前缀路由 → 静态文件 → nf → 最长兜底 → return 0**。
  - 静态命中 = uri 前缀匹配 **且** 解析出的文件存在（uv_fs_stat）；
    文件不存在 → 继续走 nf/兜底（nf 不被静态遮蔽，文档明示）。
  - 前缀不匹配或解析失败（含路径穿越）→ 继续后续匹配。
- 路径解析：strip `uriPrefix` + `local_uri`（civetweb 已解码）剩余段 → `dir + "/" + rest`；
  逐段拒绝 `..`；目录 → 尝试 `index.html`，不存在则视为未命中。
- 命中 → 入队 `STATIC(path)` 节点 → drain：先跑 before 链（中间件能拦静态），
  未短路则 `mg_send_mime_file(conn, path, NULL)`（mime 按扩展名自动，整响应直发）
  → 记 stats 200 + bytes += 文件大小。
- 运行中注册：thunk 读 `g_teb.files` 与 routes 同款（只读指针，注册即挂）。

### Response::file（单文件，进响应体）

- 读整个文件入 body（`uv_fs_*`，UTF-8 路径），Content-Type 按扩展名
  （`mg_get_builtin_mime_type`），Content-Length 随 body 自然带；
  文件超 `upload_max_bytes` → `Error`（防内存爆，文档建议大文件走 `$web->file`）。
- 不存在 / 不可读 → `Error`。

## 4. stats —— 运行统计

```php
$web->stats(): array;
/*
[
  'started_at'       => int,   // unix 时间；未启动为 0
  'uptime'           => int,   // 秒
  'requests_total'   => int,   // drain 处理过的 HTTP 请求数（含 nf/静态）
  'requests_active'  => int,   // 本批正在处理中
  'responses'        => [200 => n, 404 => n, ...],  // 按写出状态码计数
  'bytes_sent'       => int,   // PHP 侧 mg_write 累计（含流式/500）；
                               // mg_send_mime_file 内部写出只计文件体字节（文档注明近似）
  'ws_active'        => int,   // 当前打开的 WS 连接
  'ws_frames'        => int,   // 收到的帧总数
  'ws_closed'        => int,   // 累计关闭数
]
*/
```

实现要点：
- 计数器全部放 `TebGlobal`（drain/回调均在 PHP 主线程，写入无锁；thunk 里
  requests 也在入队后由 drain 计数 → 无跨线程写）。
- 引入 `teb_wr(conn, buf, len)` 统一替换直调 `mg_write`（顺带累加 bytes_sent；
  流式 `Stream::write`、`teb_write_response`、500、静态文件全走它）。
- 状态码计数挂三个出口：`teb_write_response`、`teb_write_500`、Stream 头发出、
  静态直通（200）。
- 计数器在 `Web::start` 复位；`stats()` 任意时刻可调（未启动返回全 0 + 当前时间）。

## 5. 实现顺序（P0 完成后）

1. `teb_wr` 统一写出口 + TebGlobal 计数器 + `Web::stats`（最小侵入，先行冒烟）。
2. before 链（drain 分发重构：before → route/nf 分发抽共用）。
3. `Response::file` + `$web->file`（STATIC 节点 + 路径解析 + mg_send_mime_file）。
4. WebSocket（队列 kind 扩展 + WS 回调 + WebSocket 类 + 登记表生命周期）。
5. 文档 README 增补 P1 章节 + 冒烟（curl 静态文件、curl 简易 WS 客户端连帧、
   before 短路/异常、stats 断言）+ 提交（不 push）。

依赖：无引擎侧改动（全部 teb.c + 文档）。

---

## P2 候选（**未冻结**，用户挑完再立项）

1. `Response::cookie` 结构化属性：path/domain/secure/httpOnly/expires（Set-Cookie 拼装）。
2. 静态文件缓存与续传：ETag/If-Modified-Since、Range 断点续传。
3. 访问日志：access log 格式化输出（可开关注入 stats 或独立流）。
4. CORS 辅助：预检与响应头预设。
5. WebSocket 握手前认证（connect 回调拒绝）+ 子协议
   （`mg_set_websocket_handler_with_subprotocols`）。
6. 优雅重启 restart / 路由与静态表热更新。
7. 简版 session：cookie id + 服务端存储。
