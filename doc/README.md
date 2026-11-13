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
| `start` | `(array $options): self` | civetweb 配置直通并启动服务器（见下表） |
| `run` | `: void` | 阻塞主循环（内部逐批处理请求），直到 `stop()` |
| `drain` | `: void` | 单次泵——处理当前挂起请求后返回；无请求时阻塞至多 10ms（有请求立即被唤醒）。需要自控事件循环时替代 `run()` |
| `stop` | `: void` | 停止服务器、释放路由；`__destruct` 自动调用 |

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

- 传入未知配置项会导致启动失败（抛 `Error`）；同一进程同时只允许一个 server。

**路由匹配规则**（按优先级）：

1. **精确匹配**：`"/hello"` 只匹配 `/hello`；
2. **前缀匹配**：`"/hello"` 匹配 `/hello/xxx`；
3. 无命中时取最长注册路由（fallback）。

回调须返回 `Response` 实例；回调抛异常或返回非 `Response` 时自动回 `500 Internal Server Error`（异常信息照常打印），服务不受影响。

### Tphp\Teb\Request

路由回调的入参（不可手动 `new`——构造器会抛 `Error`）。信息在回调进入时一次性缓存，方法均无副作用。

| 方法 | 返回 | 说明 |
|---|---|---|
| `method()` | `string` | HTTP 方法（GET/POST/…） |
| `uri()` | `string` | 原始 URI（含 query） |
| `path()` | `string` | 清理后的本地路径（防目录穿越） |
| `query()` | `string` | `?` 后的查询串（无则 `""`） |
| `header(string $name)` | `?string` | 取请求头，**大小写不敏感**；不存在返回 `null` |
| `headers()` | `array` | 全部请求头（`name => value`） |
| `body(int $maxLen = 65536)` | `string` | 读请求体（读完或无体返回 `""`） |
| `remoteAddr()` | `string` | 客户端 IP |
| `remotePort()` | `int` | 客户端端口 |
| `isSsl()` | `bool` | 是否 HTTPS |
| `httpVersion()` | `string` | HTTP 版本（`"1.1"` 等） |

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

示例：

```php
return new Response("created", 201)
    ->header("X-Request-Id", $id)
    ->cookie("sid", $sid);
```

## 常量

| 常量 | 值 | 说明 |
|---|---|---|
| `Tphp\Teb\VERSION` | `"0.1.0"` | 扩展版本 |
| `Tphp\Teb\FEATURES_DEFAULT` | `0` | 默认特性集 |
| `Tphp\Teb\FEATURES_CGI` | `4` | CGI 特性位 |

## 规则与限制

- **一个进程一个 server**：`start()` 后必须先 `stop()` 才能再次 `start()`。
- **回调在主线程执行**：路由回调由主循环（`run()`/`drain()`）触发，回调内可直接使用任意 PHP 能力（文件、数据库、协程等）；回调期间对应的服务器线程同步等待，`Request` 读到的连接不会被复用或关闭。
- **无匹配路由时**：请求交回 civetweb 默认处理——`document_root` 下存在的静态文件将被直接返回，否则 404。
- **回调返回值**：必须是 `Response`；否则打 warning 并自动回 500。
- **配置值全部为字符串**：`start()` 的 options 值必须是 string，整数需显式转换。
- **编译与功能**：HTTPS/WebSocket/IPv6/CGI/缓存/统计全量启用；gzip 仅 Linux/macOS 默认启用（见「功能特性」「跨平台编译」两节）。
