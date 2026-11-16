# API 分组表

## `namespace` 命名空间 `use Tphp\Teb;`

### `class` 类名 `Web`

服务器生命周期对象。除 `run` / `drain` / `stop` 外均返回 `self`（可链式）。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `__construct` | - | - | 创建服务器实例 |
| `route` | `string` `$uri` 路由路径 <br> `callable` `$cb` 回调 `function (Request $r): Response` | self | 注册路由，start 前后均可；stop 清空路由表，重启后需重新注册 |
| `notFound` | `callable` `$cb` 回调 | self | 未命中回调（单条，重复调用替换）；注册后覆盖静态回退与最长兜底；Request 的 `uri()` 为原始未匹配路径 |
| `before` | `callable` `$cb` 中间件 `function (Request $r)` | self | 全局中间件按注册序执行：返回 `null` 继续、`Response` 短路、其他返回值或异常 → 500；可拦静态文件；内可 `beginStream()` 走流式契约 |
| `file` | `string` `$uriPrefix` URI 前缀 <br> `string` `$dir` 目录 | self | 静态目录映射（可多条）；匹配在路由之后、nf/兜底之前；rest 逐段拒绝 `..`；目录取 index.html；缺失 fallthrough 不遮蔽 nf/兜底 |
| `websocket` | `string` `$uri` 路径 <br> `callable` `$onOpen` `function (WebSocket $s, Request $r)` <br> `?callable` `$onMessage` = null `function (WebSocket $s, string $data, int $bits)` <br> `?callable` `$onClose` = null `function (WebSocket $s)` | self | 注册 WebSocket；`$bits` 为消息 opcode（1=文本 2=二进制）；onMessage 异常 → 关连接，onOpen/onClose 异常忽略；客户端 PING/PONG/CLOSE 帧不进 onMessage |
| `start` | `array` `$options` 配置项（值全部为 string，整数需 `(string)` 转换） | self | 启动服务器（civetweb 配置直通）；未知键抛 `Error`；同一进程只允许一个 server；配置项见下方 |
| `run` | - | - | 阻塞主循环（内部逐批处理请求）直到 `stop()` |
| `drain` | - | - | 单次泵：处理当前挂起请求后返回；无请求阻塞至多 10ms（有请求立即唤醒）。自控事件循环时替代 `run()` |
| `stop` | - | - | 停止服务器、释放路由与 notFound；幂等；**回调内调用合法**（挂起至当前批处理完再真停）；重启后需重新 `route()`/`notFound()` |
| `stats` | - | array | `started_at` / `uptime` / `requests_total`（**含入口 413 被拒请求**） / `requests_active` / `responses`（按状态码，键为十进制码字符串，如 `"413"`） / `bytes_sent` / `ws_active` / `ws_frames`（数据消息数，分片合并计 1，控制帧不计） / `ws_closed`；任意时刻可调，start 时复位 |

#### start() 配置项

| 键 | 说明 | 示例值 |
| :--- | :--- | :--- |
| `document_root` | 文档根目录 | `"."` |
| `listening_ports` | 监听端口（可含 `ssl:` 前缀） | `"8080"` |
| `num_threads` | 工作线程数 | `"50"` |
| `request_timeout_ms` | 请求超时（毫秒） | `"30000"` |
| `max_request_size` | civetweb：**请求行+头**读缓冲上限（字节，缺省 16384）；头超限由 civetweb 直接回 **400**（Malformed message，PHP 侧无感知），**不限制请求体** | `"65536"` |
| `error_log_file` / `access_log_file` | 日志文件路径 | - |
| `ssl_certificate` | SSL 证书链文件 | - |
| `authentication_domain` | 认证域 | `"mydomain.com"` |
| `protect_uri` | 需要保护的路径列表 | `"uri1,uri2"` |
| `upload_tmp_dir` | **teb 自留键**：上传 tmp 目录（缺省系统临时目录） | `"/tmp/up"` |
| `upload_max_bytes` | **teb 自留键**：请求体/上传累计上限（字节，缺省 64M）。非 multipart 且声明 `Content-Length` 超限 → **入口 413** 不进回调；chunked/未声明长度读取超限 → `body()`/`json()`/`form()` **抛 `Error`**；multipart → `UploadedFile::error()=1` | `"10485760"` |

- 自留键被识别后摘除，不透传 civetweb；传入未知配置项会导致启动失败（抛 `Error`）。

### `class` 类名 `Request`

路由回调的入参（**不可手动 `new`**——构造器抛 `Error`）。请求行/头在回调进入时一次性缓存，方法无副作用；body 类信息惰性解析后缓存复用。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `method` | - | string | HTTP 方法（GET/POST/…） |
| `uri` | - | string | 原始 URI（路径 + query，与请求行一致） |
| `path` | - | string | 清理后的本地路径（防目录穿越） |
| `query` | - | string | `?` 后的查询串（无则 `""`） |
| `header` | `string` `$name` 头名 | ?string | 取请求头，**大小写不敏感**；不存在返回 `null` |
| `headers` | - | array | 全部请求头（`name => value`） |
| `body` | `int` `$maxLen` = 65536 返回截断长度 | string | 读请求体。**首次调用读满全部并缓存**（与 `json()`/`form()` 共用），按 `$maxLen` 截断返回。**请求体超 `upload_max_bytes` 抛 `Error`**（不缓存残缺数据；catch 后再调重复抛同一异常） |
| `remoteAddr` | - | string | 客户端 IP |
| `remotePort` | - | int | 客户端端口 |
| `isSsl` | - | bool | 是否 HTTPS |
| `httpVersion` | - | string | HTTP 版本（`"1.1"` 等） |
| `queryParams` | - | array | query string 解析为关联数组（urlencoded，后值覆盖前值；结果缓存） |
| `queryParam` | `string` `$k` 键 <br> `mixed` `$default` = null 缺省值 | mixed | 单个 query 参数，缺失返回 `$default` |
| `cookies` | - | array | 解析 `Cookie` 头（`k=v; k2=v2`；结果缓存） |
| `cookie` | `string` `$k` 键 <br> `mixed` `$default` = null 缺省值 | mixed | 单个 cookie，缺失返回 `$default` |
| `json` | `mixed` `$default` = null 缺省值 | mixed | body 按 JSON 解析（引擎 C 层 decode）；坏 JSON / 空 body 返回 `$default`；body 超限抛 `Error`（同 `body()`） |
| `contentType` | - | ?string | `Content-Type` 头原值；无则 `null` |
| `isJson` | - | bool | Content-Type（大小写不敏感）包含 `"json"` |
| `clientIp` | - | string | `X-Forwarded-For` 首段（trim），无头时回落 `remoteAddr()` |
| `contentLength` | - | int | 请求体长度（负值钳为 0） |
| `form` | - | array | 表单解析（惰性，结果缓存）：GET/HEAD 解析 query；`multipart/form-data` 走完整表单解析（**会耗尽 body，勿先调 `body()`**）；其余读 body 缓存后按 urlencoded 解析。**解析失败/文本字段超 `upload_max_bytes` 抛 `Error`**（不缓存半截文本；重复调用重复抛，完整接收的文件仍可 `file()`/`files()` 取） |
| `file` | `string` `$name` 字段名 | ?UploadedFile | 单个上传文件；非 multipart 或无此名返回 `null`；首次触发的表单解析失败同样抛 `Error` |
| `files` | - | array | 全部上传文件（`name => UploadedFile`；非 multipart 为空数组） |
| `beginStream` | `int` `$status` = 200 状态码 <br> `array` `$headers` = [] 响应头 | Stream | 开始流式响应（契约见 Stream）；幂等：重复调用返回同一实例 |

### `class` 类名 `Response`

回调的返回值，由 PHP 侧构造、链式配置；状态行、头部、`Set-Cookie`、`Content-Length` 拼装全部在 C 侧完成。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `__construct` | `string` `$body` = "" 响应体 <br> `int` `$status` = 200 状态码 <br> `array` `$headers` = [`"Content-Type" => "text/plain"`] 响应头 | - | 默认 Content-Type `text/plain` |
| `header` | `string` `$k` 键 <br> `string` `$v` 值 | self | 追加/覆盖响应头 |
| `cookie` | `string` `$k` 键 <br> `string` `$v` 值 | self | 设置 `Set-Cookie` |
| `withHeaders` | `array` `$headers` 头表 | self | 批量合并响应头；传非数组抛 `TypeError` |
| `status` | `int` `$code` 状态码 | self | 修改状态码（默认 200） |
| `body` | - | string | 读取当前响应体 |

#### 静态工厂（native static，`Response::json(...)` 直接调用）

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `json` | `mixed` `$data` 数据 <br> `int` `$status` = 200 状态码 <br> `array` `$headers` = [] 响应头 | Response | `$data` JSON 编码为响应体；不可编码（对象/非法 UTF-8）抛 `Error`；默认 Content-Type `application/json`（用户 headers 可覆盖） |
| `text` | `string` `$s` 文本 <br> `int` `$status` = 200 <br> `array` `$headers` = [] | Response | 默认 Content-Type `text/plain` |
| `html` | `string` `$s` HTML <br> `int` `$status` = 200 <br> `array` `$headers` = [] | Response | 默认 Content-Type `text/html` |
| `redirect` | `string` `$url` 目标 URL <br> `int` `$status` = 302 <br> `array` `$headers` = [] | Response | 空 body、无默认 Content-Type；`Location` 最后强制写入（覆盖用户同名项） |
| `file` | `string` `$path` 文件路径 <br> `int` `$status` = 200 <br> `array` `$headers` = [] | Response | 读整个文件入 body（超 `upload_max_bytes` / 不可读抛 `Error`）；Content-Type 按扩展名 |

示例：

```php
return new Response("created", 201)
    ->header("X-Request-Id", $id)
    ->cookie("sid", $sid);

return Response::json(["ok" => true]);
return Response::redirect("/login");
```

### `class` 类名 `Stream`

`$req->beginStream()` 的产物——大响应/推送场景的分块写出。**惰性发头**：首次 `write()`（或 `end()`）才发送状态行 + 用户头 + `Connection: close`；**不带 Content-Length、不用 chunked**（连接关闭定界）。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `status` | `int` `$code` 状态码 | self | 头未发时可改状态码；已发/已结束抛 `Error` |
| `header` | `string` `$k` 键 <br> `string` `$v` 值 | self | 头未发时可加响应头；同上 |
| `write` | `string` `$chunk` 数据块 | int | 惰性发头后写出，返回字节数；已 `end()` 后抛 `Error` |
| `end` | `string` `$last` = "" 末块 | - | 结束流（幂等）；可携带最后一块 |

**回调契约**（drain 侧自动处理）：

- 回调内调用过 `beginStream()` → **回调返回值一律忽略**（`return null` 或返回 `Response` 均可）；
- 回调正常结束但未 `end()` → 自动收尾（未发头则发一个空体响应头）；
- 回调抛异常：头已发 → 仅结束流（不写 500，客户端收到已发出的部分）；头未发 → 照常写 500。

**写法注意**：流式回调的返回值被忽略，**不要声明 `: Response` 返回类型**再 `return null`——类型检查失败会抛 TypeError（当前引擎实现下该异常虽不影响本请求响应，但会污染后续请求的回调执行，修复前请按无返回类型声明书写）。

```php
$web->route("/events", function (Request $r) {
    $s = $r->beginStream(200, ["Content-Type" => "text/event-stream"]);
    foreach ($events as $e) {
        $s->write("data: " . $e . "\n\n");
    }
    $s->end();
});
```

### `class` 类名 `UploadedFile`

`$req->file()` / `$req->files()` 的产物——multipart 上传的单个文件。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `name` | - | string | 浏览器申报的原始文件名 |
| `type` | - | string | **服务端按扩展名推断的 mime**（`mg_get_builtin_mime_type`，未知 → `text/plain`）——不是浏览器申报值，与 `$_FILES['type']` 有差异 |
| `tmpName` | - | string | tmp 落盘路径；`moveTo()` 成功或超限后为 `""` |
| `size` | - | int | 文件字节数 |
| `error` | - | int | PHP `UPLOAD_ERR_*` 对齐：0=OK、1=超 `upload_max_bytes`、6=tmp 目录不可用 |
| `isMoved` | - | bool | 是否已 `moveTo()` |
| `moveTo` | `string` `$dest` 目标路径 | bool | 落盘：先尽力删除已存在目标，`rename` 失败回落 `copyfile`+`unlink`（全 libuv，UTF-8 路径安全）；已 moved / error≠0 / tmp 空返回 `false` |

**tmp 生命周期**：上传文件先落 `<upload_tmp_dir>/tphp_up_<pid>_<seq>`；**回调结束后 drain 自动回收所有未 `moveTo()` 的 tmp**——`moveTo()` 必须在路由回调内完成。

**已知差异**：空文件选择器（`filename=""`）无法与文本字段区分，呈现为文本字段空值（`error=4 NO_FILE` 不可达）；`type()` 是服务端推断值。

### `class` 类名 `WebSocket`

`$web->websocket()` 的 `onOpen` / `onMessage` 回调入参（连接生命周期随连接，onClose 后不可用）。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `send` | `string` `$data` 数据 <br> `bool` `$binary` = false 是否二进制帧 | int | 写帧（`mg_lock_connection` 防并发写），返回字节数；连接已关 / 写失败抛 `Error` |
| `ping` | `string` `$data` = "" 载荷 | bool | 发 PING 帧；失败返回 `false` |
| `close` | `string` `$data` = "" 关闭载荷 | - | 发 CLOSE 帧并断开（幂等）；置 close_requested 后本帧回调返回即断连 |
| `remoteAddr` | - | string | 客户端 IP；连接已关抛 `Error` |

**回调签名**（注册于 `Web::websocket`）：

- `onOpen(WebSocket $s, Request $r)`
- `onMessage(WebSocket $s, string $data, int $bits)` —— `$bits` 为消息 opcode：1=文本 2=二进制
- `onClose(WebSocket $s)` —— onMessage 抛异常 → 关连接；onOpen/onClose 异常忽略

**已知差异（WS 帧层）**：civetweb 的 data 回调给的是**已解 mask 的单帧载荷**（帧首字节 FIN|RSV|opcode 在 `bits` 入参）；teb 在 drain 侧做帧层处理——PING 自动回 PONG、PONG 忽略、CLOSE 回 CLOSE 后断开、FIN=0 分片按连接装配后交付，客户端 PING/PONG/CLOSE 帧不会进 onMessage。**曾误判为「未解 mask」**，实为测试客户端被引擎 `^` 语义坑（字符串 `^` 返回 int，掩码拼装须用 `chr(ord()^ord())`）。
