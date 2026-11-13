# teb 对象 API P0 六项完善 — 设计稿（冻结版）

> 状态：设计经三轮确认冻结，"按稿实现"。任何窗口接手时**先读本文件**，勿凭 README
> 旧版或 FFI 时代印象复刻。实现计划见 [P0-plan.md](P0-plan.md)。
> 本文以代码形态说明契约；C 侧行为以"实现要点"给出。

## 总览

1. 流式响应 `Request::beginStream()`
2. 文件上传 `Request::form()/file()/files()` + `UploadedFile`
3. `Web::notFound()`
4. Request 解析助手（query/cookie/json/clientIp…，body 必须缓存化）
5. Response 静态工厂 `json/text/html/redirect`
6. 安全 stop（draining / pending_stop）

**P0 明确不做**：方法路由糖 get/post/405（用户明砍）。P1（before/WebSocket/静态文件/stats）
已立项，设计见 [P1-design.md](P1-design.md)；P2 尚未冻结（候选清单附 P1 文末）。

---

## 1. 流式响应

```php
$s = $req->beginStream(int $status = 200, array $headers = []): Stream;

$s->status(int $code): self;          // 头未发时可改；已发 → Error
$s->header(string $k, string $v): self; // 同上
$s->write(string $chunk): int;        // 惰性发头后写出，返回字节数
$s->end(string $last = ""): void;     // 幂等；可携带最后一块
```

实现要点：
- Stream props：0=conn 1=status 2=headers 3=header_sent 4=ended（private）。
- **惰性头**：首次 `write()`（或 `end()` 时还没发过）才发
  `HTTP/1.1 <status> <reason>` + 用户 headers + `Connection: close\r\n\r\n`。
  **不带 Content-Length、不用 chunked**（close 定界）。
- `beginStream` 幂等：已 begin 过直接返回同一个 Stream；Stream 挂在
  `Request.props[4]`。
- **回调契约扩展**：该请求若已 beginStream → 回调返回值**一律忽略**（null 也接受，
  返回了 Response 也忽略）；回调没 end → drain 自动收尾（等价 end("")：没发过头则
  发一个空体响应头）。
- **异常中止**：头已发 → 只置 ended，**不写 500**；头未发 → 照常写 500，同时把
  header_sent/ended 置位（锁死 Stream，防止事后向已死连接写）。
- 无 Stream 时维持现状：必须返回 Response 否则 500。

## 2. 文件上传

```php
$form = $req->form(): array;                  // 惰性解析，首次调用触发，之后缓存
$f    = $req->file(string $name): ?UploadedFile;
$fs   = $req->files(): array;                 // name => UploadedFile

$f->name(): string;    // 浏览器申报的原始文件名
$f->type(): string;    // 扩展名推断的 mime（见已知差异）
$f->tmpName(): string;
$f->size(): int;
$f->error(): int;      // PHP UPLOAD_ERR_* 对齐
$f->isMoved(): bool;
$f->moveTo(string $dest): bool;
```

- Request props 扩到 8 槽：0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
  5=body_cache 6=qp_cache 7=cookie_cache。`TV_NULL` = 未解析哨兵（解析后必存 T_ARR/T_STR）。
- `form()` 解析路径：
  - GET/HEAD → 解析 query string（手动 urlencoded 解析，`mg_url_decode(is_form=1)`）；
  - `Content-Type: multipart/form-data` → `mg_handle_form_request`（**会耗尽 body**，
    文档注明：multipart 请求勿先调 `body()`）；
  - 其余有 body → 读满 body 缓存后手动解析 urlencoded。
  - `body()/json()/form()`（urlencoded 分支）三者**共用 props[5] body 缓存**，互不挤占。
- civetweb 回调：
  - `field_found`：先落上一文本字段；`filename[0]` 非空 → 生成
    `<upload_tmp_dir>/tphp_up_<pid>_<seq>` 写入 path（超 512B → ABORT），返回 STORE；
    否则返回 GET。
  - `field_get`：**按 valuelen 累积**（multipart 分块、非 NUL 结尾）；
    累计超 `upload_max_bytes` → HANDLE_ABORT。
  - `field_store`：`size > max` → unlink + error=1；否则建 UploadedFile 入 files[key]。
- `moveTo`：已 moved / error≠0 / tmp 空 → false；先尽力 `uv_fs_unlink(dest)`，
  `uv_fs_rename` → 失败回落 `uv_fs_copyfile` + unlink(src)（全走 libuv，UTF-8 安全）；
  成功置 moved、清 tmp。
- **tmp 自动回收**：drain 在回调完成后遍历 files 缓存，unlink 所有未 moved 的 tmp。
- `Web::start()` 自留键：`upload_tmp_dir`（缺省 `uv_os_tmpdir()`）、
  `upload_max_bytes`（缺省 64M）——**识别后从 options 摘除，不透传 civetweb**
  （透传会 unknown option 导致启动失败）。

### 上传错误码（按本机 PHP 实测对齐）

| 码 | 常量 | teb 产生时机 |
|---|---|---|
| 0 | OK | 正常 |
| 1 | INI_SIZE | 文件超 upload_max_bytes |
| 6 | NO_TMP_DIR | tmp 路径生成失败 |
| 3/7/8 | PARTIAL/CANT_WRITE/EXTENSION | civetweb 不回调场景区间，天然不可达 |
| 4 | NO_FILE | **不可达**，见已知差异 |

> 冻结讨论中出现过 "4=PARTIAL / 7=NO_FILE" 的记忆表述，与 PHP 实测
> （`php -r` 实机输出：PARTIAL=3、NO_FILE=4、NO_TMP_DIR=6、CANT_WRITE=7、EXTENSION=8）
> 不符；"对齐 PHP" 是拍板本意，故以实测值为准。

### 已知差异（文档必须注明）

- **空文件选择器无法识别**：civetweb 把"无 filename 参数"和"filename=\"\""都归一成
  `filename=""` 传给 field_found，文本字段与空文件不可区分 → 呈现为文本字段空值，
  error=4(NO_FILE) 不可达。
- `type()` 是服务端按扩展名推断（`mg_get_builtin_mime_type`，未知 → text/plain），
  **不是**浏览器申报的 mime，与 $_FILES['type'] 有差异。

## 3. notFound

```php
$web->notFound(callable $cb): self;   // 单条；重复调用替换
```

- 挂 `g_teb.nf` 单条（不在 routes 链表，stop 时单独释放）。
- thunk 匹配顺序：**精确 → 前缀 → 有 nf 用 nf → 无 nf 维持现状（最长路由兜底）→
  return 0（静态回退）**。
- 即：注册了 nf 后，静态回退与最长兜底一并被覆盖（文档明示）；不注册则行为与现状
  完全一致。实现上 `r = g_teb.nf` 直接复用现有入队/调用路径。

## 4. Request 解析助手

```php
$req->body(int $maxLen = 65536): string;   // 改为读 body 缓存（首次读满全部），按 maxLen 截断
$req->queryParams(): array;                // 解析缓存 props[6]
$req->queryParam(string $k, $default = null);
$req->cookies(): array;                    // 解析 headers.Cookie，缓存 props[7]
$req->cookie(string $k, $default = null);
$req->json($default = null);               // body 缓存 + tphp_json_decode_c；坏 JSON → default
$req->contentType(): ?string;
$req->isJson(): bool;                      // ci 包含 "json"
$req->clientIp(): string;                  // X-Forwarded-For 首段（trim），否则 remote_addr
$req->contentLength(): int;                // info.content_length，负值钳到 0
```

- **body 必须缓存化**：当前每次 `mg_read` 是流式耗尽，一次读全存 props[5]，
  `body()/json()/form()`（urlencoded）共用；上限 upload_max_bytes。
- header 大小写不敏感查找抽成共用 helper。

## 5. Response 静态工厂

```php
Response::json($data, int $status = 200, array $headers = []): Response;
Response::text(string $s, int $status = 200, array $headers = []): Response;
Response::html(string $s, int $status = 200, array $headers = []): Response;
Response::redirect(string $url, int $status = 302, array $headers = []): Response;
```

- native static（`is_static=true`，引擎以 `self=NULL` 分发，vm.c 已支持）。
- 共用 build helper：body 取所有权；默认 Content-Type（json=application/json、
  text=text/plain、html=text/html）仅在用户 headers 无 content-type（ci）时预置，
  用户 headers 后合并覆盖；cookies 空数组。
- redirect：空 body、无默认 ctype、**Location 最后强制写入**（覆盖用户同名项）。
- json encode 依赖引擎新增 C API（见下）。

### 引擎侧配合（TinyPHP.rt）

```c
/* ext/json/json.c —— 复用 enc_val + JBuf，vm 传 NULL（enc_val 不使用 vm） */
bool tphp_json_encode_c(TVal v, TStr **out);
/* ext/pkg/pkg.h —— 与 tphp_json_decode_c 并列声明 */
bool tphp_json_encode_c(struct TVal v, struct TStr **out);
```

## 6. 安全 stop

```php
$web->stop(): void;   // 幂等（无运行中服务器 → no-op）
```

- `TebGlobal` 增 `draining / pending_stop / web_obj / nf / upload_tmp_dir /
  upload_max_bytes`。
- `Web::stop` 在 `draining` 中**只置 pending_stop**（回调里调 stop 是合法用法）；
  否则立即真停。
- `teb_drain_once`：取批（锁内）→ `draining=true` → 处理完整批（worker 已
  done+broadcast）→ 锁内置 false → 有 pending_stop 则执行抽出来的
  `teb_real_stop()`（shutdown+broadcast → mg_stop join → 清队列 → 释放 web_obj
  引用与 props[0] → 释放 routes + nf）。真停不依赖 self。
- 真停前 worker 必已被唤醒 → mg_stop join 不死锁。
