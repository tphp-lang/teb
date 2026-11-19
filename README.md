# Teb

`tphp/teb` 是基于 [civetweb](https://github.com/civetweb/civetweb) 的 HTTP 服务器扩展包（type=ext），以七个原生类的形式链入 tphp——`Tphp\Teb\Web` 服务器对象、`Tphp\Teb\Request` 请求对象、`Tphp\Teb\Response` 响应对象、`Tphp\Teb\Session` 会话对象、`Tphp\Teb\Stream` 流式响应、`Tphp\Teb\UploadedFile` 上传文件、`Tphp\Teb\WebSocket` 连接对象。回调收发全部对象化，无句柄、无手拼 HTTP 头、无轮询循环。

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

完整示例见 [example/hello.php](example/hello.php)。

## 会话（登录态）

```php
$web->route("/login", function (Request $r) {
    $r->session()->set("user", "alice");   // 惰性打开；Set-Cookie 自动注入
    return new Response("welcome");
});

$web->route("/whoami", fn(Request $r) =>
    new Response("user=" . $r->session()->get("user", "-")));

$web->route("/logout", function (Request $r) {
    $r->session()->destroy();              // 删数据 + 清 cookie
    return new Response("bye");
});
```

会话 id 为 64 位随机 hex（cookie 名 `tphp_sid`，`HttpOnly; SameSite=Lax`），文件存储于 `session_save_path`（缺省系统临时目录下 `tphp_sess`），空闲 `session_lifetime`（缺省 14 天）后过期，活跃会话滑动续期。完整行为契约与限制见 [doc/API.md](doc/API.md) 的 Session 节。

## 测试

```bash
bash tests/run.sh              # 总冒烟：36 项断言，约 40s（路由/请求/上限/keep-alive 复用与降级）
bash tests/run_p0.sh           # P0 冒烟：45 项断言（流式/上传/notFound/助手/工厂/stop）
bash tests/run_p1.sh           # P1 冒烟：30 项断言（before/静态/stats/WS 含帧层）
bash tests/run_session.sh      # Session/cookie opts 冒烟：33 项断言（会话链/注入/过期/GC）
bash tests/run_route2.sh       # 路由批A 冒烟：19 项断言（{param}/方法分发/405/五档优先级）
bash tests/run_pool.sh         # 执行池冒烟：14 项断言（并行度/同会话并发写/池内 autoload 快照/池 stop/restart×3）
bash example/smoke_examples.sh # 示例冒烟：49 项断言
```

默认使用兄弟目录引擎 `../TinyPHP.rt/build/tphp.exe`，可用 `TPHP_BIN=...` 覆盖。
前置：`tphp ext --add <teb 包路径>` 链入 teb 后再跑。

覆盖范围：路由匹配（精确/前缀/最长兜底及优先级）、请求信息（method/uri 含
query/path/query/httpVersion/isSsl/remoteAddr）、header 大小写不敏感、POST
body 回显、Response 链式配置（status/header/cookie/withHeaders）、500 兜底
（非 Response 返回/回调抛异常）、Request 构造拦截、stop 幂等与同进程同端口
重启（重启后需重新 `route()` 注册）、无路由静态文件直通与 404、before 短路/
异常/拦静态、静态映射与穿越拒绝、stats 计数、WebSocket（握手/onOpen/onMessage
回显与 bits/PING-PONG/分片装配/服务端 CLOSE/onClose/ws 统计）、请求体上限
（声明 Content-Length 入口 413、chunked 读取超限抛 Error 可捕获、恰好等于
上限放行、multipart 文本字段超限 form() 抛 Error、入口 413 计入 stats、
请求头超 max_request_size 由 civetweb 回 400）、keep-alive（同连接复用、
HTTP/1.0 与 Connection: close 客户端降级、响应不带 close 头）、Session（Set-Cookie 格式与
滑动续期、jar 跨请求读写/计数持久化、异常路径 flush、regenerateId、空闲过期
换新 sid、destroy 清 cookie、惰性零副作用、流式先开/后开注入差异、静态与
before 路径注入、伪造 sid 拒绝、过期文件 GC）、`Response::cookie` 三参
opts（全属性字段、非法键 ValueError 可捕获）与执行池（4×500ms 慢回调并行
墙钟达标、同会话并发写零丢更新、池回调 stop 信号链、restart×3 重建克隆）。

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

完整 API 分组表见 **[doc/API.md](doc/API.md)**：`Web` / `Request` / `Response` / `Session` / `Stream` / `UploadedFile` / `WebSocket` 七个类的方法名、参数、返回值与说明，含 `start()` 配置项表与各对象行为契约（会话存储与注入限制、流式回调、上传 tmp 生命周期、WS 帧层已知差异）。

## 路由匹配规则（按优先级）

1. **精确匹配**：`"/hello"` 只匹配 `/hello`（纯字面量注册）；
2. **路径模板匹配**：`"/user/{id}"` 段级全匹配——段数相等、`{name}` 捕获单段（`[A-Za-z_][A-Za-z0-9_]*` 参数名，值自动 URL 解码，经 `routeParam()`/`routeParams()` 读取）；字面段逐字节相等；模板不参与前缀档、不匹配多余段（`/user/{id}` 不匹配 `/user/1/posts`）；
3. **前缀匹配**：`"/hello"` 匹配 `/hello/xxx`（纯字面量注册）；
4. **方法分发**：`get()/post()/put()/patch()/delete()/head()/options()` 限定方法，`route()` 任意方法；同路径可共存多个方法路由；**路径已匹配但方法全不符 → `405 Method Not Allowed`（带 `Allow` 头，在 notFound/兜底之前，不进回调）**；未知方法（如 PROPFIND）只命中 `route()` 注册；
5. **notFound**：注册了 `notFound()` 时，1-3 全未命中的请求进 notFound 回调（**覆盖静态回退与兜底**）；
6. **最长路由兜底**：无 notFound 时取注册 uri 最长的一条（**模板参与**，按原始模板串长度；此档不产路径参数，`routeParam()` 落缺省值）；
7. 全无命中交回 civetweb 默认处理（静态文件/404）。

字面注册优先于模板（`route("/lit/7")` 胜 `get("/lit/{id}")`）；非法模板（裸花括号/空参数名/重复参数名）在注册时抛 `ValueError`。

回调须返回 `Response` 实例；回调抛异常或返回非 `Response` 时自动回 `500 Internal Server Error`，服务不受影响（异常信息照常打印）。**流式回调例外**：契约见 [doc/API.md](doc/API.md) 的 Stream 节。

## 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `Tphp\Teb\VERSION` | `"0.1.0"` | 扩展版本 |
| `Tphp\Teb\FEATURES_DEFAULT` | `0` | 默认特性集 |
| `Tphp\Teb\FEATURES_CGI` | `4` | CGI 特性位 |

## 规则与限制

- **一个进程一个 server**：`start()` 后必须先 `stop()` 才能再次 `start()`；`stop()` 清空路由表与 notFound，重启后需重新注册。
- **回调在执行池执行（缺省 `callback_threads = min(4, CPU核数)`）**：每个池线程是独立 tphp runtime（类表/常量/globals 隔离），HTTP 请求由池线程 + 主线程并行消费——慢回调不再阻塞全站；`callback_threads=0` 关池回到纯主线程串行（逃生舱）。回调内可直接使用任意 PHP 能力（文件、数据库、协程等）；回调期间对应的服务器线程同步等待，`Request` 读到的连接不会被复用或关闭。
- **WS 回调固定主线程**：onOpen/onMessage/onClose 只由主线程执行——跨帧/跨连接的 `global` 状态（房间表等）与主脚本种子同线程，"跨回调共享状态用 global"的心智保持不变。
- **globals/define/静态类属性按线程隔离**：池线程看不到主脚本种子值，跨**请求**的共享状态请用 Session 或文件；同一次请求内的多个回调（before→路由）恒在同一线程，`global` 传值不受影响。
- **回调闭包捕获须可序列化**：池启用时路由/中间件回调在 start 时跨线程克隆，捕获对象需实现 `__cloneState`（内置 `Web` 已支持 `use ($web)`）；不可克隆捕获在 start 抛 `NotCloneableError`。
- **回调返回值**：必须是 `Response`（或流式回调——见 doc/API.md Stream 契约）；否则打 warning 并自动回 500。
- **multipart 勿先调 `body()`**：`form()` 的 multipart 分支会耗尽连接 body，之后 `body()`/`json()` 得到空串。
- **上传 `moveTo()` 须在回调内完成**：回调结束后未移动的 tmp 文件由 drain 自动回收。
- **配置值全部为字符串**：`start()` 的 options 值必须是 string，整数需显式转换；`upload_tmp_dir`/`upload_max_bytes`/`session_save_path`/`session_lifetime`/`session_gc_interval`/`callback_threads` 为 teb 自留键。
- **keep-alive 缺省开启**（`enable_keep_alive=yes`，显式传值可关）：一条持久连接占一个 civetweb worker 线程（空闲 `keep_alive_timeout_ms` 缺省 500ms 回收）——`num_threads` 请按峰值常连接数配置；404 错误页（civetweb 无 `error_pages` 配置时）恒 close，需要可复用的 404 请注册 `notFound()`。
- **请求大小是两层限制**：请求行+头看 `max_request_size`（civetweb，缺省 16K，头超限 civetweb 直接回 400，PHP 侧无感知）；请求体看 `upload_max_bytes`（缺省 64M）——声明 `Content-Length` 的非 multipart 请求超限在入口直接 413（回调不执行，计入 stats），chunked/未声明长度读取超限由 `body()`/`json()`/`form()` 抛 `Error`（不 catch → 500），multipart 文件超限保留 `UploadedFile::error()=1` 语义、文本字段超限则 `form()` 抛 `Error`（重复抛；完整接收的文件仍可 `file()`/`files()` 取）。
- **编译与功能**：HTTPS/WebSocket/IPv6/CGI/缓存/统计全量启用；gzip 仅 Linux/macOS 默认启用（见「功能特性」「跨平台编译」两节）。
- **会话**：文件存储在 `session_save_path`（缺省系统临时目录下 `tphp_sess`），仅调用过 `$r->session()` 的请求才下发 cookie 与读写文件；Set-Cookie 自动注入覆盖正常/500/流式未发头/静态四类路径，静态 304、流式首次 `write()` 之后、WS `onOpen` 三处不注入；同一 `session_save_path` 不可多进程共用。完整契约与限制见 doc/API.md 的 Session 节。
