# API 分组表

## `namespace` 命名空间 `use Tphp\Teb;`

### `class` 类名 `Web`

服务器生命周期对象。除 `run` / `drain` / `stop` 外均返回 `self`（可链式）。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `__construct` | - | - | 创建服务器实例 |
| `route` | `string` `$uri` 路由路径 <br> `callable` `$cb` 回调 `function (Request $r): Response` | self | 注册路由（**任意方法**），start 前后均可；stop 清空路由表，重启后需重新注册 |
| `get` / `post` / `put` / `patch` / `delete` / `head` / `options` | 同 `route` | self | **方法快捷注册**（批A）：仅响应对应 HTTP 方法；同路径可共存多个方法路由；路径匹配但方法全不符 → `405 Method Not Allowed`（带 `Allow` 头，先于 notFound，不进回调）。`$uri` 支持 `/user/{id}` 路径模板（参数经 `Request::routeParam()` 读取，规则见 README 路由匹配节）；非法模板抛 `ValueError` |
| `notFound` | `callable` `$cb` 回调 | self | 未命中回调（单条，重复调用替换）；注册后覆盖静态回退与最长兜底；Request 的 `uri()` 为原始未匹配路径 |
| `before` | `callable` `$cb` 中间件 `function (Request $r)` | self | 全局中间件按注册序执行：返回 `null` 继续、`Response` 短路、其他返回值或异常 → 500；可拦静态文件；内可 `beginStream()` 走流式契约 |
| `file` | `string` `$uriPrefix` URI 前缀 <br> `string` `$dir` 目录 | self | 静态目录映射（可多条）；匹配在路由之后、nf/兜底之前；rest 逐段拒绝 `..`；目录取 index.html；缺失 fallthrough 不遮蔽 nf/兜底 |
| `websocket` | `string` `$uri` 路径 <br> `callable` `$onOpen` `function (WebSocket $s, Request $r)` <br> `?callable` `$onMessage` = null `function (WebSocket $s, string $data, int $bits)` <br> `?callable` `$onClose` = null `function (WebSocket $s)` | self | 注册 WebSocket；`$bits` 为消息 opcode（1=文本 2=二进制）；onMessage 异常 → 关连接，onOpen/onClose 异常忽略；客户端 PING/PONG/CLOSE 帧不进 onMessage |
| `start` | `array` `$options` 配置项（值全部为 string，整数需 `(string)` 转换） | self | 启动服务器（civetweb 配置直通）；未知键抛 `Error`；同一进程只允许一个 server；配置项见下方 |
| `run` | - | - | 阻塞主循环（内部逐批处理请求）直到 `stop()` |
| `drain` | - | - | 单次泵（run 内部也用它）：处理当前挂起请求后返回；无请求阻塞至多 10ms（有请求立即唤醒）。自控事件循环时替代 `run()`。池线程是**额外**消费者（`callback_threads>0` 时 HTTP 请求可能由池处理，本调用只兜主线程份额；WS 请求恒由主线程处理） |
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
| `session_save_path` | **teb 自留键**：会话文件目录（缺省系统临时目录下 `tphp_sess`；父目录须存在，目录缺失时自动创建） | `"/var/lib/tphp/sess"` |
| `session_lifetime` | **teb 自留键**：会话空闲过期秒数（缺省 `1209600`，14 天）。cookie `Max-Age` 与服务端文件 TTL 同值 | `"86400"` |
| `session_gc_interval` | **teb 自留键**：会话目录过期文件清扫的节流秒数（缺省 `300`） | `"60"` |
| `callback_threads` | **teb 自留键**：HTTP 回调执行池线程数（缺省 `min(4, CPU核数)`）。`"0"` 关池（纯主线程串行，逃生舱）。每个池线程是独立 runtime（见「线程模型」节） | `"8"` |
| `enable_keep_alive` | civetweb 键，**teb 缺省注入 `yes`**（显式传值优先）：HTTP/1.1 持久连接；一条持久连接占一个 worker 线程（空闲 `keep_alive_timeout_ms` 缺省 500ms 回收），`num_threads` 按峰值常连接数配置 | `"no"` |

- 自留键（`upload_tmp_dir` / `upload_max_bytes` / `session_*` / `callback_threads`）被识别后摘除，不透传 civetweb；传入未知配置项会导致启动失败（抛 `Error`）。

#### 线程模型（执行池）

- **HTTP 回调缺省由执行池并行执行**：`callback_threads`（缺省 `min(4, CPU核数)`）个池线程各持独立 tphp runtime（类表/常量/globals/字符串 intern 全隔离），与主线程一起消费请求队列——单个慢回调不再阻塞全站；`callback_threads=0` 回到纯主线程串行（与旧版行为完全一致的逃生舱）。
- **WS 回调固定主线程**：onOpen/onMessage/onClose 只在主线程执行——跨帧/跨连接的 `global` 状态（如聊天室房间表）与主脚本种子同线程。
- **同请求内回调恒同线程**：before 链与路由回调在同一次分发里执行，`global` 在请求内传值不受池影响；跨**请求**的 globals/`define`/静态类属性按线程隔离（池线程看不到主脚本种子），跨请求共享状态请用 Session 或文件。
- **回调闭包跨线程克隆**：池启用时所有路由/中间件/WS 回调在 `start()` 序列化快照（运行期新增的注册在发布时快照），捕获值必须可克隆（对象需 `__cloneState`；内置 `Web` 支持 `use ($web)`），否则 `start()` 抛 `NotCloneableError`。`Request`/`Response`/`Stream`/`Session`/`WebSocket` 对象不跨线程（生命周期恒在处理它的线程内）。
- **autoload 跨线程边界（批B）**：
  1. `spl_autoload_register()` 必须在 `start()` **之前**调用——start 一次性序列化快照、池线程首泵导入；start 之后注册/更换**不传播**池线程（主线程仍生效）；
  2. autoload 回调必须可序列化（同闭包克隆规则），不可序列化时 start 打 warning 且池线程无 autoloader（类 miss 直接 `Error: Class not found`）；
  3. **start 之后动态加载的类不进池线程**——类表克隆只在池线程启动时发生一次（重复克隆引擎 fatal），推荐**所有类在 start() 前加载完毕**（主脚本编译期类 + start 前 require 的类都会进克隆源）；
  4. **池线程回调内 `require` 含类声明的文件：串行可用，并发 unsupported**——注册进引擎类表 master 的写入无锁，同名注册已做幂等去重（restart 克隆不会重复 fatal），但**并发** require 仍存在 check-then-put 与编译期静态状态的数据竞态（已知限制 R8，勿在池回调里并发动态加载新类；串行请求下的池内 autoload 按需加载是受支持的形态，见 smoke_pool）；
  5. **字面量 `new Foo()` 被共享字节码 ic 短路**：首个消费线程 resolve 后 inline cache 缓存类指针，**其余线程执行同一闭包时不再走 resolve/autoload**（ic 属冻结字节码，跨线程共享）；需要「每个线程各自触发 autoload」（如按线程隔离的懒加载统计）请用动态类名 `new $cls()`——`op_new_dyn` 每次解析、无 ic。取证/分支可用 `\Tphp\Teb\Internal::inPoolThread(): bool`（当前线程是否为池线程）。
- **stop 链**：池回调内 `$web->stop()` 只置信号，由主线程 `run()`/`drain()` 轮询后统一执行真停（shutdown → join 池 → 停服务器 → 释放配置）；主线程回调内 stop 语义与旧版相同（draining 中挂 pending_stop，批末真停）。

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
| `routeParams` | - | array | 路径模板参数关联数组（批A；值已 URL 解码；非模板路由/无参数返回 `[]`；结果缓存） |
| `routeParam` | `string` `$k` 参数名 <br> `mixed` `$default` = null 缺省值 | mixed | 单个路径模板参数（如 `/user/{id}` 的 `id`），缺失返回 `$default` |
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
| `session` | - | Session | 打开会话（**惰性**：首次调用读取/生成会话 id，同请求幂等缓存）；未调用不产生任何 cookie 与文件 I/O。行为契约与限制见 Session 节 |
| `beginStream` | `int` `$status` = 200 状态码 <br> `array` `$headers` = [] 响应头 | Stream | 开始流式响应（契约见 Stream）；幂等：重复调用返回同一实例 |

### `class` 类名 `Response`

回调的返回值，由 PHP 侧构造、链式配置；状态行、头部、`Set-Cookie`、`Content-Length` 拼装全部在 C 侧完成。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `__construct` | `string` `$body` = "" 响应体 <br> `int` `$status` = 200 状态码 <br> `array` `$headers` = [`"Content-Type" => "text/plain"`] 响应头 | - | 默认 Content-Type `text/plain` |
| `header` | `string` `$k` 键 <br> `string` `$v` 值 | self | 追加/覆盖响应头 |
| `cookie` | `string` `$k` 键 <br> `string` `$v` 值 <br> `array` `$opts` = [] 属性 | self | 设置 `Set-Cookie`；`$opts` 支持的属性键见下方子表 |
| `withHeaders` | `array` `$headers` 头表 | self | 批量合并响应头；传非数组抛 `TypeError` |
| `status` | `int` `$code` 状态码 | self | 修改状态码（默认 200） |
| `body` | - | string | 读取当前响应体 |

#### `cookie()` 的 `$opts` 属性键

| 键 | 类型 | 说明 |
| :--- | :--- | :--- |
| `path` | string | `Path` 属性 |
| `domain` | string | `Domain` 属性 |
| `expires` | int | Unix 时间戳 → `Expires` 属性（GMT IMF-fixdate 格式） |
| `max_age` | int | `Max-Age` 属性（秒） |
| `secure` | bool | 输出 `Secure` 标志 |
| `httponly` | bool | 输出 `HttpOnly` 标志 |
| `sameSite` / `same_site` | string | 仅 `"Lax"` / `"Strict"` / `"None"`（大小写不敏感） |

- 属性键**大小写不敏感**；未知键 → `ValueError`（拼错键静默丢安全标志比报错更危险）；类型不符 → `TypeError`；非法 `sameSite` 取值 → `ValueError`。
- `$v` / `path` / `domain` 含 CR/LF → `ValueError`（响应分割注入防线）。
- 属性按固定顺序拼接在值尾部：`Path → Domain → Expires → Max-Age → Secure → HttpOnly → SameSite`；不传 `$opts` 时输出与旧版完全一致（`k=v`）。

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

return new Response("remembered", 200)
    ->cookie("remember", $v, ["httponly" => true, "max_age" => 2592000,
                               "sameSite" => "Lax"]);

return Response::json(["ok" => true]);
return Response::redirect("/login");
```

### `class` 类名 `Session`

`$req->session()` 的产物——服务端会话（**不可手动 `new`**，构造器抛 `Error`）。数据在回调结束时自动落盘；**`Set-Cookie` 由 teb 在响应写出时自动注入**（覆盖正常响应、500 兜底、流式未发头、静态文件四类路径）。

| 方法名 | 参数 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `id` | - | string | 会话 id（64 位小写 hex） |
| `get` | `string` `$k` 键 <br> `mixed` `$default` = null 缺省值 | mixed | 取值，缺失返回 `$default` |
| `set` | `string` `$k` 键 <br> `mixed` `$v` 值 | self | 存值；顶层值必须是 null/bool/int/float/string/array，否则 `TypeError`；`destroy()` 之后调用会**换新 id 复活**会话（防会话固定） |
| `has` | `string` `$k` 键 | bool | 是否存在 |
| `remove` | `string` `$k` 键 | self | 删除一项 |
| `all` | - | array | 全部数据的**副本** |
| `destroy` | - | void | 立即删除服务端数据并下发清除 cookie（本响应 `Max-Age=0`） |
| `regenerateId` | - | self | 换新会话 id（防会话固定），数据保留，旧 id 服务端立即失效 |

#### 行为契约

- **cookie**：名 `tphp_sid`，值 64 位小写 hex；形态 `tphp_sid=<id>; Path=/; HttpOnly; SameSite=Lax; Max-Age=<session_lifetime>`；非法/缺失的入站 id 一律视为新会话（伪造防线）。
- **滑动续期**：每个持有会话的响应都重发 `Set-Cookie` 并刷新服务端数据文件——活跃会话永不过期；空闲超过 `session_lifetime` 后 cookie 与服务端数据同时过期。
- **存储**：`session_save_path` 目录下单文件 JSON 序列化（每 id 一个文件，写入走临时文件原子替换）；过期文件按 `session_gc_interval` 节流清扫。
- **惰性**：不调用 `$r->session()` 的请求不产生任何 Set-Cookie、不发生会话文件 I/O。
- 用户在 Response 上自行 `->cookie("tphp_sid", …)` 时，自动注入让位（不覆盖用户值）。
- 回调抛异常（→ 500）时会话数据仍会落盘，500 响应同样携带 Set-Cookie。

#### 限制

- 数据必须可 JSON 编码：`set()` 顶层拒绝对象/闭包（`TypeError`）；嵌套不可编码值不落盘（仅打印警告，旧数据保留）。
- 静态文件命中 `If-Modified-Since` 的 **304 响应不带自动 Set-Cookie**；登录态下发请走动态端点。
- 流式响应须**先 `session()` 后首次 `write()`** 才能拿到 Set-Cookie（响应头发出后无法追加）；之后才打开的会话本响应无 cookie。
- WebSocket 握手完成后才进入 `onOpen`，其内打开的会话**不下发 cookie**（数据仍照常落盘）。
- 同一 `session_save_path` **不可多进程共用**（无跨进程锁）。

```php
$web->route("/login", function (Request $r) {
    $s = $r->session();
    $s->set("user", "alice");
    return new Response("welcome");
});

$web->route("/whoami", function (Request $r) {
    return new Response("user=" . $r->session()->get("user", "-"));
});

$web->route("/logout", function (Request $r) {
    $r->session()->destroy();
    return new Response("bye");
});
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
- 回调抛异常：头已发 → 仅结束流（不写 500，客户端收到已发出的部分）；头未发 → 照常写 500；
- 会话注入：须**先 `$r->session()` 再首次 `write()`** 才能随响应头带出 Set-Cookie（见 Session 节限制）。

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
