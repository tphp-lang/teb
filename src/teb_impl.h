#pragma once
/*
 * tphp/teb - C extension (object API)
 *
 * civetweb HTTP server 原生桥接，2025 master 源码随包静态链入。
 * 对外只暴露七个原生类（无顶层函数）：
 *
 *   Tphp\Teb\Web          服务器生命周期：route()/notFound()/before()/file()/websocket()
 *                         /start()/run()/drain()/stop()/stats()
 *   Tphp\Teb\Request      路由回调入参：method()/uri()/header()/body()/form()/file()...
 *   Tphp\Teb\Response     回调返回值：new Response(...) + 链式 + 静态工厂 json/text/html/redirect/file
 *   Tphp\Teb\Session      服务端会话：$r->session() 惰性打开，id/get/set/has/remove/all/
 *                         destroy/regenerateId；Set-Cookie 由响应写出点自动注入
 *   Tphp\Teb\Stream       流式响应：beginStream() 产物，惰性发头、close 定界
 *   Tphp\Teb\UploadedFile 上传文件：form()/files() 产物，moveTo() 落盘
 *   Tphp\Teb\WebSocket    WS 连接（P1）：websocket() 回调入参，send/ping/close/remoteAddr
 *
 * WS 帧层（RFC6455 子集，drain 侧解码）：civetweb data 回调给已解 mask 的
 * 单帧载荷（FIN|opcode 在 bits）——控制帧不出回调：PING 自动回 PONG、PONG
 * 忽略、CLOSE 回 CLOSE 并断连；数据帧 FIN=0 分片按 conn 装配后交付
 * onMessage（bits=消息 opcode，1=文本 2=二进制），协议错/超限断连。
 *
 * 线程模型（执行池，2026-11-19 起）：
 *   civetweb worker 进入 thunk/WS 回调 → 入队阻塞等 done。消费端两路：
 *   主线程 Web::run()/drain()（全量节点）+ N 个执行池线程（callback_threads，
 *   缺省 min(4,核数)；仅 HTTP 节点——WS 回调固定主线程，跨帧 global 状态
 *   与主脚本同线程）。池线程 = 独立 runtime（worker.c 引导序：thread 内部类
 *   → async/io/closure init → class_tab_clone → 重取 7 个 teb 类句柄 →
 *   tphp_vm_run_closure 跑 `\Tphp\Teb\Internal::pump()` 循环）。闭包跨线程
 *   走通用 serialize（'C' 格式，Program 进程级注册表），发布点快照成
 *   blob、池线程按 (节点, config_gen) 缓存本地副本。共享字节码的类 ic 指向
 *   "先解析线程"的克隆体 → 类判定走指针快路+名字兜底（teb_is_cls），引擎
 *   tphp_class_isa 同步补跨线程克隆等价（类型检查汇合点）。
 *   keep-alive 缺省开启（enable_keep_alive=yes 透传注入）：PHP 响应不再手写
 *   Connection: close（Stream 无 Content-Length 例外，close 定界 + 禁复用；
 *   413 提前拒收同样禁复用），HEAD 不发 body。
 *   路由匹配：精确 → 前缀 → 静态文件（前缀+存在性，thunk 内判定）→ notFound
 *   → 最长路由兜底 → return 0 交 civetweb 默认处理。
 *   回调异常/非 Response → C 侧写 500（异常信息照常打印）；流式回调返回值
 *   一律忽略（收尾契约见 doc/API.md Stream 节）。
 *   stats 计数全部持 g_teb.mu；会话读写持 per-sid 条带锁（open→flush 段，
 *   同会话请求串行防丢更新，任意消费线程皆可持有）；回调期间 conn 归本
 *   请求独占。Set-Cookie 在写出时刻注入（正常/500/流式未发头/静态四类
 *   路径），数据 flush 挂 reclaim 后。
 */
#include "pkg.h"            /* tphp_ext_reg_const_str / tphp_ext_reg_fn / tphp_json_encode_c */
#include "value.h"          /* TVal / TV_* / tstr_* / tm_* */
#include "opcode.h"         /* VM / BuiltinFn */
#include "class.h"          /* TClass / TObj / tphp_obj_alloc / register_compile */
#include "tphp.h"           /* tphp_warn */
#include "ast.h"            /* AStmtList / tphp_parse_script（pump Program 编译） */
#include "tsched.h"         /* tphp_sched_loop_init（池线程引导） */

#include <uv.h>             /* uv_mutex_t / uv_cond_t / uv_fs_* / uv_os_* / uv_thread_* */

/* ---- ext/worker 同款跨线程机械（TinyPHP.rt/ext/worker/worker.c L27-48 对照）----
 * 池线程 = 独立 runtime（thread-local VM/堆/类表克隆），闭包经通用
 * serialize（'C' 格式，Program 进程级注册表跨线程解析）克隆，字节码只读共享。 */
extern int tphp_vm_run_closure(Program *prog, Program **share, int n_share,
                               TClosure *clo, TVal *main_ret);
extern void tphp_worker_init(void);          /* worker.c 尾：本线程内部类 */
extern void tphp_async_init(void);           /* sched.c */
extern void tphp_io_init(void);              /* io.c */
extern void tphp_closure_class_init(void);   /* vm.c */
extern void gc_register(GCHead *h);          /* mem.c */
extern Program *tphp_compile(AStmtList *ast, const char *file);
extern void tphp_prog_freeze(Program *prog);

/* civetweb 头（teb 包内 civetweb/ 目录） */
#include "civetweb/civetweb.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- Session（doc/API.md Session 节）---- */
#define TEB_SESS_COOKIE     "tphp_sid"          /* 会话 cookie 名 */
#define TEB_SESS_SID_LEN    64                  /* 64 位小写 hex（32B CSPRNG） */
#define TEB_SESS_MAX_BYTES  (1 << 20)           /* 单会话文件读上限 1MB */
#define TEB_SESS_PATH_CAP   512                 /* save_path(400) + '/' + sid(64) + ".tmp" */

/* ---- vm.c ---- */
/* 异常安全的回调调用：回调抛异常落本函数 local setjmp，返回 false。 */
extern bool tphp_vm_call_bounded(struct VM *vm, TVal callable, TVal *args,
                                 int n, TVal *ret);

/* ---- 全局状态 ---- */

/* 路由方法位掩码（批A：方法分发；0 = 任意方法，route() 即此） */
#define TEB_M_GET     (1u << 0)
#define TEB_M_POST    (1u << 1)
#define TEB_M_PUT     (1u << 2)
#define TEB_M_PATCH   (1u << 3)
#define TEB_M_DELETE  (1u << 4)
#define TEB_M_HEAD    (1u << 5)
#define TEB_M_OPTIONS (1u << 6)

/* 路径模板段（批A：`/user/{id}` 大括号语法；注册期解析一次，发布后只读） */
typedef struct {
    char *text;        /* 字面量内容 / 参数名（不含花括号） */
    bool is_param;
} TebSeg;

typedef struct TebRoute {
    char *uri;         /* 原始注册串（模板含 {x}；最长兜底档仍按它 strlen 排序） */
    TVal callable;
    /* 跨线程序列化 blob（pools_enabled 时在发布点生成；池线程据此重建
     * 本地闭包副本——共享 callable 对象绝不跨线程 addref/release） */
    char *cb_blob;
    uint32_t cb_blob_len;
    uint32_t methods;        /* 方法位掩码，0 = 任意（route()） */
    TebSeg *segs;            /* 非 NULL = 路径模板；NULL = 纯字面量 */
    uint8_t n_seg;
    struct TebRoute *next;
} TebRoute;

/* P1-3 静态文件目录映射（$web->file(prefix, dir)；多条，头插） */
typedef struct TebStatic {
    char *prefix;
    char *dir;
    struct TebStatic *next;
} TebStatic;

/* P1-4 WebSocket 注册（$web->websocket(uri, onOpen, onMsg, onClose)） */
typedef struct TebWsReg {
    char *uri;
    TVal on_open;
    TVal on_msg;      /* TV_NULL = 未提供 */
    TVal on_close;    /* TV_NULL = 未提供 */
    /* 三回调各自的跨线程序列化 blob（0=open 1=msg 2=close） */
    char *cb_blob[3];
    uint32_t cb_blob_len[3];
    struct TebWsReg *next;
} TebWsReg;

/* P1-4 WS 连接登记（node 持 obj +1，生命周期 = 连接；mu 保护） */
typedef struct TebWsSock {
    TObj *obj;
    struct mg_connection *conn;
    struct TebWsSock *next;
} TebWsSock;

/* WS 分片装配（FIN=0 数据帧累积；conn 键。列表结构由 ws_frag_mu 保护——
 * 同连接帧经队列 done 协议天然串行，跨连接帧并发改链须加锁） */
typedef struct TebWsFrag {
    struct mg_connection *conn;
    int opcode;                /* 首帧 opcode（TEXT/BINARY），续帧聚合后按它交付 */
    char *buf;
    size_t len, cap;
    struct TebWsFrag *next;
} TebWsFrag;

/* 队列节点 kind */
enum {
    TEQ_REQ = 0,      /* 路由/nf/兜底命中的 HTTP 请求 */
    TEQ_STATIC = 1,   /* 静态文件命中 */
    TEQ_WS_OPEN = 2,  /* WS 握手完成（ready） */
    TEQ_WS_MSG = 3,   /* WS 数据帧 */
    TEQ_WS_CLOSE = 4  /* WS 连接关闭 */
};

typedef struct TebRequest {
    int kind;
    struct mg_connection *conn;
    TebRoute *route;            /* TEQ_REQ：命中路由（nf/兜底也走此） */
    char **pvals;               /* TEQ_REQ：模板参数值（thunk 匹配产出，
                                 * 名字借 route->segs；n_pvals=参数段数） */
    int n_pvals;
    char *path;                 /* TEQ_STATIC：文件路径（thunk 分配，drain 释放） */
    long long path_size;        /* TEQ_STATIC：文件大小（bytes_sent 近似用） */
    TebWsReg *ws_reg;           /* TEQ_WS_*：所属注册 */
    char *ws_data;              /* TEQ_WS_MSG：帧载荷拷贝（civetweb 已解 mask，出回调即失效） */
    size_t ws_data_len;
    int ws_bits;                /* TEQ_WS_MSG：帧首字节原值（FIN|RSV|opcode），drain 解码 */
    struct TebRequest *next;
    bool taken;                 /* 已被消费者（drain/pool）摘走：shutdown 时
                                 * 必须等 done，不得提前放行 worker（否则
                                 * worker 走默认处理与稍后的响应写出双写 conn） */
    bool done;
} TebRequest;

/* P1-1 运行统计（写方：drain/回调主线程 + thunk 线程入口 413 → 计数加锁） */
typedef struct {
    int64_t started_at;         /* start 时 time(NULL)；0 = 从未启动 */
    int64_t requests_total;     /* 入队处理 + 入口 413 拒绝的 HTTP 请求数 */
    int64_t requests_active;    /* 本批正在处理中 */
    int64_t bytes_sent;         /* teb_wr 累计（静态直通另加文件大小，近似） */
    int64_t ws_active;
    int64_t ws_frames;
    int64_t ws_closed;
    struct { int64_t code; int64_t n; } resp_tab[32];
    int resp_n;
} TebStats;

typedef struct {
    bool inited;
    struct mg_context *ctx;
    TebRoute *routes;          /* 注册的路由列表（与 ctx 解耦；mu 内发布+匹配读） */
    TebRoute *nf;              /* notFound 回调（单条；替换走退休链，stop 释放） */
    TebRoute *nf_retired;      /* 被替换的旧 nf 节点（防悬挂指针，stop 释放） */
    TebRoute *befores;         /* P1-2 before 中间件链（按注册序 = 尾插；mu 内发布） */
    TebStatic *statics;        /* P1-3 静态目录映射（多条，头插；mu 内发布） */
    TebWsReg *ws_regs;         /* P1-4 WS 注册（头插；stop 释放） */
    TebWsSock *ws_socks;       /* P1-4 WS 连接登记（mu 保护） */
    TebWsFrag *ws_frags;       /* WS 分片装配（ws_frag_mu 保护链表结构） */
    TebRequest *req_head;      /* 挂起请求队列头 */
    TebRequest *req_tail;
    uv_mutex_t mu;             /* 保护队列 / done / shutdown / ws_socks / stats /
                                * 配置发布 / session GC 节流 */
    uv_mutex_t ws_frag_mu;     /* ws_frags 链表结构（独立锁：不与 mu 嵌套） */
    uv_mutex_t sess_locks[64]; /* per-sid 条带锁（sid 哈希取模；open→flush 段） */
    uv_cond_t cv;              /* worker↔drain 双向唤醒 */
    bool mu_inited;
    bool aux_locks_inited;     /* ws_frag_mu + sess_locks 一次性初始化 */
    bool shutdown;             /* stop 置位：唤醒全部等待中的 worker */
    bool pending_stop;         /* drain 批内收到 stop → 批末真停 */
    bool stop_requested;       /* 非主线程 stop()：只置标志，主线程执行真停 */
    uv_thread_t main_tid;      /* start() 所在线程（stop 线程归属判定） */
    uint64_t config_gen;       /* 配置代数：任一注册发布 +1（池线程重建闭包副本用） */
    int64_t up_seq;            /* 上传 tmp 名序号（全局自增，并发 multipart 防撞名） */
    /* 执行池：pool_n>0 时 start 创建，real_stop 按序 join */
    uv_thread_t pool_tids[16];
    int pool_n;
    int64_t callback_threads;   /* start 自留键；-1=未配置（缺省 min(4,核数)），0=逃生舱 */
    bool pools_armed;           /* 池启用：注册点序列化 blob + 池线程本地化启用 */
    Program **share;            /* 字节码共享表（main prog + inc_progs；start 捕获，stop 释放） */
    int n_share;
    char *const_blob;           /* 用户常量快照（start 捕获，池首泵导入，stop 释放） */
    uint32_t const_blob_len;
    char *autoload_blob;        /* autoload 回调快照（批B；start 捕获，池首泵导入） */
    uint32_t autoload_blob_len;
    TVal web_obj;              /* 运行中 Web 对象（+1；real_stop 释放） */
    char upload_tmp_dir[400];  /* 上传 tmp 目录（start 自留键；缺省系统 tmp） */
    int64_t upload_max_bytes;  /* 上传/单字段累计上限（缺省 64M） */
    char session_save_path[400];   /* 会话文件目录（start 自留键；缺省 <tmp>/tphp_sess） */
    int64_t session_lifetime;      /* 会话空闲过期秒数（缺省 1209600 = 14 天） */
    int64_t session_gc_interval;   /* 目录 GC 节流秒数（缺省 300） */
    int64_t session_last_gc;       /* 上次全量 GC 时刻（start 复位 0） */
    TebStats st;               /* P1-1 运行统计 */
} TebGlobal;

extern TebGlobal g_teb;

/* 类句柄（registry 构建于 init，drain 只跑主线程；定义于 teb_core.c） */
extern _Thread_local TClass *g_web_cls;
extern _Thread_local TClass *g_req_cls;
extern _Thread_local TClass *g_resp_cls;
extern _Thread_local TClass *g_stream_cls;
extern _Thread_local TClass *g_upfile_cls;
extern _Thread_local TClass *g_ws_cls;
extern _Thread_local TClass *g_sess_cls;
/* 池线程专属（主线程恒 false/空；_Thread_local 天然隔离；定义于 teb_pool.c） */
extern _Thread_local bool g_in_pool_thread;
extern _Thread_local bool g_consts_imported;
extern _Thread_local bool g_autoload_imported;   /* 批B */
typedef struct { const void *key; uint64_t gen; TVal local; } TebLocalCb;
extern _Thread_local TebLocalCb *g_local_cbs;
extern _Thread_local int g_local_n, g_local_cap;
extern _Thread_local uint64_t g_local_gen;

/* 匹配结果（批A）：命中路由 + 参数值 + 方法不符证据（405 用） */
typedef struct {
    TebRoute *route;       /* 命中（NULL = 未命中或 405） */
    char **pvals; int n_pvals;   /* 模板参数（移交调用方→req 拥有） */
    bool method_miss;      /* 有路径匹配候选但方法全不符 → 405 */
    uint32_t allow;        /* 路径匹配候选的 methods 并集（Allow 头） */
} TebMatch;

/* ---- 文件上传（doc/API.md UploadedFile 节；解析器实现于 teb_request.c）---- */
typedef struct {
    TArr *form;                 /* 文本字段 key=>value */
    TArr *files;                /* key => UploadedFile */
    char cur_key[256];          /* 当前文本字段名（field_get 首块给定） */
    char cur_fkey[256];         /* 当前文件字段名（field_found 给定） */
    char cur_fname[512];        /* 当前文件浏览器申报名 */
    char *acc;                  /* 文本字段值累积（按 valuelen，二进制安全） */
    size_t acc_len, acc_cap;
    int64_t total;              /* 全局累计字节 */
    int64_t max_bytes;
    char tmp_dir[400];
    bool aborted;               /* field_* 回调返回过 ABORT（驱动仍返 field_count） */
    bool over_limit;            /* 中止原因 = 累计超 upload_max_bytes */
} FormCtx;

/* ==== 跨 TU 函数原型（按宿主文件分组；域内专属函数保持 static） ==== */

/* ---- teb_core.c ---- */
void teb_lock(void);
void teb_aux_locks_init(void);
void teb_sess_lock(const char *sid, size_t len);
void teb_sess_unlock(const char *sid, size_t len);
char *teb_dup(const char *s);
int teb_wr(struct mg_connection *conn, const void *buf, size_t len);
void teb_stat_resp(int64_t code);
bool teb_is_cls(TObj *o, TClass *c, const char *nm);
bool teb_str_icmp(const char *a, const char *b);
bool teb_str_icontains(const char *hay, const char *needle);
void teb_set_slot(TObj *o, int slot, TVal v);
bool teb_fs_unlink(const char *path);
bool teb_fs_rename(const char *from, const char *to);
bool teb_fs_copyfile(const char *from, const char *to);
TVal teb_header_ci(TObj *o, const char *name);
TArr *teb_parse_urlencoded(const char *s, size_t len);
const char *teb_arg_str(VM *vm, TVal *args, int n, int i, const char *who);
int64_t teb_arg_int(VM *vm, TVal *args, int n, int i, const char *who);
TClass *teb_cls_new(const char *fqcn);
void teb_cls_add_native(TClass *cls, const char *name, void *fn, bool is_static);
void teb_cls_add_ctor(TClass *cls, void *fn);
void teb_cls_add_prop(TClass *cls, const char *name);
void teb_snapshot_cb(TVal cb, char **blob_out, uint32_t *len_out);
TVal teb_cb_acquire(VM *vm, const void *key, TVal shared,
                    const char *blob, uint32_t blen);
void teb_local_cache_free(void);
int teb_read_file_max(const char *path, int64_t max_sz, char **out, size_t *outlen);

/* ---- teb_route.c ---- */
uint32_t teb_method_bit(const char *m);
bool teb_static_match(const char *uri, char **out_path, long long *out_size);
void teb_match_route(const char *uri, uint32_t mbit, TebMatch *out);
TVal nm_web_route(VM *vm, void *self, TVal *args, int n);
TVal nm_web_get(VM *vm, void *self, TVal *args, int n);
TVal nm_web_post(VM *vm, void *self, TVal *args, int n);
TVal nm_web_put(VM *vm, void *self, TVal *args, int n);
TVal nm_web_patch(VM *vm, void *self, TVal *args, int n);
TVal nm_web_delete(VM *vm, void *self, TVal *args, int n);
TVal nm_web_head(VM *vm, void *self, TVal *args, int n);
TVal nm_web_options(VM *vm, void *self, TVal *args, int n);
TVal nm_web_not_found(VM *vm, void *self, TVal *args, int n);
TVal nm_web_before(VM *vm, void *self, TVal *args, int n);
TVal nm_web_file(VM *vm, void *self, TVal *args, int n);
TVal nm_web_websocket(VM *vm, void *self, TVal *args, int n);

/* ---- teb_server.c ---- */
int teb_req_thunk(struct mg_connection *conn, void *cbdata);
int teb_ws_connect_ok(const struct mg_connection *conn, void *cbdata);
void teb_ws_ready_cb(struct mg_connection *conn, void *cbdata);
int teb_ws_data_cb(struct mg_connection *conn, int bits, char *data,
                   size_t len, void *cbdata);
void teb_ws_close_cb(const struct mg_connection *conn, void *cbdata);
/* 当前线程 drain 批深度（0=不在批内；定义于 teb_server.c）。
 * drain 消费者多线程并发（主线程 mode=0 + 执行池 mode=1），"是否在批内"
 * 只被本线程的 nm_web_stop 查询——共享 bool 会被并发消费者空泵互清，
 * 导致 handler 内 stop() 误走直停分支（debug-teb-stop-hang 根因）。 */
extern _Thread_local int g_teb_drain_depth;
void teb_drain_mode(VM *vm, int mode);
void teb_drain_once(VM *vm);
void teb_register_websocket(void);

/* ---- teb_web.c ---- */
void teb_real_stop(void);
void teb_register_web(void);

/* ---- teb_pool.c ---- */
void teb_spawn_pools(void);
char *teb_const_snapshot(uint32_t *out_len);
char *teb_autoload_snapshot(uint32_t *out_len);
void teb_backfill_blobs(void);
void teb_register_internal(void);

/* ---- teb_request.c ---- */
TVal teb_info_get(TObj *o, const char *key);
void teb_reclaim_tmp(TObj *req);
TArr *teb_parse_cookies(TObj *o);
void teb_register_request(void);
void teb_register_uploadedfile(void);

/* ---- teb_response.c ---- */
void teb_write_response(struct mg_connection *conn, TObj *r, TObj *req);
void teb_write_500(struct mg_connection *conn, TObj *req);
void teb_stream_send_headers(TObj *s);
void teb_register_response(void);
void teb_register_stream(void);

/* ---- teb_session.c ---- */
TObj *teb_sess_of(TObj *req);
char *teb_sess_cookie_line(TObj *sess);
void teb_session_flush(TObj *req);
TVal nm_req_session(VM *vm, void *self, TVal *args, int n);
void teb_register_session(void);
