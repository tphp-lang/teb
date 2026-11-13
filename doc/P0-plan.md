# teb P0 六项完善 — 实现计划

> 设计契约见 [P0-design.md](P0-design.md)（冻结版，先读它）。本文件是执行清单。
> 状态：**未开工**（2026-11-13 冻结，下次按此执行）。
> P1（before/WebSocket/静态文件/stats）设计见 [P1-design.md](P1-design.md)，
> P0 完成并验证后按其"实现顺序"一节继续。

## 改动文件

| 文件 | 改动 |
|---|---|
| TinyPHP.rt/ext/json/json.c | 新增 `tphp_json_encode_c`（复用 enc_val+JBuf，vm 传 NULL） |
| TinyPHP.rt/ext/pkg/pkg.h | 声明 `bool tphp_json_encode_c(struct TVal, struct TStr **)` |
| teb/src/teb.c | 主体实现（当前 ~830 行） |
| teb/doc/README.md | 六项 API 文档 + 已知差异（空文件、type 非浏览器申报） |
| teb/tests/ | 冒烟脚本增补 |

## 调研事实（实现时直接用，勿重复调研）

- 引擎 `T_NULL=0`（零初始化即 NULL 哨兵）；props 槽 `tphp_obj_alloc` 置 defval、
  `tobj_free`（mem.c:307）逐槽 tval_release。native 静态方法引擎原生支持
  （vm.c:3507 → 1571-1574，`self=NULL`，返回值 +1 所有权转移 vm.c:1587）。
- civetweb `mg_handle_form_request` / `mg_form_data_handler`（civetweb.h:1202-1303）：
  - `field_found` 的 filename **恒非 NULL**，无文件时为 `""`（handle_form.inl:60-65）
    → 空文件不可区分。
  - `field_get` 分块、非 NUL 结尾 → 按 valuelen 累积；返回 HANDLE_GET=继续 /
    HANDLE_NEXT=完 / HANDLE_ABORT=中止（query/urlencoded 路径每字段只调一次，
    返回 HANDLE_GET 安全）。
  - `field_store(path,size)` 完整落盘才回调；fopen 失败/部分上传不回调。
  - path 是 mg 内部 `char path[512]` 栈缓冲 → tmp 路径 <512B。
- PHP 上传错误码（本机 php 实测）：OK=0 INI=1 FORM=2 PARTIAL=3 NO_FILE=4
  NO_TMP_DIR=6 CANT_WRITE=7 EXTENSION=8。
- `mg_get_builtin_mime_type` / `mg_url_decode` / `mg_get_header` / `mg_get_response_code_text`
  可直接用；`uv_os_tmpdir` / `uv_os_getpid` / `uv_fs_rename` / `uv_fs_copyfile` /
  `uv_fs_unlink` 走 libuv（UTF-8 路径安全）。
- **勿用 libc `strdup` + `tm_free` 混搭**（mem.c 有 bins/纯 malloc 两套实现）；
  统一写 `teb_dup()` = tm_malloc+memcpy。
- `tphp_json_decode_c` 已在 pkg.h:17；pkg.h→opcode.h→value.h，TVal 完整。

## teb.c 实现明细

1. **基础设施**：`teb_dup`、`teb_set_slot`（release 旧值再写）、`str_icontains`；
   `TebGlobal` 增 `nf/draining/pending_stop/web_obj/upload_tmp_dir/upload_max_bytes`；
   类句柄增 `g_stream_cls/g_upfile_cls`；Request props 扩 8 槽。
2. **流式**（设计 §1）：Stream 类 4 方法 + `Request::beginStream` + drain 契约
   （返回值忽略 / 自动收尾 / 异常中止两分支）。
3. **上传**（设计 §2）：FormCtx 三回调、form()/file()/files()、UploadedFile
   （uv rename→copy 回落）、drain tmp 回收、start() 摘除自留键。
4. **notFound**（设计 §3）：`r = g_teb.nf` 插在前缀匹配后、最长兜底前。
5. **解析助手**（设计 §4）：body 缓存化 + qp/cookie 缓存 + json/clientIp 等。
6. **工厂**（设计 §5）：`teb_resp_build` + 4 个 native static；引擎 json.c/pkg.h 先行。
7. **安全 stop**（设计 §6）：draining/pending_stop + `teb_real_stop()` 抽出。

## 执行顺序（每块即重链冒烟）

1. 引擎 json.c + pkg.h → 后台 `c:\project\tphp\TinyPHP.rt\build.cmd`（~6min，零警告）。
2. teb.c 按上面 1→7 顺序实现（同一文件，一次写完）。
3. 引擎构建完成后：`.\build\tphp.exe ext --add C:\project\tphp\teb`（~30-60s）。
4. 冒烟（PowerShell：Start-Process 重定向 out/err + Invoke-WebRequest/curl → 读输出 → Kill；
   长驻脚本用 drain()+时间窗自停，用完删 %TEMP% 临时文件）：
   - 流式：`curl -N` 逐块可见；回调抛异常且已发头 → 无 500；未 end → 自动收尾；
   - 上传：`curl -F name=@file` → files()/moveTo 落盘；tmp 无残留；超限 error=1；
   - notFound 命中；无 nf 时静态回退/最长兜底行为不变；
   - 工厂 4 个：状态码/头/体正确；json 不可编码 → Error；
   - 回调内 `$web->stop()` 不死锁、run() 正常退出；重复 stop 幂等；
   - 解析助手：query/cookie/json/clientIp/缓存幂等/重复 body()。
5. 文档 README 增补 → teb 仓中文短句提交、TinyPHP.rt 仓 conventional 英文提交
   （`fix(json): …`），**两仓只 commit 不 push**（用户自己 push）。

## 教训（勿重蹈）

- realloc 后严禁读旧指针 memcpy（tm_realloc 换址已搬移，旧块首 8B 被 free 链覆盖
  → find_method 解引用垃圾 = 随机 AV）。
- add_native 类构建 helper 用 teb.c 现有版本（勿改回 &ms[1] 那版）。
- 引擎 vm.c 调试断点用完全清（git diff 零残留）。
