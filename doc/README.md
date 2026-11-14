# tphp/teb 使用文档

`tphp/teb` 是基于 [civetweb](https://github.com/civetweb/civetweb) 的 HTTP 服务器扩展包（type=ext），以三个原生类的形式链入 tphp——`Tphp\Teb\Web` 服务器对象、`Tphp\Teb\Request` 请求对象、`Tphp\Teb\Response` 响应对象。回调收发全部对象化，无句柄、无手拼 HTTP 头、无轮询循环。

内置 civetweb 2025 master 源码（`civetweb/` 目录），随包一起编译，无运行时动态库依赖。

## 安装

```bash
tphp ext --add <teb 包路径>     # 安装并编译链入 tphp
tphp ext --relink               # 修改扩展源码后重新编译
```

链入成功后，控制台会输出 `Linked N ext package(s) into tphp`。

## 快速开始

```php
<?php
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$web = new Web();

$web->route("/", fn(Request $r) => new Response("hello world"));

$web->route("/api/status", function (Request $r): Response {
    return new Response('{"ok":true}', 200, [
        "Content-Type" => "application/json",
    ]);
});

$web->start([
    "document_root" => __DIR__,
    "listening_ports" => "8080",
    "num_threads" => "50",
]);

$web->run();    // 阻塞常驻（Ctrl+C 停止；对象析构自动 stop）
```

完整示例见 [tests/hello.php](../tests/hello.php)。

## 测试

```bash
bash tests/run.sh        # 冒烟套件：22 项断言，约 40s
```

默认使用兄弟目录引擎 `../TinyPHP.rt/build/tphp.exe`，可用 `TPHP_BIN=...` 覆盖。
前置：`tphp ext --add <teb 包路径>` 链入 teb 后再跑。

覆盖范围：路由匹配（精确/前缀/最长兜底及优先级）、请求信息（method/uri 含
query/path/query/httpVersion/isSsl/remoteAddr）、header 大小写不敏感、POST
body 回显、Response 链式配置（status/header/cookie/withHeaders）、500 兜底
（非 Response 返回/回调抛异常）、Request 构造拦截、stop 幂等与同进程同端口
重启（重启后需重新 `route()` 注册）、无路由静态文件直通与 404。

## 功能特性

| 特性 | Windows | Linux | macOS | 说明 |
|---|---|---|---|---|
| HTTP/1.1 | ✓ | ✓ | ✓ | civetweb 基础能力 |
| HTTPS（TLS） | ✓ | ✓ | ✓ | OpenSSL **运行时动态加载**，编译/链接均不依赖 OpenSSL；系统有 libssl 时 `ssl:` 端口即生效 |
| CGI | ✓ | ✓ | ✓ | 默认开启（`NO_CGI` 未定义） |
| 静态文件缓存 | ✓ | ✓ | ✓ | 默认开启（`NO_CACHING` 未定义） |
| WebSocket | ✓ | ✓ | ✓ | `USE_WEBSOCKET` |
| IPv6 | ✓ | ✓ | ✓ | `USE_IPV6` |
| 服务器统计 | ✓ | ✓ | ✓ | `USE_SERVER_STATS` |
| gzip 响应压缩 | — | ✓ | ✓ | `USE_ZLIB` + `-lz`（见下） |

gzip 在 Linux/macOS 使用系统 zlib；Windows 默认不启用（避免引入第三方库依赖），需要时在 `mod.json` 的 `ext.os.windows` 中加 `"cflags": ["-DUSE_ZLIB"], "libs": ["-lz"]`（MSYS2 CLANG64 下 `pacman -S mingw-w64-zlib`）后重新 `ext --relink`。

未启用：Lua/duktape 脚本（需引入第三方运行时）、HTTP/2（civetweb 官方标注 prototype，不建议生产使用）。

## 跨平台编译（mod.json）

本包的编译与链接指令全部声明在 `mod.json` 的 `ext` 契约里，三平台分栏：

```json
"ext": {
  "sources": ["src", "civetweb"],          // 递归收集 *.c
  "include": ["."],                        // 包内 -I（civetweb/civetweb.h）
  "defines": ["OPENSSL_API_3_0", "USE_WEBSOCKET", "USE_IPV6", "USE_SERVER_STATS"],
  "cflags": ["-w"],
  "os": {
    "windows": { "cflags": [], "libs": ["-lwinpthread"], "ldflags": [] },
    "linux":   { "cflags": ["-DUSE_ZLIB"], "libs": ["-lz"], "ldflags": [] },
    "darwin":  { "cflags": ["-DUSE_ZLIB"], "libs": ["-lz"], "ldflags": [] }
  },
  "tphp_abi": "0.0.2"
}
```

- `defines` / `cflags` / `include`：三平台通用的编译指令；
- `os.<平台>.cflags/libs/ldflags`：仅该平台生效的编译与链接指令（`libs` 为 `-l` 形式；Windows 的 `-lwinpthread` 来自 civetweb 对 `clock_gettime` 的 time64 重定向）；
- 链接指令（`libs` + `ldflags`，含平台分栏）由 tphp 在 `ext --relink` 时自动附加到链接响应文件，包内无需自备构建脚本。

## API 参考

### Tphp\Teb\Web

服务器生命周期对象。所有方法返回 `$this`（可链式，`run`/`drain`/`stop` 除外）。

| 方法 | 签名 | 说明 |
|---|---|---|
| `route` | `(string $uri, callable $cb): self` | 注册路由；`$cb` 签名 `function (Request $r): Response`。可在 `start()` 前后任意调用 |
| `notFound` | `(callable $cb): self` | 无路由命中时的回调（单条，重复调用替换）。**注册后覆盖静态文件回退与最长兜底**；回调签名同 `route`，Request 的 `uri()` 为原始未匹配路径 |
| `start` | `(array $options): self` | civetweb 配置直通并启动服务器（见下表） |
| `run` | `: void` | 阻塞主循环（内部逐批处理请求），直到 `stop()` |
| `drain` | `: void` | 单次泵——处理当前挂起请求后返回；无请求时阻塞至多 10ms（有请求立即被唤醒）。需要自控事件循环时替代 `run()` |
| `stop` | `: void` | 停止服务器、释放路由与 notFound；幂等（无运行中服务器时静默返回）；**回调内调用合法**（挂起至当前批处理完再真停）。`stop()` 会清空路由表——重启后需重新 `route()`/`notFound()` |

`start()` 的 `$options` 常用项（全部为**字符串值**，整数需 `(string)` 转换）：

| 键 | 说明 | 示例值 |
|---|---|---|
| `document_root` | 文档根目录 | `"."` |
| `listening_ports` | 监听端口（可含 `ssl:` 前缀） | `"8080"` |
| `num_threads` | 工作线程数 | `"50"` |
| `request_timeout_ms` | 请求超时（毫秒） | `"30000"` |
| `error_log_file` / `access_log_file` | 日志文件路径 | — |
| `ssl_certificate` | SSL 证书链文件 | — |
| `authentication_domain` | 认证域 | `"mydomain.com"` |
| `protect_uri` | 需要保护的路径列表 | `"uri1,uri2"` |
| `upload_tmp_dir` | **teb 自留键**：上传 tmp 目录（缺省系统临时目录） | `"/tmp/up"` |
| `upload_max_bytes` | **teb 自留键**：上传与单请求 body 上限（字节；缺省 64M） | `"10485760"` |

- 传入未知配置项会导致启动失败（抛 `Error`）；同一进程同时只允许一个 server。自留键被识别后摘除，不透传 civetweb。

**路由匹配规则**（按优先级）：

1. **精确匹配**：`"/hello"` 只匹配 `/hello`；
2. **前缀匹配**：`"/hello"` 匹配 `/hello/xxx`；
3. **notFound**：注册了 `notFound()` 时，未命中 1/2 的请求进 notFound 回调（**覆盖静态回退与兜底**）；
4. 无 notFound 时取最长注册路由（fallback）；
5. 全无命中交回 civetweb 默认处理（静态文件/404）。

回调须返回 `Response` 实例；回调抛异常或返回非 `Response` 时自动回 `500 Internal Server Error`，服务不受影响（异常信息照常打印）。**流式回调例外**：见 Stream 章节契约。

### Tphp\Teb\Request

路由回调的入参（不可手动 `new`——构造器会抛 `Error`）。请求行/头信息在回调进入时一次性缓存，方法均无副作用；body 类信息（body/json/form）惰性解析后缓存复用。

| 方法 | 返回 | 说明 |
|---|---|---|
| `method()` | `string` | HTTP 方法（GET/POST/…） |
| `uri()` | `string` | 原始 URI（路径 + query，与请求行一致） |
| `path()` | `string` | 清理后的本地路径（防目录穿越） |
| `query()` | `string` | `?` 后的查询串（无则 `""`） |
| `header(string $name)` | `?string` | 取请求头，**大小写不敏感**；不存在返回 `null` |
| `headers()` | `array` | 全部请求头（`name => value`） |
| `body(int $maxLen = 65536)` | `string` | 读请求体。**首次调用读满全部并缓存**（与 `json()`/`form()` 共用缓存），按 `maxLen` 截断返回；重复调用读缓存 |
| `remoteAddr()` | `string` | 客户端 IP |
| `remotePort()` | `int` | 客户端端口 |
| `isSsl()` | `bool` | 是否 HTTPS |
| `httpVersion()` | `string` | HTTP 版本（`"1.1"` 等） |
| `queryParams()` | `array` | query string 解析为关联数组（urlencoded，后值覆盖前值；结果缓存） |
| `queryParam(string $k, $default = null)` | `mixed` | 单个 query 参数，缺失返回 `$default` |
| `cookies()` | `array` | 解析 `Cookie` 头（`k=v; k2=v2`；结果缓存） |
| `cookie(string $k, $default = null)` | `mixed` | 单个 cookie，缺失返回 `$default` |
| `json($default = null)` | `mixed` | body 按 JSON 解析（引擎 C 层 decode）；坏 JSON / 空 body 返回 `$default` |
| `contentType()` | `?string` | `Content-Type` 头原值；无则 `null` |
| `isJson()` | `bool` | Content-Type（大小写不敏感）包含 `"json"` |
| `clientIp()` | `string` | `X-Forwarded-For` 首段（trim），无头时回落 `remoteAddr()` |
| `contentLength()` | `int` | 请求体长度（负值钳为 0） |
| `form()` | `array` | 表单解析（惰性，结果缓存）：GET/HEAD 解析 query；`multipart/form-data` 走完整表单解析（**会耗尽 body，勿先调 `body()`**）；其余读 body 缓存后按 urlencoded 解析 |
| `file(string $name)` | `?UploadedFile` | 单个上传文件；非 multipart 或无此名返回 `null` |
| `files()` | `array` | 全部上传文件（`name => UploadedFile`；非 multipart 为空数组） |
| `beginStream(int $status = 200, array $headers = [])` | `Stream` | 开始流式响应（见 Stream 章节）。幂等：重复调用返回同一实例 |

### Tphp\Teb\Response

回调的返回值，由 PHP 侧构造，链式配置；HTTP 状态行、头部、`Set-Cookie`、`Content-Length` 的拼装全部在 C 侧完成。

```php
new Response(string $body = "", int $status = 200,
             array $headers = ["Content-Type" => "text/plain"])
```

| 方法 | 签名 | 说明 |
|---|---|---|
| `header` | `(string $k, string $v): self` | 追加/覆盖响应头 |
| `cookie` | `(string $k, string $v): self` | 设置 `Set-Cookie` |
| `withHeaders` | `(array $headers): self` | 批量合并响应头 |
| `status` | `(int $code): self` | 修改状态码（默认 200） |
| `body` | `: string` | 读取当前响应体 |

静态工厂（**native static**，`Response::json(...)` 直接调用）：

| 方法 | 签名 | 说明 |
|---|---|---|
| `json` | `($data, int $status = 200, array $headers = []): Response` | `$data` JSON 编码为响应体；不可编码（对象/非法 UTF-8）抛 `Error`；默认 Content-Type `application/json`（用户 headers 可覆盖） |
| `text` | `(string $s, int $status = 200, array $headers = []): Response` | 默认 Content-Type `text/plain` |
| `html` | `(string $s, int $status = 200, array $headers = []): Response` | 默认 Content-Type `text/html` |
| `redirect` | `(string $url, int $status = 302, array $headers = []): Response` | 空 body、无默认 Content-Type；`Location` 最后强制写入（覆盖用户同名项） |

示例：

```php
return new Response("created", 201)
    ->header("X-Request-Id", $id)
    ->cookie("sid", $sid);

return Response::json(["ok" => true]);
return Response::redirect("/login");
```

### Tphp\Teb\Stream

`$req->beginStream()` 的产物——大响应/推送场景的分块写出。**惰性发头**：首次 `write()`（或 `end()`）才发送状态行 + 用户头 + `Connection: close`；**不带 Content-Length、不用 chunked**（连接关闭定界）。

| 方法 | 签名 | 说明 |
|---|---|---|
| `status` | `(int $code): self` | 头未发时可改状态码；已发/已结束抛 `Error` |
| `header` | `(string $k, string $v): self` | 头未发时可加响应头；同上 |
| `write` | `(string $chunk): int` | 惰性发头后写出，返回字节数；已 `end()` 后抛 `Error` |
| `end` | `(string $last = ""): void` | 结束流（幂等）；可携带最后一块 |

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

### Tphp\Teb\UploadedFile

`$req->file()` / `$req->files()` 的产物——multipart 上传的单个文件。

| 方法 | 返回 | 说明 |
|---|---|---|
| `name()` | `string` | 浏览器申报的原始文件名 |
| `type()` | `string` | **服务端按扩展名推断的 mime**（`mg_get_builtin_mime_type`，未知 → `text/plain`）——不是浏览器申报值，与 `$_FILES['type']` 有差异 |
| `tmpName()` | `string` | tmp 落盘路径；`moveTo()` 成功或超限后为 `""` |
| `size()` | `int` | 文件字节数 |
| `error()` | `int` | PHP `UPLOAD_ERR_*` 对齐：0=OK、1=超 `upload_max_bytes`、6=tmp 目录不可用 |
| `isMoved()` | `bool` | 是否已 `moveTo()` |
| `moveTo(string $dest)` | `bool` | 落盘到 `$dest`：先尽力删除已存在目标，`rename` 失败回落 `copyfile`+`unlink`（全 libuv，UTF-8 路径安全）；已 moved / error≠0 / tmp 空返回 `false` |

**tmp 生命周期**：上传文件先落 `<upload_tmp_dir>/tphp_up_<pid>_<seq>`；**回调结束后 drain 自动回收所有未 `moveTo()` 的 tmp**——**`moveTo()` 必须在路由回调内完成**。

**已知差异**：空文件选择器（`filename=""`）无法与文本字段区分，呈现为文本字段空值（`error=4 NO_FILE` 不可达）；`type()` 是服务端推断值。


## P1：before / 静态文件 / stats / WebSocket（已落地）

| API | 签名 | 说明 |
|---|---|---|
| `Web::before` | `(callable $cb): self` | 全局中间件，按注册序执行；返回 `null` 继续、`Response` 短路、其它类型/异常 → 500；**可拦静态文件**；before 内 `beginStream()` 走流式契约 |
| `Web::file` | `(string $uriPrefix, string $dir): self` | 静态目录映射（多条）；匹配顺序在路由之后、nf/兜底之前；rest 逐段拒绝 `..`；目录取 `index.html`；缺失 fallthrough 不遮蔽 nf/兜底 |
| `Web::stats` | `(): array` | `started_at`/`uptime`/`requests_total`/`requests_active`/`responses`(按状态码)/`bytes_sent`/`ws_active`/`ws_frames`/`ws_closed`；任意时刻可调，start 时复位 |
| `Response::file` | `(string $path, int $status = 200, array $headers = []): Response` | 读整个文件入 body（超 `upload_max_bytes`/不可读抛 `Error`）；Content-Type 按扩展名 |
| `Web::websocket` | `(string $uri, callable $onOpen, ?callable $onMessage = null, ?callable $onClose = null): self` | 回调签名：`onOpen(WebSocket $s, Request $r)`、`onMessage(WebSocket $s, string $data, int $bits)`、`onClose(WebSocket $s)`；onMessage 抛异常 → 关连接；onOpen/onClose 异常忽略 |
| `WebSocket::send` | `(string $data, bool $binary = false): int` | 写帧（连接已关抛 `Error`） |
| `WebSocket::ping` / `close` / `remoteAddr` | 见源码 | `close` 发 CLOSE 帧并断开（幂等） |

**已知遗留（WS 帧层）**：civetweb 的 data 回调给的是原始掩码帧（未解 mask），`onMessage` 的 `$data` 当前为帧原始数据——回显类用法待下轮修帧解析（见 P1-design 教训节）。

## 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `Tphp\Teb\VERSION` | `"0.1.0"` | 扩展版本 |
| `Tphp\Teb\FEATURES_DEFAULT` | `0` | 默认特性集 |
| `Tphp\Teb\FEATURES_CGI` | `4` | CGI 特性位 |

## 规则与限制

- **一个进程一个 server**：`start()` 后必须先 `stop()` 才能再次 `start()`；`stop()` 清空路由表与 notFound，重启后需重新注册。
- **回调在主线程执行**：路由回调由主循环（`run()`/`drain()`）触发，回调内可直接使用任意 PHP 能力（文件、数据库、协程等）；回调期间对应的服务器线程同步等待，`Request` 读到的连接不会被复用或关闭。
- **回调返回值**：必须是 `Response`（或流式回调——见 Stream 契约）；否则打 warning 并自动回 500。
- **multipart 勿先调 `body()`**：`form()` 的 multipart 分支会耗尽连接 body，之后 `body()`/`json()` 得到空串。
- **上传 `moveTo()` 须在回调内完成**：回调结束后未移动的 tmp 文件由 drain 自动回收。
- **配置值全部为字符串**：`start()` 的 options 值必须是 string，整数需显式转换；`upload_tmp_dir`/`upload_max_bytes` 为 teb 自留键。
- **编译与功能**：HTTPS/WebSocket/IPv6/CGI/缓存/统计全量启用；gzip 仅 Linux/macOS 默认启用（见「功能特性」「跨平台编译」两节）。
