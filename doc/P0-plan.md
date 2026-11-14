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

> 2026-11-14 已用桌面源码（C:\Users\28249\Desktop\civetweb-master，1.17，与 teb/civetweb 逐字节同源）逐条核验，全部成立。

- 引擎 `T_NULL=0`（零初始化即 NULL 哨兵）；props 槽 `tphp_obj_alloc` 置 defval、
  `tobj_free`（mem.c:307）逐槽 tval_release。native 静态方法引擎原生支持
  （vm.c:3507 → 1571-1574，`self=NULL`，返回值 +1 所有权转移 vm.c:1587）。
- civetweb `mg_handle_form_request` / `mg_form_data_handler`（civetweb.h:1202-1303）：
  - `field_found` 的 filename **恒非 NULL**，无文件时为 `""`
    （url_encoded_field_found：filename==NULL → filename_dec[0]=0 后统一传本地
    filename_dec；multipart 无 filename 同走此路）→ 空文件不可区分。
  - `field_get` 分块、非 NUL 结尾 → 按 valuelen 累积；返回 HANDLE_GET=继续 /
    HANDLE_NEXT=完 / HANDLE_ABORT=中止（civetweb.h:1285-1298）。
    **multipart 续块 key=NULL、keylen=0**（handle_form.inl:989-995，get_block>0 分支）
    → FormCtx 累积逻辑必须判 key==NULL = 当前字段续块；首块 key 已 url_decode，
    value 恒为原始字节（二进制安全）。
  - query/urlencoded 路径每字段只调一次（url_encoded_field_get 完整解码），
    返回 HANDLE_GET 安全。
  - `field_store(path,size)` 完整落盘才回调；fwrite 失败/部分上传
    remove_bad_file 清理不回调（civetweb.h:1273-1276 注释明示）。
  - path 是 mg 内部 `char path[512]` 栈缓冲（handle_form.inl:200），
    回调收 sizeof-1=511 → tmp 路径 <512B。
  - 返回值=处理字段数，<0=错误；**已 STORE 成功的文件调用方负责删除**
    （部分成功后中止同样返回 <0，tmp 回收必须覆盖此路径）。
    POST 无 body（content_length=0 且非 chunked）且 method≠GET → -1；
    GET 无 query_string → -1（form() 需区分 0 字段与无效请求）。
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
- **ext 包是安装副本机制**（2026-11-14）：改源仓后必须重跑 `tphp ext --add`（重新拷贝
  +编译）；`--relink` 只重编安装副本旧代码。`ext --list` 的 built-in 标记是注册表
  残留，exe 里未必有符号（grep 二进制验证）。并行会话的全量 build 会洗掉 teb 链接。
- **引擎 bug #1（已修 2026-11-14）**：`tphp_vm_call_bounded` 异常逃逸分支不清
  `vm->x.exception` → 残留指针让后续所有无 catch 回调恒 500（teb P0 冒烟 29 FAIL
  的根因）；累积堆破坏至进程退出段错误。修复：escaped 分支置 NULL（异常对象挂
  GC 表，sweep 回收）。修复后引擎全量 247 passed 0 failed。
- **引擎 bug #2（已修复 2026-11-14，主窗口会话）**：**任何 teb 脚本自然退出必段
  错误（EXIT=139）**——根因在 **teb 侧**而非引擎 op 层：`nm_web_start` 原实现
  `g_teb.web_obj = TV_OBJ(o)`（bitcopy 不持有）+ **一次 addref 一次两用**
  （g_teb 全局存储份 + 返回值转移份）→ real_stop 的 release 吃掉 main frame
  返回值 TMP 槽的合法份额 → 帧尾二次 free（EXIT=139）。修复 = 全局份与返回值
  转移份各自独立 addref（teb.c `nm_web_start` 尾部双 addref；所有权约定见引擎
  doc/ext-dev.md）。

  定位过程证据链（留档）：
  - bug #1 修复在位前提下最小复现仍 EXIT=139（5 行脚本：new→start→stop→退出，
    已收编引擎回归 `tests/standard/test-native-self-return.php`，无 teb 时 skip）；
  - 对照排除：纯 PHP 对象 self 链式 / teb 仅 route 均 EXIT=0（触发面收敛到 start）；
  - 隔离实验：start 临时 `return TV_NULL` → EXIT=0（self 返回链实锤）；
  - gdb：main frame `$web` 占 slot0（CV，真引用 rc=1）+ slot1（返回值 dst，幽灵）；
  - `$web = null` 用户侧规避无效（幽灵在返回值 dst 槽非 CV）。
  - 主窗口修复后：repro-139 EXIT=0、teb 冒烟 44/44 + 22/22（自然退出断言已解除
    kill 收尾规避）、引擎回归用例过。
  - 教训：gdb 槽 dump 的"rc 少 1"当时被归因为引擎 op 层对账错位——实际是
    **ext 侧全局存储 TV_OBJ 未按"存储份+转移份"双 addref**；ext-dev.md 所有权
    约定应作为 ext 开发首读项。
- **流式路由写法**：返回值被契约忽略，**不要声明 `: Response` 再 `return null`**
  ——类型检查 TypeError 走异常路径（bug #1 修复前会污染后续请求）。文档已注明。
