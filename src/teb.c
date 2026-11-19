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

/* 统一分配器：tm_malloc + memcpy。勿用 libc strdup —— stop 释放路由表走
 * tm_free，strdup 块与之混搭会 mi_free invalid pointer（2026-11-14 冒烟实测）。 */
static char *teb_dup(const char *s) {
    size_t n = strlen(s);
    char *p = tm_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

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
    bool draining;             /* 消费者（drain/pool 线程）正在处理一批 */
    bool pending_stop;         /* draining 中收到 stop → 批末真停 */
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

static TebGlobal g_teb;

/* 类句柄（registry 构建于 init，drain 只跑主线程） */
static _Thread_local TClass *g_web_cls;
static _Thread_local TClass *g_req_cls;
static _Thread_local TClass *g_resp_cls;
static _Thread_local TClass *g_stream_cls;
static _Thread_local TClass *g_upfile_cls;
static _Thread_local TClass *g_ws_cls;
static _Thread_local TClass *g_sess_cls;

/* ---- 执行池状态 ---- */
static Program *g_pump_prog;   /* 泵 Program（main 编译一次，进程级只读共享） */

/* 池线程专属（主线程恒 false/空；_Thread_local 天然隔离） */
static _Thread_local bool g_in_pool_thread = false;
static _Thread_local bool g_consts_imported = false;
static _Thread_local bool g_autoload_imported = false;   /* 批B */
typedef struct { const void *key; uint64_t gen; TVal local; } TebLocalCb;
static _Thread_local TebLocalCb *g_local_cbs = NULL;
static _Thread_local int g_local_n = 0, g_local_cap = 0;
static _Thread_local uint64_t g_local_gen = 0;

/* 首次初始化必须发生在 Web::start（PHP 主线程、mg_start 创建 worker 之前） */
static void teb_lock(void) {
    if (!g_teb.mu_inited) {
        uv_mutex_init(&g_teb.mu);
        uv_cond_init(&g_teb.cv);
        g_teb.mu_inited = true;
    }
    uv_mutex_lock(&g_teb.mu);
}

/* ws_frag_mu + per-sid 条带锁一次性初始化（start 内、建线程前调用，
 * 单次赋值无竞态窗口） */
static void teb_aux_locks_init(void) {
    if (g_teb.aux_locks_inited) return;
    uv_mutex_init(&g_teb.ws_frag_mu);
    for (int i = 0; i < 64; i++) uv_mutex_init(&g_teb.sess_locks[i]);
    g_teb.aux_locks_inited = true;
}

/* per-sid 条带锁（open 持有 → flush 释放；覆盖整个请求的读-改-写段，
 * 同会话请求串行 = PHP file session 独占锁同语义，防丢更新） */
static void teb_sess_lock(const char *sid, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)sid[i]; h *= 16777619u; }
    uv_mutex_lock(&g_teb.sess_locks[h & 63]);
}
static void teb_sess_unlock(const char *sid, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)sid[i]; h *= 16777619u; }
    uv_mutex_unlock(&g_teb.sess_locks[h & 63]);
}

/* ---- 基础设施 helpers ---- */

/* Request info 缓存读取（定义于 Request 方法区；此处前置） */
static TVal teb_info_get(TObj *o, const char *key);

/* Session（定义于 Session 方法区；响应写出点在本文件更早处）：
 * 取 Request 上缓存的会话对象（无则 NULL）；生成 Set-Cookie 整行
 * （tm_malloc，调用方 tm_free；无会话返回 NULL）。 */
static TObj *teb_sess_of(TObj *req);
static char *teb_sess_cookie_line(TObj *sess);
static void teb_session_flush(TObj *req);

/* 执行池（定义于 drain 区）：单条完整处理（drain 批内与池泵共用） */
static void teb_process_one(VM *vm, TebRequest *q);

/* P1-1 统一写出口：全部 PHP 侧响应写出走此（bytes_sent 累加）。
 * 主线程（drain/回调）与 thunk 线程（入口 413）都会调 → 计数加锁。 */
static int teb_wr(struct mg_connection *conn, const void *buf, size_t len) {
    int n = (int)mg_write(conn, buf, (unsigned long long)len);
    if (n > 0) {
        teb_lock();
        g_teb.st.bytes_sent += n;
        uv_mutex_unlock(&g_teb.mu);
    }
    return n;
}

/* P1-1 按实际写出状态码计数（resp_tab 线性表，溢出丢弃新码）。
 * 主线程 + thunk 线程（413）双写 → 加锁。 */
static void teb_stat_resp(int64_t code) {
    if (code < 100 || code > 999) return;
    teb_lock();
    for (int i = 0; i < g_teb.st.resp_n; i++) {
        if (g_teb.st.resp_tab[i].code == code) {
            g_teb.st.resp_tab[i].n++;
            uv_mutex_unlock(&g_teb.mu);
            return;
        }
    }
    if (g_teb.st.resp_n < 32) {
        g_teb.st.resp_tab[g_teb.st.resp_n].code = code;
        g_teb.st.resp_tab[g_teb.st.resp_n].n = 1;
        g_teb.st.resp_n++;
    }
    uv_mutex_unlock(&g_teb.mu);
}

/* 类判定（跨线程执行安全）：池线程共享字节码的类内联缓存（ic）指向
 * "先解析线程"的克隆类——同名不同指针。指针快路（本线程分配的对象，
 * 常规路径零开销）+ 名字兜底（PHP 代码里 new 出来的跨线程类实例）。 */
static bool teb_is_cls(TObj *o, TClass *c, const char *nm) {
    if (!o || !o->cls) return false;
    return o->cls == c || tstr_case_eq(tstr_val(o->cls->name), nm);
}

/* ASCII 大小写不敏感整串比较 */
static bool str_icmp(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* ASCII 大小写不敏感子串 */
static bool str_icontains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
    }
    return false;
}

/* 引用计数槽写入：release 旧值再落新（v 为 +1 所有权转移） */
static void teb_set_slot(TObj *o, int slot, TVal v) {
    tval_release(&o->props[slot]);
    o->props[slot] = v;
}

/* ---- libuv 同步 fs（UTF-8 路径安全；loop=NULL = 当前 loop） ---- */

static bool teb_fs_unlink(const char *path) {
    uv_fs_t r;
    int rc = uv_fs_unlink(NULL, &r, path, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

static bool teb_fs_rename(const char *from, const char *to) {
    uv_fs_t r;
    int rc = uv_fs_rename(NULL, &r, from, to, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

static bool teb_fs_copyfile(const char *from, const char *to) {
    uv_fs_t r;
    int rc = uv_fs_copyfile(NULL, &r, from, to, 0, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

/* 请求头大小写不敏感查找（返回 info 缓存内借用引用；未找到 TV_NULL） */
static TVal teb_header_ci(TObj *o, const char *name) {
    TVal hs = teb_info_get(o, "headers");
    if (hs.t != T_ARR) return TV_NULL;
    TArr *a = (TArr *)hs.u.p;
    /* 精确查（interned 键可指针相等快路） */
    TVal *v = tarr_get(a, TV_STR(tstr_intern(name, strlen(name))));
    if (v && v->t != T_UNDEF) return *v;
    /* 大小写不敏感线性兜底 */
    tarr_sync_buckets(a);
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;
        TStr *k = (TStr *)bk->key.u.p;
        if (tstr_case_eq(tstr_val(k), name)) return a->data[bk->val_off];
    }
    return TV_NULL;
}

/* urlencoded "k=v&k2=v2" → 新 TArr（键值解码、'+'→空格；后值覆盖前值）。
 * 解码缓冲 1KB/4KB（civetweb 自身键缓冲同量级），超长段静默截断。 */
static TArr *teb_parse_urlencoded(const char *s, size_t len) {
    TArr *a = tarr_new();
    if (!s || len == 0) return a;
    const char *p = s, *end = s + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *seg_end = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t)(seg_end - p));
        const char *k = p;
        size_t kl = eq ? (size_t)(eq - p) : (size_t)(seg_end - p);
        const char *v = eq ? eq + 1 : seg_end;
        size_t vl = eq ? (size_t)(seg_end - (eq + 1)) : 0;
        char kb[1024], vb[4096];
        int kln = mg_url_decode(k, (int)(kl > INT_MAX ? INT_MAX : kl),
                                kb, (int)sizeof(kb), 1);
        int vln = mg_url_decode(v, (int)(vl > INT_MAX ? INT_MAX : vl),
                                vb, (int)sizeof(vb), 1);
        if (kln >= 0) {
            TVal key = TV_STR(tstr_new(kb, (size_t)kln));
            TVal val = TV_STR(tstr_new(vln >= 0 ? vb : "",
                                       vln >= 0 ? (size_t)vln : 0));
            tarr_set(a, key, val);   /* key 非 interned → 下方 release；val 所有权转移 */
            tval_release(&key);
        }
        p = amp ? amp + 1 : end;
    }
    return a;
}

/* ---- 路由匹配 ---- */

/* 模板解析（批A，注册期一次）：uri 须 `/` 开头按 `/` 切段；`{name}` 段的
 * name 须 [A-Za-z_][A-Za-z0-9_]*。含 `{` 才解析；非法形态 raise ValueError
 * （raise 前释放全部已建段——**在节点 malloc 之前调用**，注册中止零泄漏）。
 * 返回 segs（调用方移交节点）；无模板返回 NULL。 */
static TebSeg *teb_route_parse(const char *uri, int *out_n, VM *vm,
                               const char *who) {
    *out_n = 0;
    if (!uri || uri[0] != '/' || !strchr(uri, '{')) return NULL;
    const char *p = uri;
    int cap = 8, n = 0;
    TebSeg *segs = tm_malloc(sizeof(TebSeg) * (size_t)cap);
    bool bad = false;
    while (*p && !bad) {
        const char *sl = strchr(p, '/');
        size_t len = sl ? (size_t)(sl - p) : strlen(p);
        if (len > 0) {                      /* 跳过开头连续 /（uri 已保证 / 开头） */
            bool is_param = (len >= 2 && p[0] == '{' && p[len - 1] == '}');
            char *text = tm_malloc(len + 1);
            if (is_param) {
                memcpy(text, p + 1, len - 2);
                text[len - 2] = 0;
                if (text[0] == 0 ||
                    !((text[0] >= 'A' && text[0] <= 'Z') ||
                      (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_'))
                    bad = true;
                for (char *c = text; *c && !bad; c++) {
                    if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
                          (*c >= 'a' && *c <= 'z') || *c == '_'))
                        bad = true;
                }
            } else {
                /* 非整段包裹的花括号 = 非法（裸 {、}、a{b 形态） */
                if (memchr(p, '{', len) || memchr(p, '}', len)) bad = true;
                memcpy(text, p, len);      /* 字面段内容拷贝（漏拷=seg0 乱码） */
                text[len] = 0;
            }
            if (n == cap) {
                cap *= 2;
                segs = tm_realloc(segs, sizeof(TebSeg) * (size_t)cap);
            }
            segs[n].text = text;
            segs[n].is_param = is_param && !bad;
            n++;
        }
        if (!sl) break;
        p = sl + 1;
    }
    if (!bad && n > 0) {
        /* 重复参数名 = 非法 */
        for (int i = 0; i < n && !bad; i++) {
            if (!segs[i].is_param) continue;
            for (int j = i + 1; j < n; j++)
                if (segs[j].is_param &&
                    strcmp(segs[i].text, segs[j].text) == 0) bad = true;
        }
    }
    if (bad) {
        for (int i = 0; i < n; i++) tm_free(segs[i].text);
        tm_free(segs);
        tphp_vm_raise(vm, "ValueError",
                      "%s(): invalid route template \"%s\" (expected"
                      " /seg/{param} with [A-Za-z_][A-Za-z0-9_]* names,"
                      " no duplicate params)", who, uri);
        return NULL;   /* raise 不返回 */
    }
    *out_n = n;
    return segs;
}

/* 批A：方法分发 —— HTTP 方法 → 位（未知方法如 PROPFIND 返 0：与具体
 * 方法路由不符、与 ANY（methods==0）相容——mbit=0 时 (methods & 0)==0
 * 恰使具体路由判不符 ✓）。 */
static uint32_t teb_method_bit(const char *m) {
    if (!m) return 0;
    if (str_icmp(m, "GET"))     return TEB_M_GET;
    if (str_icmp(m, "POST"))    return TEB_M_POST;
    if (str_icmp(m, "PUT"))     return TEB_M_PUT;
    if (str_icmp(m, "PATCH"))   return TEB_M_PATCH;
    if (str_icmp(m, "DELETE"))  return TEB_M_DELETE;
    if (str_icmp(m, "HEAD"))    return TEB_M_HEAD;
    if (str_icmp(m, "OPTIONS")) return TEB_M_OPTIONS;
    return 0;
}

/* 模板档匹配（T2）：raw uri 按 '/' 切非空段，段数与模板一致逐段相等、
 * 参数段非空捕获并 urldecode；请求尾部只允许多余空段（"//"或尾"/"）。
 * 命中时 *out_pvals 装参数段值（数量=参数段数，tm_malloc 移交调用方）。 */
static bool teb_template_match(TebRoute *r, const char *uri,
                               char ***out_pvals, int *out_n) {
    int np = 0;
    for (int i = 0; i < r->n_seg; i++)
        if (r->segs[i].is_param) np++;
    char **pv = tm_malloc(sizeof(char *) * (size_t)(np ? np : 1));
    int vi = 0;
    bool ok = true;
    const char *p = uri;
    if (*p == '/') p++;
    for (int i = 0; i < r->n_seg && ok; i++) {
        while (*p == '/') p++;
        if (!*p) { ok = false; break; }          /* 请求段不够 */
        const char *sl = strchr(p, '/');
        size_t len = sl ? (size_t)(sl - p) : strlen(p);
        TebSeg *s = &r->segs[i];
        if (s->is_param) {
            /* urldecode 输出 ≤ 输入长（%XX 折叠），len+1 缓冲必够 */
            char *d = tm_malloc(len + 1);
            int dn = mg_url_decode(p, (int)len, d, (int)len + 1, 1);
            if (dn < 0) { memcpy(d, p, len); d[len] = 0; }   /* 解码失败回退 raw */
            pv[vi++] = d;
        } else {
            if (len != strlen(s->text) || memcmp(p, s->text, len) != 0)
                ok = false;
        }
        p = sl ? sl + 1 : NULL;
        if (!p) { p = ""; }
    }
    if (ok) {                                    /* 请求须恰好耗尽（余空段可） */
        while (*p == '/') p++;
        if (*p) ok = false;
    }
    if (!ok) {
        for (int i = 0; i < vi; i++) tm_free(pv[i]);
        tm_free(pv);
        return false;
    }
    *out_pvals = pv;
    *out_n = vi;
    return true;
}

/* 匹配结果（批A）：命中路由 + 参数值 + 方法不符证据（405 用） */
typedef struct {
    TebRoute *route;       /* 命中（NULL = 未命中或 405） */
    char **pvals; int n_pvals;   /* 模板参数（移交调用方→req 拥有） */
    bool method_miss;      /* 有路径匹配候选但方法全不符 → 405 */
    uint32_t allow;        /* 路径匹配候选的 methods 并集（Allow 头） */
} TebMatch;

/* 五档匹配（README 路由规则，批A 扩展）：T1 精确字面量 → T2 模板全匹配
 * → T3 前缀字面量 → （T1-T3 全扫无 HIT 且 method_miss → 405，提前返回）
 * → T4 notFound → T5 最长兜底（模板参与，无方法门——保「任意未命中 URI
 * 落最长注册路由」既有契约）。每档扫全档找方法相容候选（后注册优先，
 * get+post 同路径共存）。整段在 mu 内；节点发布后不改不删，锁外可用。 */
static void teb_match_route(const char *uri, uint32_t mbit, TebMatch *out) {
    memset(out, 0, sizeof *out);
    teb_lock();
    /* T1 精确（字面量） */
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        if (r->segs || strcmp(r->uri, uri) != 0) continue;
        if (r->methods == 0 || (r->methods & mbit)) { out->route = r; goto done; }
        out->method_miss = true;
        out->allow |= r->methods;
    }
    /* T2 模板全匹配 */
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        if (!r->segs) continue;
        char **pv = NULL; int nv = 0;
        if (!teb_template_match(r, uri, &pv, &nv)) continue;
        if (r->methods == 0 || (r->methods & mbit)) {
            out->route = r; out->pvals = pv; out->n_pvals = nv;
            goto done;
        }
        for (int i = 0; i < nv; i++) tm_free(pv[i]);
        tm_free(pv);
        out->method_miss = true;
        out->allow |= r->methods;
    }
    /* T3 前缀（字面量） */
    {
        size_t lu = strlen(uri);
        for (TebRoute *r = g_teb.routes; r; r = r->next) {
            if (r->segs) continue;
            size_t lr = strlen(r->uri);
            if (lr == 0) continue;
            if (lu < lr || strncmp(r->uri, uri, lr) != 0 ||
                (uri[lr] != '/' && uri[lr] != 0)) continue;
            if (r->methods == 0 || (r->methods & mbit)) { out->route = r; goto done; }
            out->method_miss = true;
            out->allow |= r->methods;
        }
    }
    if (out->method_miss) goto done;   /* 路径已知、方法不许 → 405（不落 nf/兜底） */
    /* T4 notFound */
    if (g_teb.nf) { out->route = g_teb.nf; goto done; }
    /* T5 最长兜底（含模板；无方法门） */
    {
        size_t best_len = 0;
        for (TebRoute *r = g_teb.routes; r; r = r->next) {
            size_t l = strlen(r->uri);
            if (l > best_len) { best_len = l; out->route = r; }
        }
    }
done:
    uv_mutex_unlock(&g_teb.mu);
}

/* ---- P1-3 静态文件匹配（thunk 线程调用；纯字符串 + uv 同步 stat） ---- */

static bool teb_fs_stat(const char *path, bool *is_dir, long long *size) {
    uv_fs_t r;
    if (uv_fs_stat(NULL, &r, path, NULL) != 0) {
        uv_fs_req_cleanup(&r);
        return false;
    }
    *is_dir = S_ISDIR(r.statbuf.st_mode);
    *size = (long long)r.statbuf.st_size;
    uv_fs_req_cleanup(&r);
    return true;
}

/* decoded uri 对 statics 链做前缀匹配；命中（文件/目录 index.html 存在）
 * 返回 true 并经 out_path（tm_malloc，drain 释放）交出路径。rest 逐段
 * 拒绝 ".."（防穿越）；条目按注册序，首个命中即用。
 * 头指针在 mu 内快照，锁外遍历——statics 是头插发布，节点 next 发布后
 * 不可变（只有全局头变），链上读安全；stat 不持锁。 */
static bool teb_static_match(const char *uri, char **out_path, long long *out_size) {
    if (!uri) return false;
    size_t lu = strlen(uri);
    teb_lock();
    TebStatic *ts0 = g_teb.statics;
    uv_mutex_unlock(&g_teb.mu);
    for (TebStatic *ts = ts0; ts; ts = ts->next) {
        size_t lp = strlen(ts->prefix);
        if (lu < lp || strncmp(uri, ts->prefix, lp) != 0) continue;
        if (uri[lp] != 0 && uri[lp] != '/') continue;
        const char *rest = uri + lp;
        while (*rest == '/') rest++;
        bool bad = false;
        for (const char *p = rest; *p; ) {
            const char *slash = strchr(p, '/');
            size_t seg = slash ? (size_t)(slash - p) : strlen(p);
            if (seg == 2 && p[0] == '.' && p[1] == '.') { bad = true; break; }
            if (!slash) break;
            p = slash + 1;
        }
        if (bad) continue;

        size_t ld = strlen(ts->dir);
        while (ld > 0 && ts->dir[ld - 1] == '/') ld--;
        size_t cap = ld + strlen(rest) + 32;
        char *cand = tm_malloc(cap);
        snprintf(cand, cap, "%.*s/%s", (int)ld, ts->dir, rest);

        bool is_dir = false;
        long long sz = 0;
        if (!teb_fs_stat(cand, &is_dir, &sz)) {
            tm_free(cand);
            continue;   /* 不存在 → 未命中（nf/兜底不被静态遮蔽） */
        }
        if (is_dir) {
            snprintf(cand, cap, "%.*s/%s/index.html", (int)ld, ts->dir, rest);
            if (!teb_fs_stat(cand, &is_dir, &sz) || is_dir) {
                tm_free(cand);
                continue;   /* 目录无 index.html → 未命中 */
            }
        }
        *out_path = cand;
        *out_size = sz;
        return true;
    }
    return false;
}

/* ---- 队列入队 + 阻塞等待（thunk 与 WS 回调共用；worker 线程） ---- */

static void teb_req_free(TebRequest *req) {
    if (req->path) tm_free(req->path);
    if (req->ws_data) tm_free(req->ws_data);
    if (req->pvals) {
        for (int i = 0; i < req->n_pvals; i++) tm_free(req->pvals[i]);
        tm_free(req->pvals);
    }
    tm_free(req);
}

static bool teb_enqueue_and_wait(TebRequest *req) {
    teb_lock();
    if (g_teb.shutdown) {            /* stop 与新请求赛跑 */
        uv_mutex_unlock(&g_teb.mu);
        teb_req_free(req);
        return false;
    }
    if (g_teb.req_tail) g_teb.req_tail->next = req;
    else g_teb.req_head = req;
    g_teb.req_tail = req;
    uv_cond_broadcast(&g_teb.cv);     /* 唤醒阻塞中的 drain */

    /* 等待 PHP 侧处理完（done）或服务器停止（shutdown 且未被摘走）。
     * taken 的节点必须等 done——消费者已持该 conn，提前放行会双写。 */
    while (!req->done && !(g_teb.shutdown && !req->taken))
        uv_cond_wait(&g_teb.cv, &g_teb.mu);
    bool ok = req->done;
    if (!ok) {
        /* shutdown 且尚未被摘走：把自己从队列摘除 */
        for (TebRequest **pp = &g_teb.req_head; *pp; pp = &(*pp)->next)
            if (*pp == req) { *pp = req->next; break; }
        if (g_teb.req_tail == req) {
            g_teb.req_tail = NULL;
            for (TebRequest *p = g_teb.req_head; p; p = p->next)
                g_teb.req_tail = p;
        }
    }
    uv_mutex_unlock(&g_teb.mu);
    teb_req_free(req);   /* 分配者释放；done 后 drain 不再访问该节点 */
    return ok;
}

/* ---- 请求体上限入口预检（413）----
 * Content-Length 已声明且超 upload_max_bytes → 直接 413 不进回调、不入队。
 * multipart 例外：保留既有超限语义（UploadedFile::error()=1 / form 中止，
 * 见 run_p0 upload-limit 契约）；chunked/未声明长度的超限由 body() 读取时
 * 抛 Error 兜底（teb_body_cached）。 */
static const char TEB_413[] =
    "HTTP/1.1 413 Request Entity Too Large\r\n"
    "Content-Type: text/plain\r\nContent-Length: 22\r\n"
    "Connection: close\r\n\r\nRequest Body Too Large";

static bool ri_is_multipart(const struct mg_request_info *ri) {
    for (int i = 0; i < ri->num_headers; i++) {
        const struct mg_header *h = &ri->http_headers[i];
        if (h->name && h->value && str_icmp(h->name, "Content-Type") &&
            str_icontains(h->value, "multipart/form-data"))
            return true;
    }
    return false;
}

static void teb_write_413(struct mg_connection *conn) {
    teb_lock();
    g_teb.st.requests_total++;   /* 被拒请求同样计数（与 drain 侧同锁） */
    uv_mutex_unlock(&g_teb.mu);
    /* keep-alive 下必须显式关连接：handler 返 1 会触发 civetweb 的
     * discard_unread_request_data，把超大 body 全量读完——413 提前拒绝
     * 的意义尽失；置 must_close 后 discard 被跳过、连接直接关。 */
    mg_disable_connection_keep_alive(conn);
    teb_wr(conn, TEB_413, sizeof TEB_413 - 1);
    teb_stat_resp(413);
}

/* ---- 批A：405 方法不符（thunk 入口直写，镜像 413 的入口口径）----
 * 路径已匹配（T1-T3 有候选）但方法全不符、且无 ANY 路由救场 → 405 + Allow
 * （Allow = 命中候选 methods 并集）。CL:0 无 body → 保持 keep-alive（handler
 * 返 1 后 civetweb discard 无事可丢；与 413 的带 body 拒绝不同）。 */
static const char *TEB_M_NAMES[] = { "GET", "POST", "PUT", "PATCH",
                                     "DELETE", "HEAD", "OPTIONS" };

static void teb_write_405(struct mg_connection *conn, uint32_t allow) {
    teb_lock();
    g_teb.st.requests_total++;   /* 入口拒绝口径与 413 一致 */
    uv_mutex_unlock(&g_teb.mu);
    char allow_buf[128];
    size_t off = 0;
    for (int i = 0; i < 7 && off < sizeof allow_buf - 16; i++) {
        if (allow & (1u << i))
            off += (size_t)snprintf(allow_buf + off, sizeof allow_buf - off,
                                    off ? ", %s" : "%s", TEB_M_NAMES[i]);
    }
    char line[320];
    int ln = snprintf(line, sizeof line,
                      "HTTP/1.1 405 Method Not Allowed\r\n"
                      "Allow: %s\r\nContent-Length: 0\r\n\r\n",
                      off ? allow_buf : "GET");
    teb_wr(conn, line, (unsigned long long)ln);
    teb_stat_resp(405);
}

/* ---- civetweb request handler thunk（同步阻塞，见文件头） ---- */
static int teb_req_thunk(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri || !ri->local_uri_raw) return 0;

    /* P1-4 WS 升级请求放行：交回 civetweb 走 websocket handler 握手。
     * 我们的通配 request handler 优先级高于 WS handler，不识别 Upgrade
     * 会把握手请求吞进路由（实测：/ws 落兜底路由回 pong，握手必败）。
     * 普通 GET（无 Upgrade 头）仍走路由/nf。 */
    for (int i = 0; i < ri->num_headers; i++) {
        const struct mg_header *h = &ri->http_headers[i];
        if (h->name && h->value && str_icmp(h->name, "Upgrade") &&
            str_icontains(h->value, "websocket"))
            return 0;
    }

    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->conn = conn;

    /* P1-3 静态文件优先于 nf/兜底（存在性判定在 thunk：不存在继续走
     * 路由匹配，nf 不被静态遮蔽）；匹配/穿越检查用 decoded local_uri */
    if (teb_static_match(ri->local_uri, &req->path, &req->path_size)) {
        req->kind = TEQ_STATIC;
    } else {
        req->kind = TEQ_REQ;
        /* 批A：五档匹配（含模板档）+ 方法分发；method_miss → 405（先于
         * 413 预检：先拒方法再谈 body；未入队不跑 before，入口口径计数） */
        TebMatch m;
        teb_match_route(ri->local_uri_raw, teb_method_bit(ri->request_method),
                        &m);
        if (!m.route) {
            if (m.method_miss) {
                teb_write_405(conn, m.allow);
                tm_free(req);
                return 1;
            }
            tm_free(req);   /* 全无命中 → civetweb 默认处理 */
            return 0;
        }
        req->route = m.route;
        req->pvals = m.pvals;
        req->n_pvals = m.n_pvals;
        /* body 超限预检：声明的 Content-Length 已超上限且非 multipart →
         * 413 提前拒绝，省掉无意义的请求体传输（multipart 保留 error=1 语义） */
        if (ri->content_length > g_teb.upload_max_bytes &&
            !ri_is_multipart(ri)) {
            teb_write_413(conn);
            teb_req_free(req);   /* 含 pvals */
            return 1;
        }
    }

    return teb_enqueue_and_wait(req) ? 1 : 0;
}

/* ---- P1-4 WebSocket civetweb 回调（worker 线程：入队 + 阻塞） ---- */

static int ws_connect_ok(const struct mg_connection *conn, void *cbdata) {
    (void)conn; (void)cbdata;
    return 0;   /* 放行（握手前拒绝留 P2） */
}

static void ws_ready_cb(struct mg_connection *conn, void *cbdata) {
    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->kind = TEQ_WS_OPEN;
    req->conn = conn;
    req->ws_reg = (TebWsReg *)cbdata;
    teb_enqueue_and_wait(req);
}

static int ws_data_cb(struct mg_connection *conn, int bits, char *data,
                      size_t len, void *cbdata) {
    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->kind = TEQ_WS_MSG;
    req->conn = conn;
    req->ws_reg = (TebWsReg *)cbdata;
    req->ws_bits = bits;
    if (data && len) {
        req->ws_data = tm_malloc(len);   /* civetweb 缓冲出回调即失效：拷贝 */
        memcpy(req->ws_data, data, len);
        req->ws_data_len = len;
    }
    teb_enqueue_and_wait(req);

    /* close_requested 判定（mu 内读登记链；node 持 obj 引用无 UAF；
     * 同一连接的 data/close 回调由 civetweb 串行化，无并发读） */
    bool closed = false;
    teb_lock();
    for (TebWsSock *n = g_teb.ws_socks; n; n = n->next) {
        if (n->conn == conn && n->obj->props[1].t == T_TRUE) {
            closed = true;
            break;
        }
    }
    uv_mutex_unlock(&g_teb.mu);
    return closed ? 0 : 1;
}

static void ws_close_cb(const struct mg_connection *conn, void *cbdata) {
    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->kind = TEQ_WS_CLOSE;
    req->conn = (struct mg_connection *)conn;   /* drain 内只读比较用 */
    req->ws_reg = (TebWsReg *)cbdata;
    teb_enqueue_and_wait(req);   /* conn 在 close 回调返回后才被 civetweb 释放 */
}

/* ---- 参数 helper ---- */

static const char *arg_str(VM *vm, TVal *args, int n, int i, const char *who) {
    if (n <= i || args[i].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "%s() expects argument %d to be a string", who, i + 1);
    return tstr_val((TStr *)args[i].u.p);
}

static int64_t arg_int(VM *vm, TVal *args, int n, int i, const char *who) {
    if (n <= i)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "%s() expects at least %d argument(s)", who, i + 1);
    return tval_to_int(args[i]);
}

/* ---- 类构建 helpers（worker.c 同款模式） ---- */

static TClass *teb_cls_new(const char *fqcn) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(fqcn, strlen(fqcn));
    cls->kind = CK_CLASS;
    cls->flattened = true;
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    return cls;
}

static void teb_cls_add_native(TClass *cls, const char *name, void *fn,
                               bool is_static) {
    /* tm_realloc 语义 = 标准 realloc：换址时内容已随 realloc 内部搬移。
     * **严禁 realloc 之后再从旧指针 memcpy**——旧块已 free，块首 8 字节
     * （恰好是 TMethod.name 偏移）被 bins 空闲链 next 指针覆盖，会把
     * name 污染成链表指针 → find_method 解引用垃圾（2026-11-13 教训）。 */
    TMethod *ms = tm_realloc(cls->methods,
                             sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    TMethod *m = &ms[cls->n_methods];
    memset(m, 0, sizeof *m);
    m->name = tstr_intern(name, strlen(name));
    m->visibility = 0;            /* CVIS_PUBLIC */
    m->native = fn;
    m->is_static = is_static;
    cls->methods = ms;
    cls->n_methods++;
}

static void teb_cls_add_ctor(TClass *cls, void *fn) {
    cls->m_ctor = (int16_t)cls->n_methods;
    teb_cls_add_native(cls, "__construct", fn, false);
}

static void teb_cls_add_prop(TClass *cls, const char *name) {
    TProp *p = tm_realloc(cls->props,
                          sizeof(TProp) * (size_t)(cls->n_props + 1));
    TProp *np = &p[cls->n_props];
    memset(np, 0, sizeof *np);
    np->name = tstr_intern(name, strlen(name));
    np->defval = TV_NULL;
    np->visibility = 2;           /* CVIS_PRIVATE：仅 native 方法可读写 */
    np->slot = (uint16_t)cls->n_props;
    cls->props = p;
    cls->n_props++;
}

/* ---- 请求信息物化（回调进入时一次性缓存进 Request::$info） ---- */

static TArr *teb_req_info_arr(struct mg_connection *conn, TebRequest *q) {
    const struct mg_request_info *ri = mg_get_request_info(conn);
    TArr *a = tarr_new();
    if (!ri) return a;

    #define S(k, v) do { \
        if ((v) && *(v)) { \
            tarr_set(a, TV_STR(tstr_intern((k), strlen((k)))), \
                        TV_STR(tstr_new((v), strlen((v))))); \
        } \
    } while (0)
    #define I(k, v) \
        tarr_set(a, TV_STR(tstr_intern((k), strlen((k)))), TV_INT((int64_t)(v)))

    S("method", ri->request_method);
    /* uri：原始请求 URI = 路径 + "?" + query（civetweb request_uri 不含 query） */
    {
        char ubuf[2048];
        if (ri->query_string && *ri->query_string)
            snprintf(ubuf, sizeof ubuf, "%s?%s",
                     ri->request_uri ? ri->request_uri : "",
                     ri->query_string);
        else
            snprintf(ubuf, sizeof ubuf, "%s",
                     ri->request_uri ? ri->request_uri : "");
        S("uri", ubuf);
    }
    S("path", ri->local_uri);
    S("path_raw", ri->local_uri_raw);
    S("http_version", ri->http_version);
    S("query", ri->query_string);
    S("remote_user", ri->remote_user);
    S("remote_addr", ri->remote_addr);
    I("content_length", ri->content_length);
    I("remote_port", ri->remote_port);
    I("server_port", ri->server_port);
    I("is_ssl", ri->is_ssl);

    #undef S
    #undef I

    /* headers 子数组 */
    TArr *hdrs = tarr_new();
    for (int i = 0; i < ri->num_headers && i < 64; i++) {
        const struct mg_header *h = &ri->http_headers[i];
        if (!h->name) continue;
        tarr_set(hdrs,
                 TV_STR(tstr_intern(h->name, strlen(h->name))),
                 TV_STR(tstr_new(h->value ? h->value : "",
                                 h->value ? strlen(h->value) : 0)));
    }
    tarr_set(a, TV_STR(tstr_intern("headers", 7)), TV_ARR(hdrs));

    /* 批A：路径模板参数 → "params" 键（assoc：参数名 → urldecode 值；
     * 名字借 route->segs 的 is_param 段，值在 pvals 按 param 序） */
    if (q && q->kind == TEQ_REQ && q->route && q->route->n_seg &&
        q->pvals) {
        TArr *pa = tarr_new();
        int vi = 0;
        for (int i = 0; i < q->route->n_seg; i++) {
            if (!q->route->segs[i].is_param) continue;
            const char *pn = q->route->segs[i].text;
            TVal k = TV_STR(tstr_intern(pn, strlen(pn)));
            const char *v = (vi < q->n_pvals && q->pvals[vi])
                                ? q->pvals[vi] : "";
            TVal v2 = TV_STR(tstr_new(v, strlen(v)));
            tarr_set(pa, k, v2);
            tval_release(&k);
            vi++;
        }
        TVal pk = TV_STR(tstr_intern("params", 6));
        tarr_set(a, pk, TV_ARR(pa));
        tval_release(&pk);
    }
    return a;
}

/* ---- 响应写出（HTTP 头拼装全部在 C 侧） ---- */

/* 遍历 assoc 数组逐对写出：pre + key + sep + value + CRLF
 * headers: pre="" sep=": " ；cookies: pre="Set-Cookie: " sep="=" */
static void teb_emit_pairs(struct mg_connection *conn, TArr *a,
                           const char *pre, const char *sep) {
    tarr_sync_buckets(a);
    size_t prel = strlen(pre), sepl = strlen(sep);
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;
        TStr *k = (TStr *)bk->key.u.p;
        TVal val = a->data[bk->val_off];
        TStr *vs;
        TVal tmp = TV_NULL;
        if (val.t == T_STR) {
            vs = (TStr *)val.u.p;
        } else {
            vs = tval_to_str(val);
            tmp = TV_STR(vs);
        }
        size_t cap = prel + k->len + sepl + vs->len + 4;
        char *line = tm_malloc(cap);
        int ln = snprintf(line, cap, "%s%.*s%s%.*s\r\n",
                          pre, (int)k->len, k->c, sep,
                          (int)vs->len, vs->c);
        teb_wr(conn, line, (unsigned long long)ln);
        tm_free(line);
        tval_release(&tmp);
    }
}

/* Response 对象（props: 0=status 1=body 2=headers 3=cookies）→ 写出。
 * req = 本请求（会话注入用；可 NULL）。会话 Set-Cookie 在用户头/用户
 * cookie 之后注入——用户自己 ->cookie("tphp_sid",…) 时跳过（用户优先）。 */
static void teb_write_response(struct mg_connection *conn, TObj *r, TObj *req) {
    int64_t status = (r->props[0].t == T_INT) ? r->props[0].u.i : 200;
    TStr *body = (r->props[1].t == T_STR) ? (TStr *)r->props[1].u.p : NULL;
    const char *reason = mg_get_response_code_text(conn, (int)status);

    char line[160];
    int ln = snprintf(line, sizeof line, "HTTP/1.1 %lld %s\r\n",
                      (long long)status, reason ? reason : "OK");
    teb_wr(conn, line, (unsigned long long)ln);
    teb_stat_resp(status);

    if (r->props[2].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)r->props[2].u.p, "", ": ");
    if (r->props[3].t == T_ARR) {
        TArr *cks = (TArr *)r->props[3].u.p;
        teb_emit_pairs(conn, cks, "Set-Cookie: ", "=");
        /* 会话注入守卫：用户已设同名 cookie → 不注入 */
        TVal *own = tarr_get(cks, TV_STR(tstr_intern(TEB_SESS_COOKIE,
                                                     strlen(TEB_SESS_COOKIE))));
        if (!own || own->t == T_UNDEF) {
            char *cl = teb_sess_cookie_line(teb_sess_of(req));
            if (cl) { teb_wr(conn, cl, strlen(cl)); tm_free(cl); }
        }
    } else {
        char *cl = teb_sess_cookie_line(teb_sess_of(req));
        if (cl) { teb_wr(conn, cl, strlen(cl)); tm_free(cl); }
    }

    size_t blen = body ? body->len : 0;
    /* keep-alive：HTTP/1.1 缺省即持久，不再手写 close（Stream/413 例外，
     * 它们靠 close 定界/提前拒收，见各自注释）。HEAD 只发头不发 body——
     * close 时代该问题被掩盖，keep-alive 下 body 字节会污染下一响应。 */
    ln = snprintf(line, sizeof line, "Content-Length: %zu\r\n\r\n", blen);
    teb_wr(conn, line, (unsigned long long)ln);
    bool is_head = false;
    if (req) {
        TVal mv = teb_info_get(req, "method");
        is_head = (mv.t == T_STR &&
                   strcmp(tstr_val((TStr *)mv.u.p), "HEAD") == 0);
    }
    if (body && blen && !is_head) teb_wr(conn, body->c, (unsigned long long)blen);
}

/* 回调异常 / 未返回 Response 的兜底。HEAD/TAIL 拆分供会话 cookie 注入；
 * keep-alive 下 TAIL 不带 close（有 Content-Length），body 独立常量供
 * HEAD 守卫（同 teb_write_response）。 */
static const char TEB_500_HEAD[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: text/plain\r\n";
static const char TEB_500_TAIL[] =
    "Content-Length: 21\r\n\r\n";
static const char TEB_500_BODY[] = "Internal Server Error";

static void teb_write_500(struct mg_connection *conn, TObj *req) {
    teb_wr(conn, TEB_500_HEAD, sizeof TEB_500_HEAD - 1);
    char *cl = teb_sess_cookie_line(teb_sess_of(req));
    if (cl) { teb_wr(conn, cl, strlen(cl)); tm_free(cl); }
    teb_wr(conn, TEB_500_TAIL, sizeof TEB_500_TAIL - 1);
    bool is_head = false;
    if (req) {
        TVal mv = teb_info_get(req, "method");
        is_head = (mv.t == T_STR &&
                   strcmp(tstr_val((TStr *)mv.u.p), "HEAD") == 0);
    }
    if (!is_head)
        teb_wr(conn, TEB_500_BODY, sizeof TEB_500_BODY - 1);
    teb_stat_resp(500);
}

/* ---- 流式响应（doc/API.md Stream 节） ----
 * Stream props: 0=conn 1=status 2=headers 3=header_sent 4=ended 5=session */

/* 惰性头发送：HTTP/1.1 <status> <reason> + 用户头 + 会话 Set-Cookie +
 * Connection: close。不带 Content-Length、不用 chunked（close 定界）。幂等。 */
static void teb_stream_send_headers(TObj *s) {
    if (s->props[3].t == T_TRUE) return;
    s->props[3] = TV_TRUE;
    struct mg_connection *conn = (s->props[0].t == T_INT)
        ? (struct mg_connection *)(uintptr_t)s->props[0].u.i : NULL;
    if (!conn) return;
    int64_t status = (s->props[1].t == T_INT) ? s->props[1].u.i : 200;
    const char *reason = mg_get_response_code_text(conn, (int)status);
    char line[160];
    int ln = snprintf(line, sizeof line, "HTTP/1.1 %lld %s\r\n",
                      (long long)status, reason ? reason : "OK");
    teb_wr(conn, line, (unsigned long long)ln);
    teb_stat_resp(status);
    if (s->props[2].t == T_ARR) {
        TArr *h = (TArr *)s->props[2].u.p;
        teb_emit_pairs(conn, h, "", ": ");
        /* 会话注入：头未发且用户未自带 Set-Cookie（Stream 手写头无 key 模型，
         * 以「是否存在任意 Set-Cookie 行」为守卫） */
        TVal *own = tarr_get(h, TV_STR(tstr_intern("Set-Cookie",
                                                   strlen("Set-Cookie"))));
        if ((!own || own->t == T_UNDEF) && s->props[5].t == T_OBJ) {
            char *cl = teb_sess_cookie_line((TObj *)s->props[5].u.p);
            if (cl) { teb_wr(conn, cl, strlen(cl)); tm_free(cl); }
        }
    } else if (s->props[5].t == T_OBJ) {
        char *cl = teb_sess_cookie_line((TObj *)s->props[5].u.p);
        if (cl) { teb_wr(conn, cl, strlen(cl)); tm_free(cl); }
    }
    /* 无 Content-Length → close 定界：同时通知 civetweb 侧放弃 keep-alive
     * 循环，否则服务端会继续等下一个请求而客户端在等服务端关 → 僵持。 */
    mg_disable_connection_keep_alive(conn);
    teb_wr(conn, "Connection: close\r\n\r\n", 21);
}

/* ---- body 缓存读取（doc/API.md Request 节：body()/json()/form() 共用 props[5]） ----
 * 首次调用读满连接（上限 upload_max_bytes）；超限不缓存残缺数据：置
 * props[5]=INT 超限标记后抛 Error，同请求后续读取重复抛同一异常（可捕获）。 */
static TStr *teb_body_cached(VM *vm, TObj *o) {
    if (o->props[5].t == T_INT)   /* 已超限：重复读取持续抛错，不返回残缺数据 */
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Request: request body exceeds upload_max_bytes (%lld bytes)",
                      (long long)g_teb.upload_max_bytes);
    if (o->props[5].t == T_STR) return (TStr *)o->props[5].u.p;
    if (o->props[0].t != T_INT) {
        TStr *s = tstr_new("", 0);
        teb_set_slot(o, 5, TV_STR(s));
        return s;
    }
    struct mg_connection *conn =
        (struct mg_connection *)(uintptr_t)o->props[0].u.i;
    int64_t limit = g_teb.upload_max_bytes;
    size_t cap = 8192, len = 0;
    if ((int64_t)cap > limit) cap = (size_t)limit;   /* 上限 < 8K 时首读不越界 */
    char *buf = tm_malloc(cap);
    for (;;) {
        if (len == cap) {
            if ((int64_t)len >= limit) {
                /* 顶到上限：探测一字节区分「恰好等于上限」与「超限」 */
                char probe;
                if (mg_read(conn, &probe, 1) > 0) {
                    tm_free(buf);   /* raise 不返回：残缺缓冲先释放、不入缓存 */
                    teb_set_slot(o, 5, TV_INT(1));
                    tphp_vm_raise(vm, "Error",
                                  "Tphp\\Teb\\Request: request body exceeds upload_max_bytes (%lld bytes)",
                                  (long long)limit);
                }
                break;   /* 恰好等于上限：正常读完 */
            }
            cap *= 2;
            if ((int64_t)cap > limit) cap = (size_t)limit;
            char *nb = tm_realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        int r = mg_read(conn, buf + len, (unsigned long long)(cap - len));
        if (r <= 0) break;
        len += (size_t)r;
    }
    TStr *s = tstr_new(buf, len);
    tm_free(buf);
    teb_set_slot(o, 5, TV_STR(s));
    return s;
}

/* ---- 文件上传（doc/API.md UploadedFile 节）----
 * civetweb 三回调：field_found 落上一文本字段 + 文件字段生成 tmp 路径；
 * field_get 按 valuelen 累积（multipart 续块 key=NULL）；field_store 建
 * UploadedFile（超限 unlink + error=1）。
 * 注意：multipart 续块 key==NULL = 当前字段续块（civetweb handle_form.inl
 * get_block>0 分支），首块 key 才带字段名。 */

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

/* 落当前文本字段（acc → form[cur_key]）并复位 */
static void teb_form_flush_text(FormCtx *fc) {
    if (fc->cur_key[0] == 0) return;
    TVal key = TV_STR(tstr_new(fc->cur_key, strlen(fc->cur_key)));
    TVal val = TV_STR(tstr_new(fc->acc ? fc->acc : "", fc->acc_len));
    tarr_set(fc->form, key, val);   /* key 非 interned → 下方 release；val 转移 */
    tval_release(&key);
    fc->cur_key[0] = 0;
    fc->acc_len = 0;
}

static int teb_form_field_found(const char *key, const char *filename,
                                char *path, size_t pathlen, void *user_data) {
    FormCtx *fc = (FormCtx *)user_data;
    teb_form_flush_text(fc);   /* 上一文本字段收尾 */

    if (filename && filename[0]) {
        /* 文件字段：记字段名/申报名，生成 <tmp_dir>/tphp_up_<pid>_<seq>。
         * seq 取全局自增（mu 内）——并发 multipart 各请求从 0 起会撞名 */
        snprintf(fc->cur_fkey, sizeof fc->cur_fkey, "%s", key ? key : "");
        snprintf(fc->cur_fname, sizeof fc->cur_fname, "%s", filename);
        teb_lock();
        int64_t seq = g_teb.up_seq++;
        uv_mutex_unlock(&g_teb.mu);
        int n = snprintf(path, pathlen, "%s/tphp_up_%lu_%lld",
                         fc->tmp_dir, (unsigned long)uv_os_getpid(),
                         (long long)seq);
        if (n < 0 || (size_t)n >= pathlen) {
            fc->aborted = true;   /* tmp 路径超 512B */
            return MG_FORM_FIELD_STORAGE_ABORT;
        }
        return MG_FORM_FIELD_STORAGE_STORE;
    }
    return MG_FORM_FIELD_STORAGE_GET;   /* 文本字段（cur_key 由 field_get 首块记） */
}

static int teb_form_field_get(const char *key, const char *value,
                              size_t valuelen, void *user_data) {
    FormCtx *fc = (FormCtx *)user_data;
    if (key) {
        /* 首块：新文本字段（civetweb 已 url 解码） */
        snprintf(fc->cur_key, sizeof fc->cur_key, "%s", key);
        fc->acc_len = 0;
    }
    /* 上限：单请求累计（文本字段值）超 upload_max_bytes → 中止解析。
     * 驱动中止后仍返回 field_count（≥0），必须自记标记供 form() 抛错。 */
    if (fc->total + (int64_t)valuelen > fc->max_bytes) {
        fc->aborted = true;
        fc->over_limit = true;
        return MG_FORM_FIELD_HANDLE_ABORT;
    }
    if (fc->acc_len + valuelen + 1 > fc->acc_cap) {
        size_t nc = fc->acc_cap ? fc->acc_cap : 256;
        while (nc < fc->acc_len + valuelen + 1) nc *= 2;
        char *na = tm_realloc(fc->acc, nc);
        if (!na) {
            fc->aborted = true;
            return MG_FORM_FIELD_HANDLE_ABORT;
        }
        fc->acc = na;
        fc->acc_cap = nc;
    }
    memcpy(fc->acc + fc->acc_len, value, valuelen);   /* 二进制安全 */
    fc->acc_len += valuelen;
    fc->acc[fc->acc_len] = 0;
    fc->total += (int64_t)valuelen;
    return MG_FORM_FIELD_HANDLE_GET;
}

/* UploadedFile 构建（props: 0=name 1=type 2=tmp 3=size 4=error 5=moved）。
 * name_str/tmp_str 为 +1 所有权转移（可 NULL → 空串）。 */
static TObj *teb_upfile_new(const char *fname, const char *tmp_path,
                            int64_t size, int err) {
    TObj *uf = tphp_obj_alloc(g_upfile_cls);
    uf->props[0] = TV_STR(tstr_new(fname ? fname : "",
                                   fname ? strlen(fname) : 0));
    const char *mime = (fname && fname[0]) ? mg_get_builtin_mime_type(fname) : NULL;
    uf->props[1] = TV_STR(tstr_new(mime ? mime : "text/plain",
                                   mime ? strlen(mime) : 10));
    uf->props[2] = tmp_path ? TV_STR(tstr_new(tmp_path, strlen(tmp_path)))
                            : TV_STR(tstr_new("", 0));
    uf->props[3] = TV_INT(size);
    uf->props[4] = TV_INT(err);
    uf->props[5] = TV_FALSE;
    return uf;
}

static int teb_form_field_store(const char *path, long long file_size,
                                void *user_data) {
    FormCtx *fc = (FormCtx *)user_data;
    int err = 0;
    const char *kept_path = path;
    if (file_size > fc->max_bytes || fc->total > fc->max_bytes) {
        teb_fs_unlink(path);   /* 超限：删 tmp，呈现 error=1 */
        err = 1;               /* UPLOAD_ERR_INI_SIZE */
        kept_path = NULL;
    }
    TObj *uf = teb_upfile_new(fc->cur_fname, kept_path,
                              (int64_t)file_size, err);
    TVal key = TV_STR(tstr_new(fc->cur_fkey, strlen(fc->cur_fkey)));
    TVal val = TV_OBJ(uf);   /* rc=1 转移给数组 */
    tarr_set(fc->files, key, val);
    tval_release(&key);
    return MG_FORM_FIELD_HANDLE_NEXT;
}

/* multipart 解析驱动（会耗尽 body——文档注明勿先调 body()）。
 * 成功：缓存 props[2]（文本字段）与 props[3]（文件）。
 * 失败（驱动 rc<0，或 field_* 回调返回 ABORT——注意驱动中止后仍返
 * field_count≥0，只能凭 fc.aborted 自记标记）：半截文本字段不入缓存；
 * 完整接收的文件仍缓存 props[3]（drain 回收 tmp 需要，file()/files()
 * 照常可取）；置 props[2]=INT 标记 → form() 本次与后续调用均抛 Error。 */
static void teb_form_parse_multipart(VM *vm, TObj *req,
                                     struct mg_connection *conn) {
    FormCtx fc;
    memset(&fc, 0, sizeof fc);
    fc.form = tarr_new();
    fc.files = tarr_new();
    fc.max_bytes = g_teb.upload_max_bytes;
    snprintf(fc.tmp_dir, sizeof fc.tmp_dir, "%s", g_teb.upload_tmp_dir);

    struct mg_form_data_handler fdh;
    memset(&fdh, 0, sizeof fdh);
    fdh.field_found = teb_form_field_found;
    fdh.field_get = teb_form_field_get;
    fdh.field_store = teb_form_field_store;
    fdh.user_data = &fc;

    int rc = mg_handle_form_request(conn, &fdh);
    if (rc < 0 || fc.aborted) {
        /* raise 不返回：先释放半截文本表与缓冲 → 落 files 缓存 → 置标记 */
        if (fc.acc) tm_free(fc.acc);
        TVal drop = TV_ARR(fc.form);
        tval_release(&drop);
        teb_set_slot(req, 3, TV_ARR(fc.files));
        teb_set_slot(req, 2, TV_INT(fc.over_limit ? 1 : 2));
        if (fc.over_limit)
            tphp_vm_raise(vm, "Error",
                          "Tphp\\Teb\\Request::form(): form data exceeds"
                          " upload_max_bytes (%lld bytes)",
                          (long long)g_teb.upload_max_bytes);
        else
            tphp_vm_raise(vm, "Error",
                          "Tphp\\Teb\\Request::form(): multipart form"
                          " parse failed");
    }
    teb_form_flush_text(&fc);             /* 最后一个文本字段 */
    if (fc.acc) tm_free(fc.acc);
    teb_set_slot(req, 2, TV_ARR(fc.form));
    teb_set_slot(req, 3, TV_ARR(fc.files));
}

/* 回调完成后回收未 moved 的上传 tmp（文档注明：moveTo 须在回调内完成） */
static void teb_reclaim_tmp(TObj *req) {
    if (req->props[3].t != T_ARR) return;
    TArr *a = (TArr *)req->props[3].u.p;
    tarr_sync_buckets(a);
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
        TVal v = a->data[bk->val_off];
        if (v.t != T_OBJ) continue;
        TObj *uf = (TObj *)v.u.p;
        if (!teb_is_cls(uf, g_upfile_cls, "Tphp\\Teb\\UploadedFile")) continue;
        if (uf->props[5].t == T_TRUE) continue;
        if (uf->props[2].t != T_STR) continue;
        TStr *t = (TStr *)uf->props[2].u.p;
        if (t->len == 0) continue;
        teb_fs_unlink(tstr_val(t));
        teb_set_slot(uf, 2, TV_STR(tstr_new("", 0)));
    }
}

/* ---- Response 静态工厂构建 helper（doc/API.md Response 静态工厂） ----
 * body/headers 为 +1 所有权转移。def_ctype：非 NULL 且用户头无
 * content-type（ci）时预置；location：非 NULL 时最后强制写入。 */
static TObj *teb_resp_build(TStr *body, int64_t status, TArr *headers,
                            const char *def_ctype, const char *location) {
    TObj *o = tphp_obj_alloc(g_resp_cls);
    /* 不跑 ctor：手动布 props（0=status 1=body 2=headers 3=cookies） */
    o->props[0] = TV_INT(status);
    o->props[1] = TV_STR(body);
    o->props[2] = headers ? TV_ARR(headers) : TV_ARR(tarr_new());
    o->props[3] = TV_ARR(tarr_new());

    TArr *h = (TArr *)o->props[2].u.p;
    if (def_ctype) {
        bool has_ct = false;
        tarr_sync_buckets(h);
        for (uint32_t b = 0; b < h->bcap; b++) {
            ABucket *bk = &h->buckets[b];
            if (bk->key.t != T_STR || bk->val_off == UINT32_MAX) continue;
            if (tstr_case_eq(tstr_val((TStr *)bk->key.u.p), "Content-Type")) {
                has_ct = true;
                break;
            }
        }
        if (!has_ct) {
            TVal k = TV_STR(tstr_intern("Content-Type", 12));
            TVal v = TV_STR(tstr_new(def_ctype, strlen(def_ctype)));
            tarr_set(h, k, v);   /* key interned；val rc=1 转移 */
        }
    }
    if (location) {
        TVal k = TV_STR(tstr_intern("Location", 8));
        TVal v = TV_STR(tstr_new(location, strlen(location)));
        tarr_set(h, k, v);   /* 最后写入覆盖用户同名项 */
    }
    return o;
}

/* ---- drain 核心：取批 → 构造 Request → 调回调 → 写响应 → 唤醒 ---- */

/* 批末真停（doc/API.md Web::stop）：shutdown+broadcast → join 执行池 →
 * mg_stop join → 清队列 → 释放 web_obj 引用与 props[0] → 释放配置（含
 * nf 退休链）。不依赖 self。顺序约束：所有消费者（池/主线程）都必须在
 * mg_stop 与配置释放之前退出，否则会踩在飞请求或悬挂配置指针上。 */
static void teb_real_stop(void) {
    teb_lock();
    g_teb.shutdown = true;
    uv_cond_broadcast(&g_teb.cv);
    uv_mutex_unlock(&g_teb.mu);

    for (int i = 0; i < g_teb.pool_n; i++)   /* 执行池（批3 创建；0 时空转） */
        uv_thread_join(&g_teb.pool_tids[i]);
    g_teb.pool_n = 0;

    mg_stop(g_teb.ctx);   /* join 全部 worker；此后队列必为空 */
    g_teb.ctx = NULL;
    g_teb.req_head = g_teb.req_tail = NULL;

    if (g_teb.web_obj.t == T_OBJ) {
        TObj *o = (TObj *)g_teb.web_obj.u.p;
        o->props[0] = TV_NULL;
        tval_release(&g_teb.web_obj);
        g_teb.web_obj = TV_NULL;
    }

    for (TebRoute *r = g_teb.routes; r; ) {
        TebRoute *nx = r->next;
        if (r->uri) tm_free(r->uri);
        if (r->cb_blob) tm_free(r->cb_blob);
        if (r->segs) {
            for (int i = 0; i < r->n_seg; i++) tm_free(r->segs[i].text);
            tm_free(r->segs);
        }
        tval_release(&r->callable);
        tm_free(r);
        r = nx;
    }
    g_teb.routes = NULL;
    if (g_teb.nf) {
        if (g_teb.nf->cb_blob) tm_free(g_teb.nf->cb_blob);
        tval_release(&g_teb.nf->callable);
        tm_free(g_teb.nf);
        g_teb.nf = NULL;
    }
    for (TebRoute *nr = g_teb.nf_retired; nr; ) {   /* 被替换的旧 nf */
        TebRoute *nx = nr->next;
        if (nr->cb_blob) tm_free(nr->cb_blob);
        tval_release(&nr->callable);
        tm_free(nr);
        nr = nx;
    }
    g_teb.nf_retired = NULL;
    /* P1-2 before 链 */
    for (TebRoute *b = g_teb.befores; b; ) {
        TebRoute *nx = b->next;
        if (b->cb_blob) tm_free(b->cb_blob);
        tval_release(&b->callable);
        tm_free(b);
        b = nx;
    }
    g_teb.befores = NULL;
    /* P1-3 静态目录映射 */
    for (TebStatic *ts = g_teb.statics; ts; ) {
        TebStatic *nx = ts->next;
        tm_free(ts->prefix);
        tm_free(ts->dir);
        tm_free(ts);
        ts = nx;
    }
    g_teb.statics = NULL;
    /* P1-4 WS：全部连接登记摘链（obj props[0] 置 0 → send/close 抛 Error）
     * + 释放引用；ws_regs 注册释放；分片装配残留回收。mg_stop 已 join，单线程免锁。 */
    TebWsSock *pend = g_teb.ws_socks;
    g_teb.ws_socks = NULL;
    while (pend) {
        TebWsSock *n = pend;
        pend = n->next;
        n->obj->props[0] = TV_NULL;
        tval_release(&(TVal){ .u.p = n->obj, .t = T_OBJ });
        tm_free(n);
    }
    TebWsFrag *wf = g_teb.ws_frags;
    while (wf) {
        TebWsFrag *nx = wf->next;
        tm_free(wf->buf);
        tm_free(wf);
        wf = nx;
    }
    g_teb.ws_frags = NULL;
    for (TebWsReg *rg = g_teb.ws_regs; rg; ) {
        TebWsReg *nx = rg->next;
        if (rg->uri) tm_free(rg->uri);
        for (int i = 0; i < 3; i++)
            if (rg->cb_blob[i]) tm_free(rg->cb_blob[i]);
        tval_release(&rg->on_open);
        tval_release(&rg->on_msg);
        tval_release(&rg->on_close);
        tm_free(rg);
        rg = nx;
    }
    g_teb.ws_regs = NULL;
    /* 执行池快照释放（join 与 mg_stop 已在上方完成 → 无线程再读） */
    if (g_teb.share) { tm_free(g_teb.share); g_teb.share = NULL; }
    g_teb.n_share = 0;
    if (g_teb.const_blob) { tm_free(g_teb.const_blob); g_teb.const_blob = NULL; }
    g_teb.const_blob_len = 0;
    if (g_teb.autoload_blob) {
        tm_free(g_teb.autoload_blob);
        g_teb.autoload_blob = NULL;
    }
    g_teb.autoload_blob_len = 0;
    g_teb.pools_armed = false;
}

/* 非主线程 stop() 的信号轮询（run/drain 每轮调用）：主线程执行真停 */
static void teb_poll_stop_request(void) {
    bool s = false;
    teb_lock();
    if (g_teb.stop_requested) {
        s = true;
        g_teb.stop_requested = false;
    }
    uv_mutex_unlock(&g_teb.mu);
    if (s && g_teb.ctx) teb_real_stop();
}

/* P1-4 摘除某连接的全部 WS 登记（mu 内摘链置 0，锁外释放引用） */
static void teb_ws_close_conn(struct mg_connection *conn) {
    TebWsSock *pend = NULL;
    teb_lock();
    TebWsSock **pp = &g_teb.ws_socks;
    while (*pp) {
        TebWsSock *n = *pp;
        if (n->conn == conn) {
            n->obj->props[0] = TV_NULL;   /* send/ping/close 此后抛 Error */
            *pp = n->next;
            n->next = pend;               /* 复用 next 域组待释放链 */
            pend = n;
        } else {
            pp = &n->next;
        }
    }
    uv_mutex_unlock(&g_teb.mu);
    while (pend) {
        TebWsSock *n = pend;
        pend = n->next;
        tval_release(&(TVal){ .u.p = n->obj, .t = T_OBJ });
        tm_free(n);
    }
}

/* WS 分片装配 helpers：链表结构走 ws_frag_mu（同连接帧经队列 done 协议
 * 天然串行，节点 buffer 归本连接线程私有——append 免锁） */

static void teb_frag_lock(void) { uv_mutex_lock(&g_teb.ws_frag_mu); }
static void teb_frag_unlock(void) { uv_mutex_unlock(&g_teb.ws_frag_mu); }

static TebWsFrag *teb_ws_frag_find(struct mg_connection *conn) {
    teb_frag_lock();
    TebWsFrag *r = NULL;
    for (TebWsFrag *f = g_teb.ws_frags; f; f = f->next)
        if (f->conn == conn) { r = f; break; }
    teb_frag_unlock();
    return r;
}

static void teb_ws_frag_drop(struct mg_connection *conn) {
    teb_frag_lock();
    for (TebWsFrag **pp = &g_teb.ws_frags; *pp; pp = &(*pp)->next) {
        if ((*pp)->conn == conn) {
            TebWsFrag *f = *pp;
            *pp = f->next;
            teb_frag_unlock();
            tm_free(f->buf);
            tm_free(f);
            return;
        }
    }
    teb_frag_unlock();
}

static bool teb_ws_frag_append(TebWsFrag *f, const char *data, size_t len) {
    if (!len) return true;
    if (f->len + len > f->cap) {
        size_t nc = f->cap ? f->cap : 256;
        while (nc < f->len + len) nc *= 2;
        char *nb = tm_realloc(f->buf, nc);
        if (!nb) return false;
        f->buf = nb;
        f->cap = nc;
    }
    memcpy(f->buf + f->len, data, len);
    f->len += len;
    return true;
}

/* 连接级 close_requested：置首个登记节点 props[1]（OPEN 节点必在），
 * ws_data_cb 醒来后返回 0 → civetweb 断开 */
static void teb_ws_mark_closed(struct mg_connection *conn) {
    teb_lock();
    for (TebWsSock *n = g_teb.ws_socks; n; n = n->next) {
        if (n->conn == conn) {
            n->obj->props[1] = TV_TRUE;
            break;
        }
    }
    uv_mutex_unlock(&g_teb.mu);
}

/* ---- 执行池：配置 blob 快照与回调本地化 ----
 * 发布点（main，mu 内或 start）把回调 serialize 成 C 字节 blob；池线程
 * 按 (节点指针, config_gen) 缓存本地闭包副本（本线程堆/gc_list 独享），
 * 换代整体作废重建。主线程永远直用共享 callable（g_in_pool_thread=false）。 */

/* serialize 一个回调进节点 blob（覆盖前释放旧的）。vm 传当前主线程——
 * 'C' 格式顺带 freeze owner Program + 进程级注册（池线程按 id 解析）。 */
static void teb_snapshot_cb(TVal cb, char **blob_out, uint32_t *len_out) {
    if (*blob_out) { tm_free(*blob_out); *blob_out = NULL; *len_out = 0; }
    TStr *s = tphp_serialize_val((VM *)tphp_vm_current, cb);
    if (!s) return;
    char *b = tm_malloc(s->len ? s->len : 1);
    memcpy(b, tstr_val(s), s->len);
    *blob_out = b;
    *len_out = (uint32_t)s->len;
    tval_release(&(TVal){ .t = T_STR, .u.p = s });
}

/* 取可用于调用的回调（+1）：主线程 = 共享对象 addref；池线程 = 反序列化
 * 本地副本（缓存，换代失效）。unserialize 失败 → raise（发布点预序列化
 * 保证正常不发生）。 */
static TVal teb_cb_acquire(VM *vm, const void *key, TVal shared,
                           const char *blob, uint32_t blen) {
    if (!g_in_pool_thread || !blob || blen == 0) {
        tval_addref(shared);
        return shared;
    }
    teb_lock();
    uint64_t gen = g_teb.config_gen;
    uv_mutex_unlock(&g_teb.mu);
    if (gen != g_local_gen) {   /* 配置换代：释放旧本地副本（本线程拥有） */
        for (int i = 0; i < g_local_n; i++) tval_release(&g_local_cbs[i].local);
        g_local_n = 0;
        g_local_gen = gen;
    }
    for (int i = 0; i < g_local_n; i++) {
        if (g_local_cbs[i].key == key) {
            tval_addref(g_local_cbs[i].local);
            return g_local_cbs[i].local;
        }
    }
    TVal out = TV_NULL;
    if (!tphp_unserialize_val(NULL, blob, blen, &out))
        tphp_vm_raise(vm, "Error",
                      "Teb: failed to rebuild route callback in pool thread");
    if (g_local_n == g_local_cap) {
        g_local_cap = g_local_cap ? g_local_cap * 2 : 16;
        g_local_cbs = tm_realloc(g_local_cbs,
                                 sizeof(TebLocalCb) * (size_t)g_local_cap);
    }
    g_local_cbs[g_local_n].key = key;
    g_local_cbs[g_local_n].gen = gen;
    g_local_cbs[g_local_n].local = out;   /* 存 +1（unserialize 归本线程） */
    g_local_n++;
    tval_addref(out);                    /* 调用方份 */
    return out;
}

/* 池线程退出前释放本地闭包缓存（含捕获对象——必须在本线程释放，F1 规则） */
static void teb_local_cache_free(void) {
    for (int i = 0; i < g_local_n; i++) tval_release(&g_local_cbs[i].local);
    tm_free(g_local_cbs);
    g_local_cbs = NULL;
    g_local_n = g_local_cap = 0;
    g_local_gen = 0;
}

/* P1-2 跑 before 链 + 分发（REQ/STATIC 共用）。返回 false = 用户回调抛异常
 * （响应由外层按流式状态收尾）；true = 响应已写出或流式接管。 */
static bool teb_dispatch(VM *vm, TObj *req, struct mg_connection *conn,
                         TebRoute *route, const char *static_path,
                         long long static_size) {
    /* before 链（按注册序）；任一 before 内 beginStream → 后续 before 与
     * 路由/静态全部跳过（外层流式收尾）。
     * 先在 mu 内快照节点指针数组再锁外执行——尾插发布的链表 next 会随
     * 并发注册变化，持锁跨回调也无法安全边走边读；节点到 stop 前不释放。 */
    int n_before = 0;
    teb_lock();
    for (TebRoute *b = g_teb.befores; b; b = b->next) n_before++;
    uv_mutex_unlock(&g_teb.mu);
    TebRoute **bs = n_before
        ? tm_malloc(sizeof(TebRoute *) * (size_t)n_before) : NULL;
    int bi = 0;
    teb_lock();
    for (TebRoute *b = g_teb.befores; b && bi < n_before; b = b->next)
        bs[bi++] = b;
    uv_mutex_unlock(&g_teb.mu);
    n_before = bi;

    for (int i = 0; i < n_before; i++) {
        TebRoute *b = bs[i];
        if (req->props[4].t == T_OBJ) { tm_free(bs); return true; }
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        TVal cb = teb_cb_acquire(vm, b, b->callable, b->cb_blob, b->cb_blob_len);
        bool ok = tphp_vm_call_bounded(vm, cb, &argv, 1, &out);
        tval_release(&cb);
        tval_release(&argv);
        if (!ok) { tm_free(bs); return false; }   /* before 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && teb_is_cls((TObj *)out.u.p, g_resp_cls,
                                         "Tphp\\Teb\\Response")) {
            teb_write_response(conn, (TObj *)out.u.p, req);   /* 短路 */
            tval_release(&out);
            tm_free(bs);
            return true;
        }
        if (out.t != T_NULL) {
            tphp_warn("PHP Warning: Teb before callback must return null "
                      "or a Tphp\\Teb\\Response instance\n");
            tval_release(&out);
            teb_write_500(conn, req);
            tm_free(bs);
            return true;
        }
        tval_release(&out);
    }
    tm_free(bs);
    if (req->props[4].t == T_OBJ) return true;   /* before 内 beginStream */

    if (route) {
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        TVal cb = teb_cb_acquire(vm, route, route->callable,
                                 route->cb_blob, route->cb_blob_len);
        bool ok = tphp_vm_call_bounded(vm, cb, &argv, 1, &out);
        tval_release(&cb);
        tval_release(&argv);
        if (!ok) return false;   /* route 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && teb_is_cls((TObj *)out.u.p, g_resp_cls,
                                         "Tphp\\Teb\\Response")) {
            teb_write_response(conn, (TObj *)out.u.p, req);
        } else if (req->props[4].t == T_OBJ) {
            /* 流式接管：返回值一律忽略 */
        } else {
            tphp_warn("PHP Warning: Teb route callback must return "
                      "a Tphp\\Teb\\Response instance\n");
            teb_write_500(conn, req);
        }
        tval_release(&out);
        return true;
    }

    if (static_path) {
        /* 会话注入：mg_send_mime_file2 的 additional_headers 挂 Set-Cookie
         * （civetweb 源码零改动；注意 304 分支不透传 → 文档限制） */
        char *cl = teb_sess_cookie_line(teb_sess_of(req));
        mg_send_mime_file2(conn, static_path, NULL, cl);
        if (cl) tm_free(cl);
        teb_stat_resp(200);
        teb_lock();
        g_teb.st.bytes_sent += static_size;   /* 近似：内部写出不走 teb_wr */
        uv_mutex_unlock(&g_teb.mu);
        return true;
    }
    return true;
}

/* WS 数据消息交付（完整帧或分片装配完成）：现造 WebSocket + 登记 + onMessage。
 * onMessage 抛异常 → 打印 + close_requested（data 回调返回 0 断连）。 */
static void teb_ws_deliver_msg(VM *vm, TebWsReg *reg, struct mg_connection *conn,
                               int opcode, const char *data, size_t len) {
    teb_lock();
    g_teb.st.ws_frames++;
    uv_mutex_unlock(&g_teb.mu);
    TObj *s = tphp_obj_alloc(g_ws_cls);
    s->props[0] = TV_INT((int64_t)(uintptr_t)conn);
    s->props[1] = TV_FALSE;
    teb_lock();
    TebWsSock *node = tm_malloc(sizeof *node);
    node->obj = s;
    node->conn = conn;
    node->next = g_teb.ws_socks;
    g_teb.ws_socks = node;
    uv_mutex_unlock(&g_teb.mu);
    tval_addref(TV_OBJ(s));   /* node 持有份 */
    if (reg->on_msg.t != T_NULL) {
        TStr *ds = tstr_new(data ? data : "", len);
        TVal args2[3] = { TV_OBJ(s), TV_STR(ds), TV_INT(opcode) };
        TVal out = TV_NULL;
        TVal mcb = teb_cb_acquire(vm, &reg->on_msg, reg->on_msg,
                                  reg->cb_blob[1], reg->cb_blob_len[1]);
        if (!tphp_vm_call_bounded(vm, mcb, args2, 3, &out)) {
            tphp_warn("PHP Warning: Teb WS onMessage callback raised "
                      "(closing connection)\n");
            s->props[1] = TV_TRUE;   /* close_requested → data 回调返回 0 */
        }
        tval_release(&mcb);
        tval_release(&out);
        tval_release(&args2[0]);   /* node 持份保留 */
        tval_release(&args2[1]);   /* 帧数据串（drain 持份） */
        /* args2[2] int 无引用 */
    }
}

/* WS 帧级写出（drain 侧；mg_lock_connection 防并发写） */
static void teb_ws_write_frame(struct mg_connection *conn, int opcode,
                               const char *data, size_t len) {
    mg_lock_connection(conn);
    mg_websocket_write(conn, opcode, data, len);
    mg_unlock_connection(conn);
}

static void teb_drain_mode(VM *vm, int mode) {
    /* mode 0 = 全部节点（主线程 drain/run，含 WS）；mode 1 = 仅 HTTP 节点
     * （池泵专用：WS 回调固定主线程——跨帧/跨连接的 global 状态（房间表等）
     * 与主脚本种子保持同线程，用户"跨回调共享状态用 global"的既有心智不破）。 */
    teb_lock();
    TebRequest *q = NULL;
    bool waited = false;
    for (;;) {
        q = NULL;
        if (mode == 0) {
            q = g_teb.req_head;
            if (q) g_teb.req_head = g_teb.req_tail = NULL;
        } else {
            /* 摘出全部非 WS 节点（保持原序）；WS 留队给主线程 */
            TebRequest *h = NULL, **tp = &h;
            for (TebRequest **pp = &g_teb.req_head; *pp; ) {
                TebRequest *n = *pp;
                if (n->kind == TEQ_REQ || n->kind == TEQ_STATIC) {
                    *pp = n->next;
                    n->next = NULL;
                    *tp = n;
                    tp = &n->next;
                } else {
                    pp = &n->next;
                }
            }
            q = h;
            /* 队尾重算（摘走尾节点时 req_tail 会悬空） */
            g_teb.req_tail = NULL;
            for (TebRequest *r = g_teb.req_head; r; r = r->next)
                if (!r->next) { g_teb.req_tail = r; break; }
        }
        if (q || g_teb.shutdown || waited) break;
        /* 单次泵语义：至多一次 10ms 等待，随后无论有无货都返回——
         * drain() 空队阻塞到"有请求"会卡死所有窗口驱动的用户脚本。 */
        waited = true;
        uv_cond_timedwait(&g_teb.cv, &g_teb.mu, 10 * 1000 * 1000);  /* 10ms */
    }
    /* mu 内整批标记 taken：此后 shutdown 不得提前放行对应 worker */
    for (TebRequest *t = q; t; t = t->next) t->taken = true;
    g_teb.draining = true;
    uv_mutex_unlock(&g_teb.mu);

    while (q) {
        TebRequest *next = q->next;

        if (q->kind == TEQ_REQ || q->kind == TEQ_STATIC) {
            teb_lock();
            g_teb.st.requests_total++;   /* 与入口 413 计数（thunk 线程）同锁 */
            g_teb.st.requests_active++;
            uv_mutex_unlock(&g_teb.mu);
            /* 构造回调入参 Request（不走 ctor；消费者持有其 rc）。
             * props: 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
             *        5=body_cache 6=qp_cache 7=cookie_cache 8=session */
            TObj *req = tphp_obj_alloc(g_req_cls);
            req->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            req->props[1] = TV_ARR(teb_req_info_arr(q->conn, q));
            TVal argv = TV_OBJ(req);
            bool ok = teb_dispatch(vm, req, q->conn,
                                   q->kind == TEQ_REQ ? q->route : NULL,
                                   q->kind == TEQ_STATIC ? q->path : NULL,
                                   q->path_size);

            TVal sv = req->props[4];   /* beginStream 过（before 或 route）？ */
            TObj *stream = (sv.t == T_OBJ &&
                            teb_is_cls((TObj *)sv.u.p, g_stream_cls,
                                       "Tphp\\Teb\\Stream"))
                ? (TObj *)sv.u.p : NULL;
            if (stream) {
                /* 流式契约：返回值一律忽略；未 end 自动收尾；
                 * 异常时头已发仅置 ended 不写 500，头未发写 500 并锁死 Stream */
                if (!ok) {
                    if (stream->props[3].t == T_TRUE) {
                        stream->props[4] = TV_TRUE;
                    } else {
                        stream->props[3] = TV_TRUE;
                        stream->props[4] = TV_TRUE;
                        teb_write_500(q->conn, req);
                    }
                } else if (stream->props[4].t != T_TRUE) {
                    teb_stream_send_headers(stream);   /* 未发头 → 空体响应头 */
                    stream->props[4] = TV_TRUE;
                }
            } else if (!ok) {
                teb_write_500(q->conn, req);   /* 非 流式 + 回调异常 */
            }
            teb_reclaim_tmp(req);      /* 未 moveTo 的上传 tmp 回收 */
            teb_session_flush(req);    /* 会话落盘（dispatch 全分支必经此点） */
            tval_release(&argv);   /* Request 对象（回调捕获则继续存活） */
            teb_lock();
            g_teb.st.requests_active--;
            uv_mutex_unlock(&g_teb.mu);
        } else if (q->kind == TEQ_WS_OPEN) {
            teb_lock();
            g_teb.st.ws_active++;
            uv_mutex_unlock(&g_teb.mu);
            TebWsReg *reg = q->ws_reg;
            TObj *s = tphp_obj_alloc(g_ws_cls);
            s->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            s->props[1] = TV_FALSE;
            teb_lock();
            TebWsSock *node = tm_malloc(sizeof *node);
            node->obj = s;
            node->conn = q->conn;
            node->next = g_teb.ws_socks;
            g_teb.ws_socks = node;
            uv_mutex_unlock(&g_teb.mu);
            tval_addref(TV_OBJ(s));   /* node 持有份（生命周期 = 连接） */
            /* on_open(WebSocket $s, Request $r)——返回值忽略；异常打印忽略 */
            TObj *rq = tphp_obj_alloc(g_req_cls);
            rq->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            rq->props[1] = TV_ARR(teb_req_info_arr(q->conn, q));
            TVal args2[2] = { TV_OBJ(s), TV_OBJ(rq) };
            TVal out = TV_NULL;
            TVal ocb = teb_cb_acquire(vm, &reg->on_open, reg->on_open,
                                      reg->cb_blob[0], reg->cb_blob_len[0]);
            if (!tphp_vm_call_bounded(vm, ocb, args2, 2, &out))
                tphp_warn("PHP Warning: Teb WS onOpen callback raised (ignored)\n");
            tval_release(&ocb);
            tval_release(&out);
            tval_release(&args2[0]);   /* node 持份保留 */
            teb_session_flush(rq);     /* WS 无 HTTP 响应可注入；数据照常落盘 */
            tval_release(&args2[1]);   /* Request 引用尽（drain 持份） */
        } else if (q->kind == TEQ_WS_MSG) {
            /* 帧层（RFC6455 子集）：控制帧不出回调——PING 自动回 PONG、
             * PONG 忽略、CLOSE 回 CLOSE 并断连；数据帧 FIN 分片装配后交付，
             * bits 交付为消息 opcode（1=文本 2=二进制）。 */
            int op = q->ws_bits & 0x0F;
            bool fin = (q->ws_bits & 0x80) != 0;
            const char *pay = q->ws_data ? q->ws_data : "";
            if (op == 0x9) {                    /* PING → PONG（同载荷） */
                teb_ws_write_frame(q->conn, MG_WEBSOCKET_OPCODE_PONG,
                                   pay, q->ws_data_len);
            } else if (op == 0xa) {             /* PONG → 忽略 */
            } else if (op == 0x8) {             /* CLOSE → 回 CLOSE + 置关断 */
                teb_ws_write_frame(q->conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,
                                   pay, q->ws_data_len);
                teb_ws_mark_closed(q->conn);
            } else if (op == 0x0 || op == 0x1 || op == 0x2) {
                TebWsFrag *fr = teb_ws_frag_find(q->conn);
                if (op == 0x0) {                /* 续帧 */
                    if (!fr) {                  /* 协议错：无进行中分片 → 断连 */
                        teb_ws_mark_closed(q->conn);
                    } else if (!teb_ws_frag_append(fr, pay, q->ws_data_len)
                               || fr->len > (size_t)g_teb.upload_max_bytes) {
                        teb_ws_frag_drop(q->conn);   /* 超限/ENOMEM → 断连 */
                        teb_ws_mark_closed(q->conn);
                    } else if (fin) {           /* 装配完成 → 交付 */
                        teb_ws_deliver_msg(vm, q->ws_reg, q->conn,
                                           fr->opcode, fr->buf, fr->len);
                        teb_ws_frag_drop(q->conn);
                    }
                } else {                        /* 首帧 TEXT/BINARY */
                    if (fin) {                  /* 单帧完整 → 直接交付 */
                        teb_ws_deliver_msg(vm, q->ws_reg, q->conn,
                                           op, pay, q->ws_data_len);
                    } else if (fr) {            /* 协议错：分片中插新数据帧 */
                        teb_ws_frag_drop(q->conn);
                        teb_ws_mark_closed(q->conn);
                    } else {
                        fr = tm_malloc(sizeof *fr);
                        memset(fr, 0, sizeof *fr);
                        fr->conn = q->conn;
                        fr->opcode = op;
                        teb_frag_lock();
                        fr->next = g_teb.ws_frags;
                        g_teb.ws_frags = fr;
                        teb_frag_unlock();
                        if (!teb_ws_frag_append(fr, pay, q->ws_data_len)
                            || fr->len > (size_t)g_teb.upload_max_bytes) {
                            teb_ws_frag_drop(q->conn);
                            teb_ws_mark_closed(q->conn);
                        }
                    }
                }
            } else {                            /* 保留 opcode（3-7,B-F）→ 断连 */
                teb_ws_mark_closed(q->conn);
            }
        } else if (q->kind == TEQ_WS_CLOSE) {
            teb_lock();
            g_teb.st.ws_active--;
            g_teb.st.ws_closed++;
            uv_mutex_unlock(&g_teb.mu);
            teb_ws_frag_drop(q->conn);   /* 未装配完的分片一并回收 */
            teb_ws_close_conn(q->conn);   /* 全部登记摘链 + props[0] 置 0 */
            TebWsReg *reg = q->ws_reg;
            if (reg->on_close.t != T_NULL) {
                /* on_close(WebSocket $s)——conn 在 close 回调阻塞期间仍有效 */
                TObj *s = tphp_obj_alloc(g_ws_cls);
                s->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
                s->props[1] = TV_TRUE;
                TVal args2[1] = { TV_OBJ(s) };
                TVal out = TV_NULL;
                TVal ccb = teb_cb_acquire(vm, &reg->on_close, reg->on_close,
                                          reg->cb_blob[2], reg->cb_blob_len[2]);
                if (!tphp_vm_call_bounded(vm, ccb, args2, 1, &out))
                    tphp_warn("PHP Warning: Teb WS onClose callback raised (ignored)\n");
                tval_release(&ccb);
                tval_release(&out);
                tval_release(&args2[1]);   /* drain 持份（一次性对象） */
            }
        }

        teb_lock();
        q->done = true;
        uv_cond_broadcast(&g_teb.cv);   /* 唤醒该请求的 worker */
        uv_mutex_unlock(&g_teb.mu);
        /* q 节点由等待中的 worker 自己释放 */
        q = next;
    }

    teb_lock();
    g_teb.draining = false;
    bool stop_now = g_teb.pending_stop;
    g_teb.pending_stop = false;
    uv_mutex_unlock(&g_teb.mu);
    if (stop_now && g_teb.ctx) {
        /* 主线程批末 → 直接真停（既有语义）；池线程批末 → 只置信号，
         * 由主线程 run/drain 轮询执行真停（real_stop 会 join 池线程，
         * 在池线程内执行会自 join 死锁）。 */
        uv_thread_t self = uv_thread_self();
        if (uv_thread_equal(&self, &g_teb.main_tid)) {
            teb_real_stop();
        } else {
            teb_lock();
            g_teb.stop_requested = true;
            uv_cond_broadcast(&g_teb.cv);
            uv_mutex_unlock(&g_teb.mu);
        }
    }
}

/* 主线程消费者：全部节点（run/drain/无池模式同路径） */
static void teb_drain_once(VM *vm) { teb_drain_mode(vm, 0); }

/* ---- 执行池（批3）：N 个独立 runtime 线程作为额外消费者 ----
 * 泵 = 泵 PHP：`while (\Tphp\Teb\Internal\pump()) {}`（pump Program），
 * 每轮 pump 调 drain 同款取批+处理（复用 teb_drain_once：mu 保护的取队
 * 对多消费者天然安全；空队 10ms 节拍，无忙等）。stop 信号链：池回调
 * stop() → stop_requested（批2）→ 主线程 run/drain poll → real_stop
 * （shutdown → join 池 → mg_stop → 释放配置，顺序见 teb_real_stop）。 */

/* 用户常量快照导入（池首泵一次；vm->consts 直写 = op_define_const 同款，
 * 去重靠 tstr_eq——seed 常量已由 vm_seed_consts 预置，重名跳过值也跳过） */
static void teb_import_consts(VM *vm) {
    if (g_consts_imported) return;
    g_consts_imported = true;
    const char *d = g_teb.const_blob;
    size_t n = g_teb.const_blob_len, pos = 0;
    if (!d || n < 4) return;
    uint32_t cnt;
    memcpy(&cnt, d, 4);
    pos = 4;
    for (uint32_t i = 0; i < cnt; i++) {
        if (pos + 4 > n) return;
        uint32_t nl;
        memcpy(&nl, d + pos, 4); pos += 4;
        if (pos + nl + 4 > n) return;
        TStr *nm = tstr_intern(d + pos, nl);
        pos += nl;
        uint32_t vl;
        memcpy(&vl, d + pos, 4); pos += 4;
        if (pos + vl > n) return;
        bool dup = false;
        for (int j = 0; j < vm->n_consts && !dup; j++)
            dup = tstr_eq(vm->consts[j].name, nm);
        if (!dup && vm->n_consts < 256) {
            TVal v = TV_NULL;
            if (tphp_unserialize_val(NULL, d + pos, vl, &v)) {
                vm->consts[vm->n_consts].name = nm;
                vm->consts[vm->n_consts].v = v;   /* 转移 unserialize +1 */
                vm->n_consts++;
            }
        }
        pos += vl;
    }
}

/* Internal::inPoolThread(): bool —— 当前线程是否为执行池线程（TLS 判定；
 * 回调内可用于分支/取证，如 autoload 快照在池线程生效的标记）。 */
static TVal nm_internal_in_pool_thread(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)self; (void)args; (void)n;
    return g_in_pool_thread ? TV_TRUE : TV_FALSE;
}

/* \Tphp\Teb\Internal\pump(): bool —— 池泵体。shutdown/ctx 失效 → false
 * （pump 循环退出 → run_closure 返回 → 线程收尾）。
 * 泵只消费 HTTP 节点（teb_drain_mode mode=1）：WS 回调固定主线程。 */
static TVal nm_pool_pump(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    if (!g_teb.ctx || g_teb.shutdown) return TV_FALSE;
    teb_import_consts(vm);
    /* 批B：autoload 导入（与 consts 同语义位：start 快照 → 池首泵导入；
     * setter 内部 addref（class.c），unserialize 的 +1 由本处 release） */
    if (!g_autoload_imported && g_teb.autoload_blob) {
        g_autoload_imported = true;
        TVal v = TV_NULL;
        if (tphp_unserialize_val(NULL, g_teb.autoload_blob,
                                 g_teb.autoload_blob_len, &v)) {
            tphp_class_set_autoload_val(v);
            tval_release(&v);
        } else {
            tphp_warn("PHP Warning: Teb autoload snapshot import failed in"
                      " pool thread\n");
        }
    }
    teb_drain_mode(vm, 1);
    return TV_TRUE;
}

/* pump 主闭包（worker_unserialize_closure 同款手工构造；fn = prog->main，
 * 零捕获；gc_register 后由 GC 回收，不显式释放——对齐 worker 先例） */
static TClosure *teb_make_main_clo(Program *p) {
    TClosure *clo = tm_malloc(sizeof *clo);
    memset(clo, 0, sizeof *clo);
    clo->h.rc = 1;
    clo->h.kind = GC_KIND_CLO;
    gc_register(&clo->h);
    clo->fn = p->main;
    clo->bfn = NULL;
    clo->n_cap = 0;
    clo->captures = NULL;
    clo->scope = NULL;
    return clo;
}

/* 池线程主体（worker.c L373-416 bootstrap 对照；无 mailbox 需求） */
static void teb_pool_thread(void *arg) {
    (void)arg;
    tphp_worker_init();          /* 线程内部类（先于 clone，worker 同序） */
    tphp_async_init();
    tphp_io_init();
    tphp_closure_class_init();
    tphp_class_tab_clone_thread();   /* master → 本线程（teb 7 类随之克隆） */
    tphp_sched_loop_init();

    /* TLS 类句柄重取：克隆体指针 ≠ master，不重取则 cls==g_resp_cls 判定
     * 全错。此时 tphp_vm_current 尚空 → class_resolve_lc 不触发 autoload。 */
    g_web_cls = class_resolve_lc("tphp\\teb\\web");
    g_req_cls = class_resolve_lc("tphp\\teb\\request");
    g_resp_cls = class_resolve_lc("tphp\\teb\\response");
    g_stream_cls = class_resolve_lc("tphp\\teb\\stream");
    g_upfile_cls = class_resolve_lc("tphp\\teb\\uploadedfile");
    g_ws_cls = class_resolve_lc("tphp\\teb\\websocket");
    g_sess_cls = class_resolve_lc("tphp\\teb\\session");
    if (!g_web_cls || !g_req_cls || !g_resp_cls || !g_stream_cls ||
        !g_upfile_cls || !g_ws_cls || !g_sess_cls) {
        tphp_warn("PHP Warning: Teb pool thread failed to resolve teb classes\n");
        return;
    }
    g_in_pool_thread = true;

    TClosure *clo = teb_make_main_clo(g_pump_prog);
    TVal ret = TV_NULL;
    tphp_vm_run_closure(g_pump_prog, g_teb.share, g_teb.n_share, clo, &ret);
    tval_release(&ret);
    teb_local_cache_free();      /* 捕获对象必须在本线程释放（F1 规则） */
}

/* 泵 Program（一次编译，进程级只读；源码恒定故永不失败——防御 warn 兜底） */
static Program *teb_pump_program(void) {
    if (!g_pump_prog) {
        static const char SRC[] =
            "<?php while (\\Tphp\\Teb\\Internal::pump()) {}";
        AStmtList *ast = tphp_parse_script(SRC, "teb-pump.php");
        if (!ast) {
            tphp_warn("PHP Warning: Teb pump program parse failed\n");
            return NULL;
        }
        g_pump_prog = tphp_compile(ast, "teb-pump.php");
        if (g_pump_prog) tphp_prog_freeze(g_pump_prog);
    }
    return g_pump_prog;
}

/* start 收尾处调用（mg_start 后、返回前）：创建池线程 */
static void teb_spawn_pools(void) {
    int want = (int)g_teb.callback_threads;
    if (want <= 0) return;
    if (want > 16) want = 16;
    if (!teb_pump_program()) {
        g_teb.pools_armed = false;
        return;
    }
    g_teb.pool_n = 0;
    for (int i = 0; i < want; i++) {
        if (uv_thread_create(&g_teb.pool_tids[i], teb_pool_thread, NULL) != 0)
            break;
        g_teb.pool_n++;
    }
    if (g_teb.pool_n < want)
        tphp_warn("PHP Warning: Teb started %d/%d callback threads\n",
                  g_teb.pool_n, want);
    if (g_teb.pool_n == 0) g_teb.pools_armed = false;   /* 全失败 → 主线程独跑 */
}

/* 用户常量快照（start 主线程构建）：u32 cnt + cnt×{u32 nl, name, u32 vl,
 * serialize(val)}。名字按字节存（池内重 intern），值走通用 serialize。 */
static char *teb_const_snapshot(uint32_t *out_len) {
    *out_len = 0;
    VM *mvm = (VM *)tphp_vm_current;
    if (!mvm) return NULL;
    uint32_t cnt = 0;
    for (int i = 0; i < mvm->n_consts; i++)
        if (mvm->consts[i].name) cnt++;
    if (!cnt) return NULL;
    size_t cap = 4096, len = 4;
    char *b = tm_malloc(cap);
    memcpy(b, &cnt, 4);
    for (int i = 0; i < mvm->n_consts; i++) {
        TStr *nm = mvm->consts[i].name;
        if (!nm) continue;
        TStr *vs = tphp_serialize_val(mvm, mvm->consts[i].v);
        if (!vs) continue;
        size_t need = 4 + nm->len + 4 + vs->len;
        while (len + need > cap) {
            cap *= 2;
            b = tm_realloc(b, cap);
        }
        uint32_t nl = nm->len, vl = vs->len;
        memcpy(b + len, &nl, 4); len += 4;
        memcpy(b + len, tstr_val(nm), nl); len += nl;
        memcpy(b + len, &vl, 4); len += 4;
        memcpy(b + len, tstr_val(vs), vl); len += vl;
        tval_release(&(TVal){ .t = T_STR, .u.p = vs });
    }
    *out_len = (uint32_t)len;
    return b;
}

/* 批B：autoload 回调快照（start 主线程；未注册返 NULL、不可序列化
 * warn+NULL——池线程无 autoloader 时类 miss 直接 Error，见 API.md 边界）。 */
static char *teb_autoload_snapshot(uint32_t *out_len) {
    *out_len = 0;
    TVal f = tphp_class_get_autoload_val();   /* 引擎 getter：+1 副本 */
    if (f.t == T_NULL) return NULL;
    TStr *s = tphp_serialize_val((VM *)tphp_vm_current, f);
    tval_release(&f);
    if (!s) {
        tphp_warn("PHP Warning: Teb spl_autoload callback is not serializable;"
                  " pool threads will have no autoloader\n");
        return NULL;
    }
    char *b = tm_malloc(s->len ? s->len : 1);
    memcpy(b, tstr_val(s), s->len);
    *out_len = (uint32_t)s->len;
    tval_release(&(TVal){ .t = T_STR, .u.p = s });
    return b;
}

/* start 时回填既有配置 blob（start 前注册是常态；与运行期发布点共用
 * teb_snapshot_cb）。start 单线程（未 draining）→ 免锁遍历安全。 */
static void teb_backfill_blobs(void) {
    for (TebRoute *r = g_teb.routes; r; r = r->next)
        teb_snapshot_cb(r->callable, &r->cb_blob, &r->cb_blob_len);
    for (TebRoute *b = g_teb.befores; b; b = b->next)
        teb_snapshot_cb(b->callable, &b->cb_blob, &b->cb_blob_len);
    if (g_teb.nf)
        teb_snapshot_cb(g_teb.nf->callable, &g_teb.nf->cb_blob,
                        &g_teb.nf->cb_blob_len);
    for (TebWsReg *rg = g_teb.ws_regs; rg; rg = rg->next) {
        teb_snapshot_cb(rg->on_open, &rg->cb_blob[0], &rg->cb_blob_len[0]);
        if (rg->on_msg.t != T_NULL)
            teb_snapshot_cb(rg->on_msg, &rg->cb_blob[1], &rg->cb_blob_len[1]);
        if (rg->on_close.t != T_NULL)
            teb_snapshot_cb(rg->on_close, &rg->cb_blob[2], &rg->cb_blob_len[2]);
    }
}

/* ---- Tphp\Teb\Web 原生方法 ---- */

/* 统一注册入口（批A，镜像 webman addRoute 先例）：route() 走 mask=0（任意
 * 方法），7 个方法快捷走各自位。模板解析在节点 malloc 前（raise 零泄漏）。 */
static TVal teb_route_add(VM *vm, void *self, const char *who,
                          TVal *args, int n, uint32_t mask) {
    const char *uri = arg_str(vm, args, n, 0, who);
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "%s() expects (string, callable)", who);
    TVal cb = args[1];
    switch (cb.t) {
    case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
    default:
        tphp_vm_raise(vm, "TypeError",
                      "%s(): callable expected as argument 2", who);
    }

    int n_seg = 0;
    TebSeg *segs = teb_route_parse(uri, &n_seg, vm, who);   /* 非法 → raise */

    TebRoute *r = tm_malloc(sizeof *r);
    r->uri = teb_dup(uri);
    r->callable = cb;
    tval_addref(cb);
    r->cb_blob = NULL;
    r->cb_blob_len = 0;
    r->methods = mask;
    r->segs = segs;
    r->n_seg = (uint8_t)n_seg;
    if (g_teb.pools_armed)   /* 池启用：发布点快照（可能 raise——须在锁外） */
        teb_snapshot_cb(cb, &r->cb_blob, &r->cb_blob_len);
    teb_lock();                 /* 发布：thunk 匹配在 worker 线程读链表 */
    r->next = g_teb.routes;
    g_teb.routes = r;
    g_teb.config_gen++;
    uv_mutex_unlock(&g_teb.mu);
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* route(string $uri, callable $cb): self —— 任意方法；start 前后均可注册 */
static TVal nm_web_route(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::route", args, n, 0);
}

/* 方法快捷注册（批A）：get/post/put/patch/delete/head/options 同款链式 self */
static TVal nm_web_get(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::get", args, n, TEB_M_GET);
}
static TVal nm_web_post(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::post", args, n, TEB_M_POST);
}
static TVal nm_web_put(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::put", args, n, TEB_M_PUT);
}
static TVal nm_web_patch(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::patch", args, n, TEB_M_PATCH);
}
static TVal nm_web_delete(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::delete", args, n, TEB_M_DELETE);
}
static TVal nm_web_head(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::head", args, n, TEB_M_HEAD);
}
static TVal nm_web_options(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::options", args, n, TEB_M_OPTIONS);
}

/* notFound(callable $cb): self —— 无路由命中时的回调；单条，重复调用替换。
 * 替换走「新节点 + 退休链」：thunk 可能正持有旧节点指针，就地改写 callable
 * 会撕引用计数——旧节点留到 stop 统一释放。 */
static TVal nm_web_not_found(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::notFound() expects a callable");
    TVal cb = args[0];
    switch (cb.t) {
    case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
    default:
        tphp_vm_raise(vm, "TypeError",
                      "Web::notFound(): callable expected as argument 1");
    }
    TebRoute *r = tm_malloc(sizeof *r);
    r->uri = NULL;
    r->callable = cb;
    tval_addref(cb);
    r->next = NULL;
    r->cb_blob = NULL;
    r->cb_blob_len = 0;
    r->methods = 0;
    r->segs = NULL;
    r->n_seg = 0;
    if (g_teb.pools_armed)
        teb_snapshot_cb(cb, &r->cb_blob, &r->cb_blob_len);
    teb_lock();
    if (g_teb.nf) {
        g_teb.nf->next = g_teb.nf_retired;
        g_teb.nf_retired = g_teb.nf;
    }
    g_teb.nf = r;
    g_teb.config_gen++;
    uv_mutex_unlock(&g_teb.mu);
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* start(array $options): self —— civetweb 配置项直通（自留键摘除） */
static TVal nm_web_start(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (g_teb.ctx)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Web::start(): server already running");
    if (n < 1 || args[0].t != T_ARR)
        tphp_vm_raise(vm, "TypeError", "Web::start() expects an array of options");

    /* civetweb 全局初始化（幂等）+ 同步原语初始化 + 状态复位——必须先于
     * mg_start 创建 worker 线程（首调即本函数，主线程串行，无竞态） */
    if (!g_teb.inited) {
        mg_init_library(0);
        g_teb.inited = true;
    }
    teb_lock();
    g_teb.shutdown = false;
    g_teb.draining = false;
    g_teb.pending_stop = false;
    g_teb.stop_requested = false;
    uv_mutex_unlock(&g_teb.mu);

    /* 辅助锁 + 主线程 id + 全局计数：mg_start 建线程前完成（批2 并发加固） */
    teb_aux_locks_init();
    g_teb.main_tid = uv_thread_self();
    g_teb.up_seq = 0;
    g_teb.config_gen = 0;
    g_teb.pool_n = 0;

    /* P1-1 stats 复位（started_at = 本次启动时刻） */
    memset(&g_teb.st, 0, sizeof g_teb.st);
    g_teb.st.started_at = (int64_t)time(NULL);

    /* 上传自留键缺省（doc/API.md UploadedFile 节：upload_tmp_dir / upload_max_bytes） */
    if (g_teb.upload_tmp_dir[0] == 0) {
        char t[380];
        size_t tl = sizeof t;
        if (uv_os_tmpdir(t, &tl) == 0 && tl > 0)
            snprintf(g_teb.upload_tmp_dir, sizeof g_teb.upload_tmp_dir,
                     "%.*s", (int)(tl > 379 ? 379 : tl), t);
        else
            snprintf(g_teb.upload_tmp_dir, sizeof g_teb.upload_tmp_dir, ".");
    }
    if (g_teb.upload_max_bytes <= 0)
        g_teb.upload_max_bytes = 64LL * 1024 * 1024;

    /* P1-5 Session 自留键缺省（doc/API.md start 配置表：
     * session_save_path / session_lifetime / session_gc_interval） */
    if (g_teb.session_save_path[0] == 0) {
        char t[380];
        size_t tl = sizeof t;
        if (uv_os_tmpdir(t, &tl) == 0 && tl > 0)
            snprintf(g_teb.session_save_path, sizeof g_teb.session_save_path,
                     "%.*s/tphp_sess", (int)(tl > 370 ? 370 : tl), t);
        else
            snprintf(g_teb.session_save_path, sizeof g_teb.session_save_path,
                     "./tphp_sess");
    }
    if (g_teb.session_lifetime <= 0)
        g_teb.session_lifetime = 1209600;      /* 14 天 */
    if (g_teb.session_gc_interval <= 0)
        g_teb.session_gc_interval = 300;
    g_teb.session_last_gc = 0;                 /* 每次 start 后允许立即 GC */

    /* assoc 数组展平成 key1,val1,...,NULL 的 const char**；
     * upload_* 、session_* 、callback_threads 为 teb 自留键——识别后摘除
     * 不透传（透传 civetweb 会 unknown option 启动失败） */
    TArr *arr = (TArr *)args[0].u.p;
    tarr_sync_buckets(arr);
    int64_t n_buckets = arr->bcap;
    /* +4：缺省追加 enable_keep_alive 用（批1 keep-alive，见循环后） */
    const char **opts = tm_malloc(sizeof(char *) * (size_t)(n_buckets * 2 + 4));
    int k = 0;
    bool has_keep_alive = false;
    bool has_cb_threads = false;
    for (int64_t bi = 0; bi < n_buckets; bi++) {
        ABucket *bk = &arr->buckets[bi];
        if (bk->key.t == T_NULL) continue;
        if (bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;        /* civetweb 配置键必须是字符串 */

        TStr *ks = (TStr *)bk->key.u.p;
        const char *kc = tstr_val(ks);
        TVal val = arr->data[bk->val_off];

        if (strcmp(kc, "enable_keep_alive") == 0) has_keep_alive = true;
        if (strcmp(kc, "callback_threads") == 0) {
            /* 执行池线程数（自留键）：缺省 min(4, CPU核数)；0 = 关池逃生舱 */
            has_cb_threads = true;
            TStr *vs;
            TVal tmp = TV_NULL;
            if (val.t == T_STR) vs = (TStr *)val.u.p;
            else { vs = tval_to_str(val); tmp = TV_STR(vs); }
            int64_t nt = strtoll(tstr_val(vs), NULL, 10);
            g_teb.callback_threads = nt < 0 ? 0 : (nt > 16 ? 16 : nt);
            tval_release(&tmp);
            continue;
        }

        if (strcmp(kc, "upload_tmp_dir") == 0) {
            TStr *vs;
            TVal tmp = TV_NULL;
            if (val.t == T_STR) vs = (TStr *)val.u.p;
            else { vs = tval_to_str(val); tmp = TV_STR(vs); }
            snprintf(g_teb.upload_tmp_dir, sizeof g_teb.upload_tmp_dir,
                     "%s", tstr_val(vs));
            tval_release(&tmp);
            continue;
        }
        if (strcmp(kc, "upload_max_bytes") == 0) {
            TStr *vs;
            TVal tmp = TV_NULL;
            if (val.t == T_STR) vs = (TStr *)val.u.p;
            else { vs = tval_to_str(val); tmp = TV_STR(vs); }
            int64_t mb = strtoll(tstr_val(vs), NULL, 10);
            g_teb.upload_max_bytes = mb > 0 ? mb : 64LL * 1024 * 1024;
            tval_release(&tmp);
            continue;
        }
        if (strcmp(kc, "session_save_path") == 0) {
            TStr *vs;
            TVal tmp = TV_NULL;
            if (val.t == T_STR) vs = (TStr *)val.u.p;
            else { vs = tval_to_str(val); tmp = TV_STR(vs); }
            snprintf(g_teb.session_save_path, sizeof g_teb.session_save_path,
                     "%s", tstr_val(vs));
            tval_release(&tmp);
            continue;
        }
        if (strcmp(kc, "session_lifetime") == 0 ||
            strcmp(kc, "session_gc_interval") == 0) {
            TStr *vs;
            TVal tmp = TV_NULL;
            if (val.t == T_STR) vs = (TStr *)val.u.p;
            else { vs = tval_to_str(val); tmp = TV_STR(vs); }
            int64_t iv = strtoll(tstr_val(vs), NULL, 10);
            if (strcmp(kc, "session_lifetime") == 0)
                g_teb.session_lifetime = iv;   /* <=0 由缺省块回落 */
            else
                g_teb.session_gc_interval = iv;
            tval_release(&tmp);
            continue;
        }

        char *kcopy = teb_dup(kc);
        opts[k++] = kcopy;

        TStr *vs;
        TVal tmp = TV_NULL;
        if (val.t == T_STR) {
            vs = (TStr *)val.u.p;
        } else {
            vs = tval_to_str(val);
            tmp = TV_STR(vs);
        }
        opts[k++] = tstr_val(vs);   /* civetweb 内部拷贝 */
        tval_release(&tmp);
    }
    /* keep-alive 缺省开启（用户显式传值优先）：civetweb 配置表默认 no，
     * 不改 vendor——在 teb 侧透传注入，升级 civetweb 不丢。 */
    if (!has_keep_alive) {
        opts[k++] = teb_dup("enable_keep_alive");
        opts[k++] = "yes";
    }
    opts[k] = NULL;

    /* 执行池准备（mg_start 前：NotCloneable 等 raise 在服务未起时干净中止）：
     * callback_threads 缺省 min(4,核数) → 字节码共享表 + 用户常量快照 +
     * 既有配置 blob 回填 → pools_armed。失败路径保持 pools_armed=false。 */
    if (!has_cb_threads) {
        int ncpu = 0;
        uv_cpu_info_t *ci = NULL;
        if (uv_cpu_info(&ci, &ncpu) == 0 && ncpu > 0) uv_free_cpu_info(ci, ncpu);
        int def = (ncpu > 0 && ncpu < 4) ? ncpu : 4;
        g_teb.callback_threads = def < 1 ? 1 : def;
    }
    g_teb.pools_armed = false;
    if (g_teb.callback_threads > 0) {
        VM *mvm = (VM *)tphp_vm_current;
        if (mvm && mvm->prog) {
            int cap = 1 + mvm->n_inc_progs;
            g_teb.share = tm_malloc(sizeof(Program *) * (size_t)cap);
            g_teb.n_share = 0;
            g_teb.share[g_teb.n_share++] = mvm->prog;
            for (int i = 0; i < mvm->n_inc_progs; i++) {
                if (mvm->inc_progs[i] && mvm->inc_progs[i] != mvm->prog)
                    g_teb.share[g_teb.n_share++] = mvm->inc_progs[i];
            }
            g_teb.const_blob = teb_const_snapshot(&g_teb.const_blob_len);
            /* 批B：autoload 回调快照（未注册/不可序列化 → NULL+warn，
             * 池线程无 autoloader = 类 miss 直接 Error——见 API.md 边界） */
            g_teb.autoload_blob =
                teb_autoload_snapshot(&g_teb.autoload_blob_len);
            /* 既有配置回填（start 前注册的路由/before/nf/ws） */
            teb_backfill_blobs();
            g_teb.pools_armed = true;
        } else {
            g_teb.callback_threads = 0;
        }
    }

    g_teb.ctx = mg_start(NULL, NULL, opts);

    for (int i = 0; i < k; i += 2) tm_free((void *)opts[i]);
    tm_free(opts);

    if (!g_teb.ctx)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Web::start(): mg_start() failed");

    /* 统一 thunk 挂所有 URI：命中路由/nf 入队阻塞，全无命中交 civetweb 默认 */
    mg_set_request_handler(g_teb.ctx, "/*", teb_req_thunk, NULL);

    /* P1-4 批量挂 WS handler（运行中 websocket() 注册者已即时挂） */
    for (TebWsReg *rg = g_teb.ws_regs; rg; rg = rg->next) {
        mg_set_websocket_handler(g_teb.ctx, rg->uri, ws_connect_ok,
                                 ws_ready_cb, ws_data_cb, ws_close_cb, rg);
    }

    teb_spawn_pools();   /* 执行池：handler 挂妥后创建（批3） */

    o->props[0] = TV_INT((int64_t)(uintptr_t)g_teb.ctx);
    /* 登记运行中实例（real_stop 释放引用并清 props[0]）。
     * 引擎 bug #2 根因（2026-11-14）：此处原 addref 一次两用（全局存储份
     * + 返回值转移份）——g_teb.web_obj 是 bitcopy 不持有，real_stop 的
     * release 实际吃掉 $web/返回值 dst 的合法份额 → main frame 返回值
     * TMP 槽变幽灵 → 自然退出二次 free（EXIT=139/SIGSEGV）。修复 = 全局
     * 份与返回值转移份各自独立 addref（引擎约定：native 返回值自带一
     * 份转移，见引擎 doc/ext-dev.md 所有权节）。 */
    g_teb.web_obj = TV_OBJ(o);
    tval_addref(TV_OBJ(o));   /* g_teb 持有自己的份（real_stop 释放） */
    tval_addref(TV_OBJ(o));   /* 返回值转移份（调用方 dst 接管） */
    return TV_OBJ(o);
}

/* drain(): void —— 单次泵（run 内部也用它） */
static TVal nm_web_drain(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    if (g_teb.ctx) teb_drain_once(vm);
    teb_poll_stop_request();
    return TV_NULL;
}

/* run(): void —— 阻塞主循环；stop() 或析构置 ctx=0 时退出 */
static TVal nm_web_run(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    while (g_teb.ctx) {
        teb_drain_once(vm);
        teb_poll_stop_request();   /* 非主线程 stop() → 本线程执行真停 */
    }
    return TV_NULL;
}

/* stop(): void —— 幂等；draining 中只挂 pending_stop（回调内 stop 合法），
 * 批末由 drain 执行 teb_real_stop（doc/API.md Web::stop）。
 * 非主线程（执行池回调）调用：只置 stop_requested + 唤醒，由主线程
 * run/drain 轮询后执行真停（join 自己会死锁，故只发信号）。 */
static TVal nm_web_stop(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (!g_teb.ctx) return TV_NULL;
    uv_thread_t self_tid = uv_thread_self();
    if (!uv_thread_equal(&self_tid, &g_teb.main_tid)) {
        teb_lock();
        g_teb.stop_requested = true;
        uv_cond_broadcast(&g_teb.cv);
        uv_mutex_unlock(&g_teb.mu);
        return TV_NULL;
    }
    if (g_teb.draining) {
        g_teb.pending_stop = true;
        return TV_NULL;
    }
    if (o->props[0].t != T_INT ||
        o->props[0].u.i != (int64_t)(uintptr_t)g_teb.ctx)
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Web::stop(): this instance is not the "
                      "running server");
    teb_real_stop();
    return TV_NULL;
}

/* P1-2 before(callable $cb): self —— 全局中间件（按注册序执行；单条可重复注册） */
static TVal nm_web_before(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::before() expects a callable");
    TVal cb = args[0];
    switch (cb.t) {
    case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
    default:
        tphp_vm_raise(vm, "TypeError",
                      "Web::before(): callable expected as argument 1");
    }
    TebRoute *b = tm_malloc(sizeof *b);
    b->uri = NULL;
    b->callable = cb;
    tval_addref(cb);
    b->next = NULL;
    b->cb_blob = NULL;
    b->cb_blob_len = 0;
    b->methods = 0;
    b->segs = NULL;
    b->n_seg = 0;
    if (g_teb.pools_armed)
        teb_snapshot_cb(cb, &b->cb_blob, &b->cb_blob_len);
    teb_lock();
    /* 尾插：按注册序执行 */
    if (!g_teb.befores) {
        g_teb.befores = b;
    } else {
        TebRoute *t = g_teb.befores;
        while (t->next) t = t->next;
        t->next = b;
    }
    g_teb.config_gen++;
    uv_mutex_unlock(&g_teb.mu);
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* __clonestate(): array —— 跨线程克隆协议：路由闭包 use($web) 捕获时经
 * serialize 进池线程，携带 ctx 句柄；恢复侧缺省按名回填 props，stop() 的
 * ctx 身份判定照常成立。无 ctx（未 start）→ 空状态。 */
static TVal nm_web_clonestate(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    TArr *a = tarr_new();
    if (o->props[0].t == T_INT) {
        TVal k = TV_STR(tstr_intern("ctx", 3));
        TVal v = TV_INT(o->props[0].u.i);
        tarr_set(a, k, v);
        tval_release(&k);
    }
    return TV_ARR(a);
}

/* P1-1 stats(): array —— 运行统计；任意时刻可调（未启动全 0）。
 * counters 可被 thunk 线程（413）并发更新 → 读取全程加锁。 */
static TVal nm_web_stats(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)self; (void)args; (void)n;
    TArr *a = tarr_new();
    teb_lock();
    TebStats *st = &g_teb.st;
    int64_t now = (int64_t)time(NULL);
    struct KV { const char *k; int64_t v; };
    struct KV kvs[] = {
        { "started_at", st->started_at },
        { "uptime", st->started_at ? now - st->started_at : 0 },
        { "requests_total", st->requests_total },
        { "requests_active", st->requests_active },
        { "bytes_sent", st->bytes_sent },
        { "ws_active", st->ws_active },
        { "ws_frames", st->ws_frames },
        { "ws_closed", st->ws_closed },
    };
    for (size_t i = 0; i < sizeof kvs / sizeof kvs[0]; i++) {
        TVal k = TV_STR(tstr_intern(kvs[i].k, strlen(kvs[i].k)));
        TVal v = TV_INT(kvs[i].v);
        tarr_set(a, k, v);
    }
    /* responses 子数组（按实际写出状态码；键 = 十进制码字符串） */
    TArr *rs = tarr_new();
    for (int i = 0; i < st->resp_n; i++) {
        char code[8];
        snprintf(code, sizeof code, "%lld", (long long)st->resp_tab[i].code);
        TVal k2 = TV_STR(tstr_intern(code, strlen(code)));
        TVal v2 = TV_INT(st->resp_tab[i].n);
        tarr_set(rs, k2, v2);
    }
    TVal rk = TV_STR(tstr_intern("responses", 9));
    TVal rv = TV_ARR(rs);   /* rs rc=1 转移 */
    tarr_set(a, rk, rv);
    uv_mutex_unlock(&g_teb.mu);
    return TV_ARR(a);       /* a rc=1 转移 */
}

/* P1-3 file(string $uriPrefix, string $dir): self —— 静态目录映射（多条） */
static TVal nm_web_file(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *prefix = arg_str(vm, args, n, 0, "Web::file");
    const char *dir = arg_str(vm, args, n, 1, "Web::file");
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::file() expects (string $uriPrefix, string $dir)");
    TebStatic *ts = tm_malloc(sizeof *ts);
    ts->prefix = teb_dup(prefix);
    ts->dir = teb_dup(dir);
    teb_lock();
    ts->next = g_teb.statics;   /* 头插：thunk 匹配读链表须同锁 */
    g_teb.statics = ts;
    g_teb.config_gen++;
    uv_mutex_unlock(&g_teb.mu);
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* P1-4 websocket(string $uri, callable $onOpen, ?callable $onMessage = null,
 *               ?callable $onClose = null): self */
static TVal nm_web_websocket(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::websocket() expects (string $uri, callable $onOpen"
                      ", ?callable $onMessage = null, ?callable $onClose = null)");
    const char *uri = arg_str(vm, args, n, 0, "Web::websocket");
    switch (args[1].t) {
    case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
    default:
        tphp_vm_raise(vm, "TypeError",
                      "Web::websocket(): onOpen must be a callable");
    }
    for (int i = 2; i < n; i++) {
        if (args[i].t == T_NULL) continue;
        switch (args[i].t) {
        case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
        default:
            tphp_vm_raise(vm, "TypeError",
                          "Web::websocket(): callbacks must be callable or null");
        }
    }
    TebWsReg *rg = tm_malloc(sizeof *rg);
    rg->uri = teb_dup(uri);
    rg->on_open = args[1];
    tval_addref(rg->on_open);
    rg->on_msg = (n >= 3 && args[2].t != T_NULL) ? args[2] : TV_NULL;
    if (rg->on_msg.t != T_NULL) tval_addref(rg->on_msg);
    rg->on_close = (n >= 4 && args[3].t != T_NULL) ? args[3] : TV_NULL;
    if (rg->on_close.t != T_NULL) tval_addref(rg->on_close);
    memset(rg->cb_blob, 0, sizeof rg->cb_blob);
    memset(rg->cb_blob_len, 0, sizeof rg->cb_blob_len);
    if (g_teb.pools_armed) {   /* 三回调各自快照（可能 raise——锁外） */
        teb_snapshot_cb(rg->on_open, &rg->cb_blob[0], &rg->cb_blob_len[0]);
        if (rg->on_msg.t != T_NULL)
            teb_snapshot_cb(rg->on_msg, &rg->cb_blob[1], &rg->cb_blob_len[1]);
        if (rg->on_close.t != T_NULL)
            teb_snapshot_cb(rg->on_close, &rg->cb_blob[2], &rg->cb_blob_len[2]);
    }
    teb_lock();
    rg->next = g_teb.ws_regs;
    g_teb.ws_regs = rg;
    g_teb.config_gen++;
    bool running = (g_teb.ctx != NULL);
    uv_mutex_unlock(&g_teb.mu);
    /* 运行中注册：立即挂（start 前注册者由 start 批量挂） */
    if (running) {
        mg_set_websocket_handler(g_teb.ctx, rg->uri, ws_connect_ok,
                                 ws_ready_cb, ws_data_cb, ws_close_cb, rg);
    }
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* ---- Tphp\Teb\Request 原生方法（信息读 props[1] 缓存） ---- */

/* 用户不得手动构造（drain 内部经 tphp_obj_alloc 绕过） */
static TVal nm_req_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\Request cannot be constructed directly "
                  "(instances are provided to route callbacks)");
}

/* 取 info 缓存键值（借用引用） */
static TVal teb_info_get(TObj *o, const char *key) {
    if (o->props[1].t != T_ARR) return TV_NULL;
    TArr *a = (TArr *)o->props[1].u.p;
    TVal *v = tarr_get(a, TV_STR(tstr_intern(key, strlen(key))));
    return v ? *v : TV_NULL;
}

/* 取 info 键值并补 +1 引用（native 方法返回协议） */
static TVal teb_info_ret(TObj *o, const char *key) {
    TVal v = teb_info_get(o, key);
    tval_addref(v);
    return v;
}

static TVal nm_req_method(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "method");
}
static TVal nm_req_uri(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "uri");
}
static TVal nm_req_path(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "path");
}
static TVal nm_req_query(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_info_ret((TObj *)self, "query");
    return v.t == T_NULL ? TV_STR(tstr_new("", 0)) : v;
}
static TVal nm_req_remote_addr(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "remote_addr");
}
static TVal nm_req_http_version(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "http_version");
}
static TVal nm_req_remote_port(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_info_get((TObj *)self, "remote_port");
    return v.t == T_INT ? TV_INT(v.u.i) : TV_INT(0);
}
static TVal nm_req_is_ssl(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_info_get((TObj *)self, "is_ssl");
    return (v.t == T_INT && v.u.i) ? TV_TRUE : TV_FALSE;
}

/* header(string $name): ?string —— 头名大小写不敏感 */
static TVal nm_req_header(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    const char *name = arg_str(vm, args, n, 0, "Request::header");
    TVal v = teb_header_ci((TObj *)self, name);
    if (v.t == T_NULL) return TV_NULL;
    tval_addref(v);
    return v;
}

/* headers(): array */
static TVal nm_req_headers(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "headers");
}

/* body(int $maxLen = 65536): string —— 读 body 缓存（首次读满全部），按 maxLen 截断 */
static TVal nm_req_body(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    TStr *full = teb_body_cached(vm, o);
    int64_t max_len = (n >= 1 && args[0].t == T_INT) ? args[0].u.i : 65536;
    if (max_len <= 0) max_len = 65536;
    if (full->len <= (size_t)max_len) {
        TVal v = o->props[5];   /* 全量：共享缓存串（借用 +1） */
        tval_addref(v);
        return v;
    }
    return TV_STR(tstr_new(full->c, (size_t)max_len));
}

/* ---- 解析助手（doc/API.md Request 节）----
 * Request props: 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
 *                5=body_cache 6=qp_cache 7=cookie_cache 8=session
 * TV_NULL = 未解析哨兵（解析后必存 T_ARR / T_STR）。 */

/* queryParams(): array —— query string urlencoded 解析（缓存 props[6]） */
static TVal nm_req_query_params(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[6].t != T_ARR) {
        TVal qv = teb_info_get(o, "query");
        const char *q = (qv.t == T_STR) ? tstr_val((TStr *)qv.u.p) : "";
        teb_set_slot(o, 6, TV_ARR(teb_parse_urlencoded(q, strlen(q))));
    }
    TVal v = o->props[6];
    tval_addref(v);
    return v;
}

/* queryParam(string $k, $default = null) */
static TVal nm_req_query_param(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    const char *k = arg_str(vm, args, n, 0, "Request::queryParam");
    TObj *o = (TObj *)self;
    if (o->props[6].t != T_ARR) {
        TVal qv = teb_info_get(o, "query");
        const char *q = (qv.t == T_STR) ? tstr_val((TStr *)qv.u.p) : "";
        teb_set_slot(o, 6, TV_ARR(teb_parse_urlencoded(q, strlen(q))));
    }
    TVal *v = tarr_get((TArr *)o->props[6].u.p,
                       TV_STR(tstr_intern(k, strlen(k))));
    if (v && v->t != T_UNDEF && v->t != T_NULL) {
        tval_addref(*v);
        return *v;
    }
    if (n >= 2) {
        TVal d = args[1];
        tval_addref(d);
        return d;
    }
    return TV_NULL;
}

/* routeParams(): array —— 路径模板参数（批A；无参数/非模板路由 → []） */
static TVal nm_req_route_params(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    TVal pv = teb_info_get(o, "params");
    if (pv.t == T_ARR) {
        tval_addref(pv);
        return pv;
    }
    return TV_ARR(tarr_new());
}

/* routeParam(string $k, $default = null) —— 对称 queryParam 命名族 */
static TVal nm_req_route_param(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    const char *k = arg_str(vm, args, n, 0, "Request::routeParam");
    TObj *o = (TObj *)self;
    TVal pv = teb_info_get(o, "params");
    if (pv.t == T_ARR) {
        TVal *v = tarr_get((TArr *)pv.u.p, TV_STR(tstr_intern(k, strlen(k))));
        if (v && v->t != T_UNDEF && v->t != T_NULL) {
            tval_addref(*v);
            return *v;
        }
    }
    if (n >= 2) {
        TVal d = args[1];
        tval_addref(d);
        return d;
    }
    return TV_NULL;
}

/* Cookie 头解析："a=1; b=2" → {a:"1",b:"2"}（'+'→空格，与 PHP urldecode 一致） */
static TArr *teb_parse_cookies(TObj *o) {
    TArr *a = tarr_new();
    TVal cv = teb_header_ci(o, "Cookie");
    if (cv.t != T_STR) return a;
    TStr *cs = (TStr *)cv.u.p;
    const char *p = tstr_val(cs), *end = p + cs->len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        const char *semi = memchr(p, ';', (size_t)(end - p));
        const char *seg_end = semi ? semi : end;
        const char *eq = memchr(p, '=', (size_t)(seg_end - p));
        const char *k = p;
        size_t kl = eq ? (size_t)(eq - p) : (size_t)(seg_end - p);
        const char *v = eq ? eq + 1 : seg_end;
        size_t vl = eq ? (size_t)(seg_end - (eq + 1)) : 0;
        char kb[1024], vb[4096];
        int kln = mg_url_decode(k, (int)(kl > INT_MAX ? INT_MAX : kl),
                                kb, (int)sizeof(kb), 1);
        int vln = mg_url_decode(v, (int)(vl > INT_MAX ? INT_MAX : vl),
                                vb, (int)sizeof(vb), 1);
        if (kln > 0) {
            TVal key = TV_STR(tstr_new(kb, (size_t)kln));
            TVal val = TV_STR(tstr_new(vln >= 0 ? vb : "",
                                       vln >= 0 ? (size_t)vln : 0));
            tarr_set(a, key, val);
            tval_release(&key);
        }
        p = semi ? semi + 1 : end;
    }
    return a;
}

/* cookies(): array —— 解析 headers.Cookie（缓存 props[7]） */
static TVal nm_req_cookies(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[7].t != T_ARR) teb_set_slot(o, 7, TV_ARR(teb_parse_cookies(o)));
    TVal v = o->props[7];
    tval_addref(v);
    return v;
}

/* cookie(string $k, $default = null) */
static TVal nm_req_cookie(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    const char *k = arg_str(vm, args, n, 0, "Request::cookie");
    TObj *o = (TObj *)self;
    if (o->props[7].t != T_ARR) teb_set_slot(o, 7, TV_ARR(teb_parse_cookies(o)));
    TVal *v = tarr_get((TArr *)o->props[7].u.p,
                       TV_STR(tstr_intern(k, strlen(k))));
    if (v && v->t != T_UNDEF && v->t != T_NULL) {
        tval_addref(*v);
        return *v;
    }
    if (n >= 2) {
        TVal d = args[1];
        tval_addref(d);
        return d;
    }
    return TV_NULL;
}

/* json($default = null)：body 缓存 + 引擎 C 层 decode；坏 JSON / 空 body → default */
static TVal nm_req_json(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    TStr *b = teb_body_cached(vm, o);
    TVal out = TV_NULL;
    if (b->len > 0 && tphp_json_decode_c(tstr_val(b), b->len, &out))
        return out;   /* decode 产物 rc=1 归调用方 */
    if (n >= 1) {
        TVal d = args[0];
        tval_addref(d);
        return d;
    }
    return TV_NULL;
}

/* contentType(): ?string */
static TVal nm_req_content_type(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_header_ci((TObj *)self, "Content-Type");
    if (v.t != T_STR) return TV_NULL;
    tval_addref(v);
    return v;
}

/* isJson(): bool —— Content-Type（ci）包含 "json" */
static TVal nm_req_is_json(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_header_ci((TObj *)self, "Content-Type");
    if (v.t != T_STR) return TV_FALSE;
    return str_icontains(tstr_val((TStr *)v.u.p), "json") ? TV_TRUE : TV_FALSE;
}

/* clientIp(): string —— X-Forwarded-For 首段（trim），否则 remote_addr */
static TVal nm_req_client_ip(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    TVal xf = teb_header_ci(o, "X-Forwarded-For");
    if (xf.t == T_STR) {
        TStr *s = (TStr *)xf.u.p;
        const char *c = tstr_val(s);
        const char *comma = memchr(c, ',', s->len);
        size_t n1 = comma ? (size_t)(comma - c) : s->len;
        size_t a = 0;
        while (a < n1 && (c[a] == ' ' || c[a] == '\t')) a++;
        size_t b = n1;
        while (b > a && (c[b - 1] == ' ' || c[b - 1] == '\t')) b--;
        if (b > a) return TV_STR(tstr_new(c + a, b - a));
    }
    return teb_info_ret(o, "remote_addr");
}

/* contentLength(): int —— info.content_length，负值钳 0 */
static TVal nm_req_content_length(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TVal v = teb_info_get((TObj *)self, "content_length");
    int64_t x = (v.t == T_INT) ? v.u.i : 0;
    return TV_INT(x > 0 ? x : 0);
}

/* beginStream(int $status = 200, array $headers = []): Stream —— 幂等
 * Stream 挂 props[4]；重复调用返回同一实例 */
static TVal nm_req_begin_stream(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    if (o->props[4].t == T_OBJ) {
        tval_addref(o->props[4]);
        return o->props[4];
    }
    int64_t status = (n >= 1) ? tval_to_int(args[0]) : 200;
    TObj *s = tphp_obj_alloc(g_stream_cls);
    s->props[0] = o->props[0];   /* conn 句柄（int64 值拷贝） */
    s->props[1] = TV_INT(status);
    if (n >= 2 && args[1].t == T_ARR) {
        tval_addref(args[1]);    /* 用户 headers 共享 COW */
        s->props[2] = args[1];
    } else {
        s->props[2] = TV_ARR(tarr_new());
    }
    s->props[3] = TV_FALSE;
    s->props[4] = TV_FALSE;
    /* 反向同步：会话已开（先 session 后 beginStream）→ 挂 Stream 槽
     * （发头时注入 Set-Cookie 用；正向分支见 teb_session_open） */
    if (o->props[8].t == T_OBJ) {
        s->props[5] = o->props[8];
        tval_addref(s->props[5]);
    }
    teb_set_slot(o, 4, TV_OBJ(s));   /* Request 持 +1（回调结束随 Request 释放） */
    tval_addref(TV_OBJ(s));          /* 返回 +1 */
    return TV_OBJ(s);
}

/* ---- 表单/上传（doc/API.md UploadedFile 节）----
 * form(): GET/HEAD → query 解析；multipart → mg_handle_form_request（耗尽
 * body，勿先调 body()）；其余 → body 缓存 urlencoded 解析。结果缓存
 * props[2]（文本字段）与 props[3]（文件，非 multipart 为空数组）。 */

static TVal nm_req_form(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[2].t == T_INT)   /* multipart 解析失败/超限：重复抛同一 Error */
        tphp_vm_raise(vm, "Error",
                      o->props[2].u.i == 1
                          ? "Tphp\\Teb\\Request::form(): form data exceeds"
                            " upload_max_bytes (%lld bytes)"
                          : "Tphp\\Teb\\Request::form(): multipart form"
                            " parse failed",
                      (long long)g_teb.upload_max_bytes);
    if (o->props[2].t == T_ARR) {
        tval_addref(o->props[2]);
        return o->props[2];
    }
    TVal mv = teb_info_get(o, "method");
    const char *method = (mv.t == T_STR) ? tstr_val((TStr *)mv.u.p) : "";
    TVal qv = teb_info_get(o, "query");
    const char *query = (qv.t == T_STR) ? tstr_val((TStr *)qv.u.p) : "";
    bool is_get = (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0);
    TVal ct = teb_header_ci(o, "Content-Type");
    const char *ctype = (ct.t == T_STR) ? tstr_val((TStr *)ct.u.p) : "";

    if (is_get) {
        teb_set_slot(o, 2, TV_ARR(teb_parse_urlencoded(query, strlen(query))));
        teb_set_slot(o, 3, TV_ARR(tarr_new()));
    } else if (str_icontains(ctype, "multipart/form-data") &&
               o->props[0].t == T_INT) {
        struct mg_connection *conn =
            (struct mg_connection *)(uintptr_t)o->props[0].u.i;
        teb_form_parse_multipart(vm, o, conn);   /* 失败抛 Error；成功缓存两槽 */
    } else {
        TStr *b = teb_body_cached(vm, o);
        teb_set_slot(o, 2, TV_ARR(teb_parse_urlencoded(tstr_val(b), b->len)));
        teb_set_slot(o, 3, TV_ARR(tarr_new()));
    }
    tval_addref(o->props[2]);
    return o->props[2];
}

/* files(): array —— name => UploadedFile（非 multipart 为空数组） */
static TVal nm_req_files(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (o->props[3].t != T_ARR) nm_req_form(vm, self, args, n);
    TVal v = o->props[3];
    tval_addref(v);
    return v;
}

/* file(string $name): ?UploadedFile */
static TVal nm_req_file(VM *vm, void *self, TVal *args, int n) {
    const char *name = arg_str(vm, args, n, 0, "Request::file");
    TObj *o = (TObj *)self;
    if (o->props[3].t != T_ARR) nm_req_form(vm, self, NULL, 0);
    TVal *v = tarr_get((TArr *)o->props[3].u.p,
                       TV_STR(tstr_intern(name, strlen(name))));
    if (v && v->t == T_OBJ) {
        tval_addref(*v);
        return *v;
    }
    return TV_NULL;
}

/* ---- Tphp\Teb\Response 原生方法 ----
 * props: 0=status(int) 1=body(str) 2=headers(arr) 3=cookies(arr) */

/* 数组槽写入：key/val 为**借用**——内部 addref（tarr_set 不 addref，见
 * vm.c op_assign 约定），数组持独立引用；COW 分离防改共享数组。
 * 调用方对自建的 key/val 负责 release（interned 键 no-op）。 */
static void teb_arr_set_borrowed(TObj *o, int slot, TVal key, TVal val) {
    TArr *a = (o->props[slot].t == T_ARR) ? (TArr *)o->props[slot].u.p : NULL;
    if (!a) { a = tarr_new(); o->props[slot] = TV_ARR(a); }
    TArr *c = tarr_cow(a);
    if (c != a) {
        TVal old = o->props[slot];
        tval_release(&old);
        o->props[slot] = TV_ARR(c);
    }
    tval_addref(key);
    tval_addref(val);
    tarr_set(c, key, val);
}

/* __construct(string $body = "", int $status = 200,
 *             array $headers = ["Content-Type" => "text/plain"]) */
static TVal nm_resp_construct(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;

    /* body */
    TVal body;
    if (n >= 1 && args[0].t == T_STR) {
        body = args[0];
        tval_addref(body);
    } else if (n >= 1) {
        body = TV_STR(tval_to_str(args[0]));   /* +1 */
    } else {
        body = TV_STR(tstr_new("", 0));
    }
    o->props[1] = body;

    /* status */
    int64_t status = 200;
    if (n >= 2) status = tval_to_int(args[1]);
    o->props[0] = TV_INT(status);

    /* headers */
    if (n >= 3 && args[2].t == T_ARR) {
        TVal h = args[2];
        tval_addref(h);
        o->props[2] = h;
    } else {
        TArr *h = tarr_new();
        tarr_set(h, TV_STR(tstr_intern("Content-Type", 12)),
                    TV_STR(tstr_new("text/plain", 10)));
        o->props[2] = TV_ARR(h);
    }

    /* cookies */
    o->props[3] = TV_ARR(tarr_new());
    (void)vm;
    return TV_NULL;
}

/* header(string $k, string $v): self */
static TVal nm_resp_header(VM *vm, void *self, TVal *args, int n) {
    const char *k = arg_str(vm, args, n, 0, "Response::header");
    const char *v = arg_str(vm, args, n, 1, "Response::header");
    TObj *o = (TObj *)self;
    TVal key = TV_STR(tstr_intern(k, strlen(k)));   /* interned：无需释放 */
    TVal val = TV_STR(tstr_new(v, strlen(v)));
    teb_arr_set_borrowed(o, 2, key, val);
    tval_release(&val);   /* 数组已持独立引用 */
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* IMF-fixdate（"Wed, 21 Oct 2015 07:28:00 GMT"）—— 纯整数 civil-from-days
 * 反推，勿用 gmtime（静态缓冲与 civetweb worker 线程有数据竞态）。 */
static void teb_http_date(int64_t ts, char *buf, size_t cap) {
    static const char *WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *MO[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    int64_t days = ts / 86400, rem = ts % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    int64_t dow = (days + 4) % 7;              /* 1970-01-01 = 周四 = 4 */
    if (dow < 0) dow += 7;
    snprintf(buf, cap, "%s, %02lld %s %04lld %02lld:%02lld:%02lld GMT",
             WD[dow], (long long)d, MO[m - 1], (long long)y,
             (long long)(rem / 3600), (long long)(rem / 60 % 60),
             (long long)(rem % 60));
}

/* cookie(string $k, string $v, array $opts = []): self
 * opts 键大小写不敏感：path/domain(string)、expires/max_age(int)、
 * secure/httponly(bool)、sameSite|same_site("Lax"|"Strict"|"None")。
 * 未知键 → ValueError（拼错静默丢安全标志比报错危险）；类型错 → TypeError；
 * $v/path/domain 含 CRLF → ValueError（响应分割注入防线）。
 * 先校验零分配，全过后再拼后缀——tphp_vm_raise 是 longjmp。 */
static TVal nm_resp_cookie(VM *vm, void *self, TVal *args, int n) {
    const char *k = arg_str(vm, args, n, 0, "Response::cookie");
    const char *v = arg_str(vm, args, n, 1, "Response::cookie");
    const char *c_path = NULL, *c_domain = NULL, *c_ss = NULL;
    int64_t c_exp = 0, c_ma = 0;
    bool c_secure = false, c_http = false;
    bool has_exp = false, has_ma = false;

    if (strchr(v, '\r') || strchr(v, '\n'))
        tphp_vm_raise(vm, "ValueError",
                      "Response::cookie(): value must not contain CR/LF");

    if (n >= 3 && args[2].t != T_ARR)
        tphp_vm_raise(vm, "TypeError",
                      "Response::cookie() expects an array of options");

    TArr *opts = (n >= 3) ? (TArr *)args[2].u.p : NULL;
    if (opts) {
        tarr_sync_buckets(opts);
        for (uint32_t b = 0; b < opts->bcap; b++) {
            ABucket *bk = &opts->buckets[b];
            if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
            if (bk->key.t != T_STR)
                tphp_vm_raise(vm, "ValueError",
                              "Response::cookie(): option keys must be strings");
            TStr *kstr = (TStr *)bk->key.u.p;
            const char *kk = kstr->c;
            TVal v = opts->data[bk->val_off];
            if (str_icmp(kk, "path")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): path must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (strchr(s, '\r') || strchr(s, '\n'))
                    tphp_vm_raise(vm, "ValueError", "Response::cookie(): path must not contain CR/LF");
                c_path = s;
            } else if (str_icmp(kk, "domain")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): domain must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (strchr(s, '\r') || strchr(s, '\n'))
                    tphp_vm_raise(vm, "ValueError", "Response::cookie(): domain must not contain CR/LF");
                c_domain = s;
            } else if (str_icmp(kk, "expires")) {
                if (v.t != T_INT)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): expires must be an int");
                c_exp = v.u.i; has_exp = true;
            } else if (str_icmp(kk, "max_age")) {
                if (v.t != T_INT)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): max_age must be an int");
                c_ma = v.u.i; has_ma = true;
            } else if (str_icmp(kk, "secure")) {
                if (v.t != T_TRUE && v.t != T_FALSE)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): secure must be a bool");
                c_secure = (v.t == T_TRUE);
            } else if (str_icmp(kk, "httponly")) {
                if (v.t != T_TRUE && v.t != T_FALSE)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): httponly must be a bool");
                c_http = (v.t == T_TRUE);
            } else if (str_icmp(kk, "sameSite") || str_icmp(kk, "same_site")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): sameSite must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (!str_icmp(s, "Lax") && !str_icmp(s, "Strict") && !str_icmp(s, "None"))
                    tphp_vm_raise(vm, "ValueError",
                                  "Response::cookie(): sameSite must be Lax, Strict or None");
                c_ss = s;
            } else {
                tphp_vm_raise(vm, "ValueError",
                              "Response::cookie(): unknown option \"%.*s\"",
                              (int)kstr->len, kk);
            }
        }
    }

    /* 校验全过 → 分配后缀（此后无 raise 点，顺序拼接固定） */
    size_t cap = strlen(v) + 8;
    if (c_path)   cap += strlen(c_path) + 10;
    if (c_domain) cap += strlen(c_domain) + 12;
    if (has_exp)  cap += 64;
    if (has_ma)   cap += 32;
    if (c_secure) cap += 10;
    if (c_http)   cap += 12;
    if (c_ss)     cap += strlen(c_ss) + 14;
    char *buf = tm_malloc(cap);
    size_t off = (size_t)snprintf(buf, cap, "%s", v);
#define TEB_CK_FMT(...)  do { \
        if ((size_t)off < cap) \
            off += (size_t)snprintf(buf + off, cap - off, __VA_ARGS__); \
    } while (0)
    if (c_path)   TEB_CK_FMT("; Path=%s", c_path);
    if (c_domain) TEB_CK_FMT("; Domain=%s", c_domain);
    if (has_exp) {
        char ed[64];
        teb_http_date(c_exp, ed, sizeof ed);
        TEB_CK_FMT("; Expires=%s", ed);
    }
    if (has_ma)   TEB_CK_FMT("; Max-Age=%lld", (long long)c_ma);
    if (c_secure) TEB_CK_FMT("; Secure");
    if (c_http)   TEB_CK_FMT("; HttpOnly");
    if (c_ss)     TEB_CK_FMT("; SameSite=%s", c_ss);
#undef TEB_CK_FMT

    TObj *o = (TObj *)self;
    TVal key = TV_STR(tstr_intern(k, strlen(k)));
    TVal val = TV_STR(tstr_new(buf, off));
    tm_free(buf);
    teb_arr_set_borrowed(o, 3, key, val);
    tval_release(&val);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* withHeaders(array $headers): self —— 桶值借用，addref 在 set 内 */
static TVal nm_resp_with_headers(VM *vm, void *self, TVal *args, int n) {
    if (n < 1 || args[0].t != T_ARR)
        tphp_vm_raise(vm, "TypeError", "Response::withHeaders() expects an array");
    TObj *o = (TObj *)self;
    TArr *in = (TArr *)args[0].u.p;
    tarr_sync_buckets(in);
    for (uint32_t b = 0; b < in->bcap; b++) {
        ABucket *bk = &in->buckets[b];
        if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;
        teb_arr_set_borrowed(o, 2, bk->key, in->data[bk->val_off]);
    }
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* status(int $code): self */
static TVal nm_resp_status(VM *vm, void *self, TVal *args, int n) {
    int64_t code = arg_int(vm, args, n, 0, "Response::status");
    TObj *o = (TObj *)self;
    o->props[0] = TV_INT(code);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* body(): string */
static TVal nm_resp_body(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[1].t != T_STR) return TV_STR(tstr_new("", 0));
    tval_addref(o->props[1]);
    return o->props[1];
}

/* ---- Response 静态工厂（doc/API.md Response 静态工厂；native static，self=NULL） ---- */

static bool resp_headers_arg(TVal *args, int n, int i, TArr **out) {
    if (n > i) {
        if (args[i].t != T_ARR) return false;
        tval_addref(args[i]);      /* 共享 COW：调用方后续修改自动分离 */
        *out = (TArr *)args[i].u.p;
    } else {
        *out = tarr_new();
    }
    return true;
}

/* Response::json($data, int $status = 200, array $headers = []): Response */
static TVal nm_resp_json(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Response::json() expects at least 1 argument");
    TStr *enc = NULL;
    if (!tphp_json_encode_c(args[0], &enc))
        tphp_vm_raise(vm, "Error",
                      "Response::json(): value cannot be encoded to JSON");
    int64_t status = (n >= 2) ? tval_to_int(args[1]) : 200;
    TArr *h = NULL;
    if (!resp_headers_arg(args, n, 2, &h)) {
        tval_release(&(TVal){ .u.p = enc, .t = T_STR });
        tphp_vm_raise(vm, "TypeError", "Response::json() expects an array of headers");
    }
    TObj *r = teb_resp_build(enc, status, h, "application/json", NULL);
    return TV_OBJ(r);
}

/* Response::text(string $s, int $status = 200, array $headers = []): Response */
static TVal nm_resp_text(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Response::text() expects at least 1 argument");
    TVal b;
    if (args[0].t == T_STR) {
        b = args[0];
        tval_addref(b);
    } else {
        b = TV_STR(tval_to_str(args[0]));   /* +1 */
    }
    int64_t status = (n >= 2) ? tval_to_int(args[1]) : 200;
    TArr *h = NULL;
    if (!resp_headers_arg(args, n, 2, &h)) {
        tval_release(&b);
        tphp_vm_raise(vm, "TypeError", "Response::text() expects an array of headers");
    }
    TObj *r = teb_resp_build((TStr *)b.u.p, status, h, "text/plain", NULL);
    return TV_OBJ(r);
}

/* Response::html(string $s, int $status = 200, array $headers = []): Response */
static TVal nm_resp_html(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Response::html() expects at least 1 argument");
    TVal b;
    if (args[0].t == T_STR) {
        b = args[0];
        tval_addref(b);
    } else {
        b = TV_STR(tval_to_str(args[0]));   /* +1 */
    }
    int64_t status = (n >= 2) ? tval_to_int(args[1]) : 200;
    TArr *h = NULL;
    if (!resp_headers_arg(args, n, 2, &h)) {
        tval_release(&b);
        tphp_vm_raise(vm, "TypeError", "Response::html() expects an array of headers");
    }
    TObj *r = teb_resp_build((TStr *)b.u.p, status, h, "text/html", NULL);
    return TV_OBJ(r);
}

/* Response::redirect(string $url, int $status = 302, array $headers = []): Response
 * 空 body、无默认 ctype、Location 最后强制写入（覆盖用户同名项） */
static TVal nm_resp_redirect(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *url = arg_str(vm, args, n, 0, "Response::redirect");
    char *urlcopy = teb_dup(url);   /* arg_str 指向 VM 串，raise 前先自持 */
    int64_t status = (n >= 2) ? tval_to_int(args[1]) : 302;
    TArr *h = NULL;
    if (!resp_headers_arg(args, n, 2, &h)) {
        tm_free(urlcopy);
        tphp_vm_raise(vm, "TypeError", "Response::redirect() expects an array of headers");
    }
    TObj *r = teb_resp_build(tstr_new("", 0), status, h, NULL, urlcopy);
    tm_free(urlcopy);
    return TV_OBJ(r);
}

/* P1-3 同步读整个文件（uv_fs；UTF-8 路径安全）。
 * max_sz = 读取字节上限（Response::file 传 upload_max_bytes、session 传
 * TEB_SESS_MAX_BYTES——耦合 g_teb 会让小上限配置打挂会话读取）。
 * 返回 0 成功（out/outlen 归调用方 tm_free）；1 不存在/不可读；2 超上限。 */
static int teb_read_file_max(const char *path, int64_t max_sz,
                             char **out, size_t *outlen) {
    uv_fs_t r;
    if (uv_fs_open(NULL, &r, path, UV_FS_O_RDONLY, 0, NULL) < 0) {
        uv_fs_req_cleanup(&r);
        return 1;
    }
    uv_file fd = (uv_file)r.result;
    uv_fs_req_cleanup(&r);
    if (uv_fs_fstat(NULL, &r, fd, NULL) < 0) {
        uv_fs_req_cleanup(&r);
        uv_fs_close(NULL, &r, fd, NULL);
        uv_fs_req_cleanup(&r);
        return 1;
    }
    int64_t sz = (int64_t)r.statbuf.st_size;
    uv_fs_req_cleanup(&r);
    if (sz > max_sz) {
        uv_fs_close(NULL, &r, fd, NULL);
        uv_fs_req_cleanup(&r);
        return 2;
    }
    char *buf = tm_malloc((size_t)sz + 1);
    int64_t off = 0;
    while (off < sz) {
        uv_buf_t iov = uv_buf_init(buf + off, (unsigned)(sz - off));
        int64_t nread = uv_fs_read(NULL, &r, fd, &iov, 1, off, NULL);
        if (nread < 0) {
            uv_fs_req_cleanup(&r);
            uv_fs_close(NULL, &r, fd, NULL);
            uv_fs_req_cleanup(&r);
            tm_free(buf);
            return 1;
        }
        uv_fs_req_cleanup(&r);
        if (nread == 0) break;   /* EOF（文件被并发截断等） */
        off += nread;
    }
    uv_fs_close(NULL, &r, fd, NULL);
    uv_fs_req_cleanup(&r);
    buf[sz] = 0;
    *out = buf;
    *outlen = (size_t)sz;
    return 0;
}

/* Response::file(string $path, int $status = 200, array $headers = []): Response
 * 读整个文件入 body（超 upload_max_bytes / 不存在 / 不可读 → Error）；
 * Content-Type 按扩展名推断（用户 headers 可覆盖） */
static TVal nm_resp_file(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *path = arg_str(vm, args, n, 0, "Response::file");
    char *pathcopy = teb_dup(path);
    int64_t status = (n >= 2) ? tval_to_int(args[1]) : 200;
    TArr *h = NULL;
    if (!resp_headers_arg(args, n, 2, &h)) {
        tm_free(pathcopy);
        tphp_vm_raise(vm, "TypeError", "Response::file() expects an array of headers");
    }
    char *data = NULL;
    size_t dlen = 0;
    int rc = teb_read_file_max(pathcopy, g_teb.upload_max_bytes, &data, &dlen);
    if (rc == 2) {
        tm_free(pathcopy);
        tphp_vm_raise(vm, "Error",
                      "Response::file(): file exceeds upload_max_bytes");
    }
    if (rc != 0) {
        tm_free(pathcopy);
        tphp_vm_raise(vm, "Error", "Response::file(): cannot read file");
    }
    const char *mime = mg_get_builtin_mime_type(pathcopy);
    TObj *r = teb_resp_build(tstr_new(data, dlen), status, h,
                             mime ? mime : "application/octet-stream", NULL);
    tm_free(data);
    tm_free(pathcopy);
    return TV_OBJ(r);
}

/* ---- Tphp\Teb\Session 原生方法（doc/API.md Session 节）----
 * Session props: 0=id(str,64hex) 1=data(arr) 2=destroyed(bool)
 * 配置（save_path/lifetime/gc）读 g_teb 不进对象 → 与 Request/Stream
 * 无回指引用环。会话文件读写全部发生在 drain 主线程（与回调串行），无锁。 */

/* 入站 sid 防线：恰好 64 位 hex 才接受（防路径拼接与伪造） */
static bool teb_sid_valid(const char *s, size_t len) {
    if (len != TEB_SESS_SID_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

/* CSPRNG sid（uv_random 32B → 小写 hex）。失败返回 false——
 * 调用方抛 Error，绝不回退弱熵（可预测 sid = 会话劫持，比 500 更糟）。 */
static bool teb_sid_random(char out[TEB_SESS_SID_LEN + 1]) {
    unsigned char raw[32];
    if (uv_random(NULL, NULL, raw, sizeof raw, 0, NULL) != 0) return false;
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hx[raw[i] >> 4];
        out[i * 2 + 1] = hx[raw[i] & 0xF];
    }
    out[TEB_SESS_SID_LEN] = 0;
    return true;
}

static bool teb_sess_ensure_dir(void) {
    uv_fs_t r;
    int rc = uv_fs_mkdir(NULL, &r, g_teb.session_save_path, 0, NULL);
    uv_fs_req_cleanup(&r);
    if (rc == 0 || rc == UV_EEXIST) return true;
    return false;
}

/* <save_path>/<sid> → buf（截断 → 返回 false，调用方按 Error 处理） */
static bool teb_sess_path(char *buf, size_t cap, const char *sid) {
    int n = snprintf(buf, cap, "%s/%s", g_teb.session_save_path, sid);
    return n > 0 && (size_t)n < cap;
}

static int64_t teb_fs_mtime(const char *path) {
    uv_fs_t r;
    if (uv_fs_stat(NULL, &r, path, NULL) < 0) {
        uv_fs_req_cleanup(&r);
        return -1;
    }
    int64_t mt = (int64_t)r.statbuf.st_mtim.tv_sec;
    uv_fs_req_cleanup(&r);
    return mt;
}

/* 原子写：<sid>.tmp → unlink 目标 → rename（Windows rename 不覆盖既有）。
 * 失败清 tmp 返回 false（调用方仅 warn——flush 在响应写出之后，无 raise 通路）。 */
static bool teb_sess_write(const char *path, const char *d, size_t n) {
    char tmp[TEB_SESS_PATH_CAP];
    int ln = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (ln <= 0 || (size_t)ln >= sizeof tmp) return false;
    uv_fs_t r;
    uv_file fd = uv_fs_open(NULL, &r, tmp,
                            UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
                            0600, NULL);
    uv_fs_req_cleanup(&r);
    if (fd < 0) return false;
    size_t off = 0;
    bool ok = true;
    while (off < n) {
        uv_buf_t iov = uv_buf_init((char *)d + off, (unsigned)(n - off));
        int64_t w = uv_fs_write(NULL, &r, fd, &iov, 1, (int64_t)off, NULL);
        uv_fs_req_cleanup(&r);
        if (w <= 0) { ok = false; break; }
        off += (size_t)w;
    }
    uv_fs_close(NULL, &r, fd, NULL);
    uv_fs_req_cleanup(&r);
    if (!ok) { teb_fs_unlink(tmp); return false; }
    teb_fs_unlink(path);              /* 不存在则失败，忽略 */
    if (!teb_fs_rename(tmp, path)) { teb_fs_unlink(tmp); return false; }
    return true;
}

/* 目录级 GC（节流：now - last_gc >= session_gc_interval 才扫）。按 mtime
 * 清过期会话文件与残留 .tmp/孤儿——不按名字，崩溃残留同规则回收。
 * 惰性单文件过期由 teb_session_open 的 mtime 检查负责，本函数只是回收。 */
static void teb_sess_gc(void) {
    int64_t now = (int64_t)time(NULL);
    teb_lock();                     /* 节流状态与注册/stats 同锁 */
    if (g_teb.session_last_gc != 0 &&
        now - g_teb.session_last_gc < g_teb.session_gc_interval) {
        uv_mutex_unlock(&g_teb.mu);
        return;
    }
    g_teb.session_last_gc = now;
    uv_mutex_unlock(&g_teb.mu);
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, g_teb.session_save_path, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_dirent_t ent;
    char path[TEB_SESS_PATH_CAP];
    while (uv_fs_scandir_next(&req, &ent) != UV_EOF) {
        if (ent.type != UV_DIRENT_FILE && ent.type != UV_DIRENT_UNKNOWN)
            continue;
        if (!teb_sess_path(path, sizeof path, ent.name)) continue;
        int64_t mt = teb_fs_mtime(path);
        if (mt >= 0 && now - mt > g_teb.session_lifetime)
            teb_fs_unlink(path);
    }
    uv_fs_req_cleanup(&req);
}

/* 取 Request 缓存的会话对象（未打开过 → NULL）——响应写出点前置声明 */
static TObj *teb_sess_of(TObj *req) {
    if (!req) return NULL;
    return (req->props[8].t == T_OBJ) ? (TObj *)req->props[8].u.p : NULL;
}

/* Set-Cookie 整行（含 \r\n，直接写出）：destroyed → 清除形态（Max-Age=0）。
 * tm_malloc，调用方 tm_free；无会话返回 NULL。 */
static char *teb_sess_cookie_line(TObj *sess) {
    if (!sess) return NULL;
    char buf[256];
    if (sess->props[2].t == T_TRUE) {
        snprintf(buf, sizeof buf,
                 "Set-Cookie: " TEB_SESS_COOKIE
                 "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n");
    } else {
        TStr *id = (sess->props[0].t == T_STR) ? (TStr *)sess->props[0].u.p : NULL;
        if (!id || id->len != TEB_SESS_SID_LEN) return NULL;
        snprintf(buf, sizeof buf,
                 "Set-Cookie: " TEB_SESS_COOKIE "=%.*s; Path=/; HttpOnly;"
                 " SameSite=Lax; Max-Age=%lld\r\n",
                 (int)id->len, id->c, (long long)g_teb.session_lifetime);
    }
    return teb_dup(buf);
}

/* 响应写出后统一落盘（消费者挂点调用）：滑动续期 = 每次打开都重写
 * （mtime 刷新与 Set-Cookie 滑动配对）。严禁 raise/调 VM——此时响应已发出。
 * 末尾统一释放 per-sid 条带锁（props[3]/props[4] 记录加锁时的 sid——
 * 会话中途 regenerateId()/destroy+set() 会换 id，解锁必须用原加锁 sid）。 */
static void teb_session_flush(TObj *req) {
    TObj *sess = teb_sess_of(req);
    if (!sess) return;
    if (sess->props[2].t != T_TRUE && sess->props[1].t == T_ARR) {
        TStr *id = (sess->props[0].t == T_STR) ? (TStr *)sess->props[0].u.p : NULL;
        if (id) {
            TStr *enc = NULL;
            if (!tphp_json_encode_c(sess->props[1], &enc) || !enc) {
                tphp_warn("PHP Warning: Teb session data is not JSON-encodable; "
                          "session not persisted for this request\n");
            } else {
                char path[TEB_SESS_PATH_CAP];
                if (!teb_sess_path(path, sizeof path, tstr_val(id))) {
                    tphp_warn("PHP Warning: Teb session_save_path too long; "
                              "session not persisted\n");
                } else if (!teb_sess_write(path, tstr_val(enc), enc->len)) {
                    tphp_warn("PHP Warning: Teb session write failed (%s)\n", path);
                }
                tval_release(&(TVal){ .t = T_STR, .u.p = enc });
            }
        }
    }
    if (sess->props[3].t == T_TRUE &&
        sess->props[4].t == T_STR) {
        TStr *lk = (TStr *)sess->props[4].u.p;
        teb_sess_unlock(tstr_val(lk), lk->len);
        teb_set_slot(sess, 4, TV_NULL);
        sess->props[3] = TV_FALSE;
    }
}

/* 惰性打开会话（$r->session()；同请求幂等）。读入站 cookie → 校验 →
 * per-sid 条带锁 → mtime 过期判定（惰性单文件过期）→ 读文件 JSON 解码 →
 * GC → 建对象。加锁顺序：全部 raise 点（目录/路径/CSPRNG）在加锁前完成，
 * 锁一旦持有直到 flush 释放（早释放 = 丢更新窗口）。 */
static TObj *teb_session_open(VM *vm, TObj *req) {
    if (req->props[8].t == T_OBJ) {
        tval_addref(req->props[8]);
        return (TObj *)req->props[8].u.p;
    }
    if (!teb_sess_ensure_dir())
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Request::session(): session_save_path is not"
                      " usable (parent directory must exist; check"
                      " \"session_save_path\" in Web::start())");

    /* 入站 sid（复用 props[7] cookie 缓存；大小写不敏感接受后统一小写） */
    if (req->props[7].t != T_ARR)
        teb_set_slot(req, 7, TV_ARR(teb_parse_cookies(req)));
    char sid[TEB_SESS_SID_LEN + 1] = {0};
    bool have_sid = false;
    {
        TArr *cks = (TArr *)req->props[7].u.p;
        TVal *cv = tarr_get(cks, TV_STR(tstr_intern(TEB_SESS_COOKIE,
                                                    strlen(TEB_SESS_COOKIE))));
        if (cv && cv->t == T_STR) {
            TStr *s = (TStr *)cv->u.p;
            if (teb_sid_valid(tstr_val(s), s->len)) {
                memcpy(sid, tstr_val(s), TEB_SESS_SID_LEN);
                for (int i = 0; i < TEB_SESS_SID_LEN; i++)
                    if (sid[i] >= 'A' && sid[i] <= 'F') sid[i] += 32;
                have_sid = true;
            }
        }
    }

    /* 路径构造（raise 点，锁前完成） */
    char path[TEB_SESS_PATH_CAP] = {0};
    if (have_sid && !teb_sess_path(path, sizeof path, sid))
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Request::session(): session_save_path"
                      " too long");

    /* 读会话数据（持锁读，防与并发 flush 撕更新）：mtime 过期则解锁作废
     * 换新；读失败/解码失败保留 sid 空数据（自愈，避免 cookie 抖动）。 */
    TArr *data = tarr_new();
    if (have_sid) {
        teb_sess_lock(sid, TEB_SESS_SID_LEN);
        int64_t mt = teb_fs_mtime(path);
        if (mt >= 0 && (int64_t)time(NULL) - mt > g_teb.session_lifetime) {
            teb_fs_unlink(path);
            teb_sess_unlock(sid, TEB_SESS_SID_LEN);   /* 过期作废 → 走下方换新 */
            have_sid = false;
        } else if (mt >= 0) {
            char *raw = NULL; size_t rlen = 0;
            if (teb_read_file_max(path, TEB_SESS_MAX_BYTES, &raw, &rlen) == 0) {
                TVal dec = TV_NULL;
                if (rlen > 0 && tphp_json_decode_c(raw, rlen, &dec) &&
                    dec.t == T_ARR) {
                    tval_release(&(TVal){ .t = T_ARR, .u.p = data });
                    data = (TArr *)dec.u.p;   /* decode +1 归本处 */
                }
                tm_free(raw);
            }
        }
        /* mt<0（文件不存在）：持锁继续，flush 写出新文件 */
    }
    if (!have_sid) {
        if (!teb_sid_random(sid)) {                 /* raise 点：此时无锁 */
            tval_release(&(TVal){ .t = T_ARR, .u.p = data });
            tphp_vm_raise(vm, "Error",
                          "Tphp\\Teb\\Request::session(): CSPRNG unavailable"
                          " (uv_random failed)");
        }
        teb_sess_lock(sid, TEB_SESS_SID_LEN);
    }

    teb_sess_gc();                          /* 节流门内，非每请求全扫（内部 mu） */

    TObj *s = tphp_obj_alloc(g_sess_cls);
    s->props[0] = TV_STR(tstr_new(sid, TEB_SESS_SID_LEN));
    s->props[1] = TV_ARR(data);
    s->props[2] = TV_FALSE;
    s->props[3] = TV_TRUE;                  /* lock_held：flush 释放 */
    s->props[4] = TV_STR(tstr_new(sid, TEB_SESS_SID_LEN));   /* lock_sid 原件 */

    teb_set_slot(req, 8, TV_OBJ(s));        /* Request 持 +1（转移） */
    tval_addref(TV_OBJ(s));                 /* 返回 +1 */
    /* 正向同步：Stream 已在（先 beginStream 后 session）→ 挂 Stream 槽 */
    TVal st = req->props[4];
    if (st.t == T_OBJ && teb_is_cls((TObj *)st.u.p, g_stream_cls,
                                    "Tphp\\Teb\\Stream")) {
        TObj *so = (TObj *)st.u.p;
        so->props[5] = TV_OBJ(s);
        tval_addref(so->props[5]);
    }
    return s;
}

/* session(): Session —— 惰性打开；同请求幂等缓存（Request props[8]）。
 * 未调用不产生任何 cookie 与文件 I/O。 */
static TVal nm_req_session(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    return TV_OBJ(teb_session_open(vm, (TObj *)self));
}

/* __construct —— 禁手动 new */
static TVal nm_sess_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\Session cannot be constructed directly "
                  "(use Request::session())");
}

/* id(): string */
static TVal nm_sess_id(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[0].t != T_STR) return TV_STR(tstr_new("", 0));
    tval_addref(o->props[0]);
    return o->props[0];
}

/* 查找键（get/has/remove 共用）：tarr_* 内部 tval_norm_key 自动把
 * 数字字符串归一为 int 键，与 set 侧一致。 */
static TVal sess_key(VM *vm, TVal *args, int n, const char *who) {
    const char *k = arg_str(vm, args, n, 0, who);
    return TV_STR(tstr_intern(k, strlen(k)));   /* interned：release no-op */
}

/* get(string $k, $default = null) */
static TVal nm_sess_get(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    TVal key = sess_key(vm, args, n, "Session::get");
    if (o->props[1].t == T_ARR) {
        TVal *v = tarr_get((TArr *)o->props[1].u.p, key);
        if (v && v->t != T_UNDEF) {
            tval_addref(*v);
            return *v;
        }
    }
    if (n >= 2) {
        TVal d = args[1];
        tval_addref(d);
        return d;
    }
    return TV_NULL;
}

/* set(string $k, $v): self —— 顶层值白名单（null/bool/int/float/string/
 * array）；destroyed 后 set → 换新 sid 复活（防会话固定）。 */
static TVal nm_sess_set(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    TVal key = sess_key(vm, args, n, "Session::set");
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Session::set() expects at least 2 arguments");
    TVal val = args[1];
    if (val.t != T_NULL && val.t != T_FALSE && val.t != T_TRUE &&
        val.t != T_INT && val.t != T_FLOAT && val.t != T_STR &&
        val.t != T_ARR)
        tphp_vm_raise(vm, "TypeError",
                      "Session::set(): value must be null/bool/int/float/"
                      "string/array (objects and closures cannot be"
                      " session-encoded)");
    if (o->props[2].t == T_TRUE) {          /* destroyed → 复活换新 sid */
        char sid[TEB_SESS_SID_LEN + 1];
        if (!teb_sid_random(sid))
            tphp_vm_raise(vm, "Error",
                          "Tphp\\Teb\\Session::set(): CSPRNG unavailable");
        TVal ns = TV_STR(tstr_new(sid, TEB_SESS_SID_LEN));
        teb_set_slot(o, 0, ns);
        o->props[2] = TV_FALSE;
    }
    if (o->props[1].t != T_ARR)
        teb_set_slot(o, 1, TV_ARR(tarr_new()));
    TArr *data = (TArr *)o->props[1].u.p;   /* 恒 rc=1：all() 返回副本 */
    tval_addref(val);                       /* tarr_set 转移 val 所有权 */
    tarr_set(data, key, val);
    tval_release(&key);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* has(string $k): bool */
static TVal nm_sess_has(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    TVal key = sess_key(vm, args, n, "Session::has");
    bool r = (o->props[1].t == T_ARR) &&
             tarr_has((TArr *)o->props[1].u.p, key);
    tval_release(&key);
    return r ? TV_TRUE : TV_FALSE;
}

/* remove(string $k): self */
static TVal nm_sess_remove(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    TVal key = sess_key(vm, args, n, "Session::remove");
    if (o->props[1].t == T_ARR)
        tarr_unset((TArr *)o->props[1].u.p, key);
    tval_release(&key);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* all(): array —— 新建副本（data 恒 rc=1，set/remove 免 COW 纪律） */
static TVal nm_sess_all(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    TArr *out = tarr_new();
    if (o->props[1].t == T_ARR) {
        TArr *src = (TArr *)o->props[1].u.p;
        tarr_sync_buckets(src);
        for (uint32_t b = 0; b < src->bcap; b++) {
            ABucket *bk = &src->buckets[b];
            if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
            TVal k = bk->key, v = src->data[bk->val_off];
            tval_addref(k);
            tval_addref(v);
            tarr_set(out, k, v);         /* 复制成对自持引用 */
            tval_release(&k);
            tval_release(&v);
        }
    }
    return TV_ARR(out);
}

/* destroy(): void —— 删文件 + 清数据 + 置 destroyed（响应注入清除 cookie） */
static TVal nm_sess_destroy(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    TStr *id = (o->props[0].t == T_STR) ? (TStr *)o->props[0].u.p : NULL;
    if (id && o->props[2].t != T_TRUE) {
        char path[TEB_SESS_PATH_CAP];
        if (teb_sess_path(path, sizeof path, tstr_val(id)))
            teb_fs_unlink(path);
    }
    if (o->props[1].t != T_ARR)
        teb_set_slot(o, 1, TV_ARR(tarr_new()));
    else {
        TArr *empty = tarr_new();
        teb_set_slot(o, 1, TV_ARR(empty));
    }
    o->props[2] = TV_TRUE;
    return TV_NULL;
}

/* regenerateId(): self —— 删旧文件 + 新 sid，数据留内存（flush 写新文件） */
static TVal nm_sess_regenerate(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[2].t != T_TRUE) {
        TStr *id = (o->props[0].t == T_STR) ? (TStr *)o->props[0].u.p : NULL;
        if (id) {
            char path[TEB_SESS_PATH_CAP];
            if (teb_sess_path(path, sizeof path, tstr_val(id)))
                teb_fs_unlink(path);
        }
    }
    char sid[TEB_SESS_SID_LEN + 1];
    if (!teb_sid_random(sid))
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Session::regenerateId(): CSPRNG unavailable");
    teb_set_slot(o, 0, TV_STR(tstr_new(sid, TEB_SESS_SID_LEN)));
    o->props[2] = TV_FALSE;
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* ---- Tphp\Teb\Stream 原生方法（doc/API.md Stream 节）----
 * props: 0=conn 1=status 2=headers 3=header_sent 4=ended 5=session */

static TVal nm_stream_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\Stream cannot be constructed directly "
                  "(use Request::beginStream())");
}

/* status(int $code): self —— 头未发可改；已发/已结束 → Error */
static TVal nm_stream_status(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (o->props[3].t == T_TRUE || o->props[4].t == T_TRUE)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Stream::status(): headers already sent");
    int64_t code = arg_int(vm, args, n, 0, "Stream::status");
    o->props[1] = TV_INT(code);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* header(string $k, string $v): self —— 同上 */
static TVal nm_stream_header(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (o->props[3].t == T_TRUE || o->props[4].t == T_TRUE)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Stream::header(): headers already sent");
    const char *k = arg_str(vm, args, n, 0, "Stream::header");
    const char *v = arg_str(vm, args, n, 1, "Stream::header");
    TVal key = TV_STR(tstr_intern(k, strlen(k)));
    TVal val = TV_STR(tstr_new(v, strlen(v)));
    teb_arr_set_borrowed(o, 2, key, val);
    tval_release(&val);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* write(string $chunk): int —— 惰性发头后写出，返回字节数 */
static TVal nm_stream_write(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (o->props[4].t == T_TRUE)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Stream::write(): stream already ended");
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "Stream::write() expects a string");
    TStr *cs = (TStr *)args[0].u.p;
    struct mg_connection *conn = (o->props[0].t == T_INT)
        ? (struct mg_connection *)(uintptr_t)o->props[0].u.i : NULL;
    if (!conn)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Stream::write(): connection unavailable");
    teb_stream_send_headers(o);
    int w = (int)teb_wr(conn, cs->c, (unsigned long long)cs->len);
    return TV_INT(w);
}

/* end(string $last = ""): void —— 幂等；可携带最后一块 */
static TVal nm_stream_end(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    if (o->props[4].t == T_TRUE) return TV_NULL;   /* 幂等 */
    struct mg_connection *conn = (o->props[0].t == T_INT)
        ? (struct mg_connection *)(uintptr_t)o->props[0].u.i : NULL;
    if (conn) {
        teb_stream_send_headers(o);
        if (n >= 1 && args[0].t == T_STR) {
            TStr *last = (TStr *)args[0].u.p;
            if (last->len)
                teb_wr(conn, last->c, (unsigned long long)last->len);
        }
    }
    o->props[4] = TV_TRUE;
    return TV_NULL;
}

/* ---- Tphp\Teb\UploadedFile 原生方法（doc/API.md UploadedFile 节）----
 * props: 0=name 1=type 2=tmp 3=size 4=error 5=moved */

static TVal nm_upfile_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\UploadedFile cannot be constructed directly "
                  "(instances are provided by Request::form()/files())");
}

static TVal nm_upfile_name(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[0].t != T_STR) return TV_STR(tstr_new("", 0));
    tval_addref(o->props[0]);
    return o->props[0];
}

static TVal nm_upfile_type(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[1].t != T_STR) return TV_STR(tstr_new("", 0));
    tval_addref(o->props[1]);
    return o->props[1];
}

static TVal nm_upfile_tmp_name(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (o->props[2].t != T_STR) return TV_STR(tstr_new("", 0));
    tval_addref(o->props[2]);
    return o->props[2];
}

static TVal nm_upfile_size(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    return TV_INT((o->props[3].t == T_INT) ? o->props[3].u.i : 0);
}

static TVal nm_upfile_error(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    return TV_INT((o->props[4].t == T_INT) ? o->props[4].u.i : 0);
}

static TVal nm_upfile_is_moved(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    return (o->props[5].t == T_TRUE) ? TV_TRUE : TV_FALSE;
}

/* moveTo(string $dest): bool —— 已 moved / error≠0 / tmp 空 → false；
 * rename 失败回落 copyfile+unlink（全 libuv，UTF-8 安全） */
static TVal nm_upfile_move_to(VM *vm, void *self, TVal *args, int n) {
    const char *dest = arg_str(vm, args, n, 0, "UploadedFile::moveTo");
    char *destcopy = teb_dup(dest);
    TObj *o = (TObj *)self;
    bool ok = false;
    if (o->props[5].t != T_TRUE &&
        !(o->props[4].t == T_INT && o->props[4].u.i != 0) &&
        o->props[2].t == T_STR) {
        TStr *src = (TStr *)o->props[2].u.p;
        if (src->len > 0) {
            teb_fs_unlink(destcopy);   /* 尽力清目标（Windows rename 不覆盖） */
            ok = teb_fs_rename(tstr_val(src), destcopy);
            if (!ok) {
                ok = teb_fs_copyfile(tstr_val(src), destcopy);
                if (ok) teb_fs_unlink(tstr_val(src));
            }
            if (ok) {
                o->props[5] = TV_TRUE;
                teb_set_slot(o, 2, TV_STR(tstr_new("", 0)));
            }
        }
    }
    tm_free(destcopy);
    return ok ? TV_TRUE : TV_FALSE;
}

/* ---- Tphp\Teb\WebSocket 原生方法（P1-4）----
 * props: 0=conn 句柄（TV_NULL = 连接已关） 1=close_requested */

static TVal nm_ws_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\WebSocket cannot be constructed directly "
                  "(instances are provided to websocket() callbacks)");
}

/* 活动连接句柄（props[0]==0 → 抛 Error("connection closed")） */
static struct mg_connection *teb_ws_conn(TObj *o, VM *vm, const char *who) {
    if (o->props[0].t != T_INT)
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\WebSocket::%s(): connection closed", who);
    return (struct mg_connection *)(uintptr_t)o->props[0].u.i;
}

/* send(string $data, bool $binary = false): int —— mg_lock_connection 包住
 * 写出（防 civetweb 侧并发写），返回字节数 */
static TVal nm_ws_send(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    struct mg_connection *conn = teb_ws_conn(o, vm, "send");
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "WebSocket::send() expects a string");
    TStr *cs = (TStr *)args[0].u.p;
    bool binary = (n >= 2 && tval_to_bool(args[1]));
    int opcode = binary ? MG_WEBSOCKET_OPCODE_BINARY : MG_WEBSOCKET_OPCODE_TEXT;
    mg_lock_connection(conn);
    int w = mg_websocket_write(conn, opcode, cs->c, cs->len);
    mg_unlock_connection(conn);
    if (w < 0)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\WebSocket::send(): write failed");
    return TV_INT(w);
}

/* ping(string $data = ""): bool */
static TVal nm_ws_ping(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    struct mg_connection *conn = teb_ws_conn(o, vm, "ping");
    TStr *cs = (n >= 1 && args[0].t == T_STR)
        ? (TStr *)args[0].u.p : NULL;
    mg_lock_connection(conn);
    int w = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_PING,
                               cs ? cs->c : "", cs ? cs->len : 0);
    mg_unlock_connection(conn);
    return w >= 0 ? TV_TRUE : TV_FALSE;
}

/* close(string $data = ""): void —— 发 close 帧 + 置 close_requested
 * （本帧回调后 data 回调返回 0 断开）；幂等 */
static TVal nm_ws_close(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    if (o->props[0].t != T_INT) return TV_NULL;   /* 已关：幂等 */
    struct mg_connection *conn = (struct mg_connection *)(uintptr_t)o->props[0].u.i;
    TStr *cs = (n >= 1 && args[0].t == T_STR) ? (TStr *)args[0].u.p : NULL;
    mg_lock_connection(conn);
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,
                       cs ? cs->c : "", cs ? cs->len : 0);
    mg_unlock_connection(conn);
    teb_lock();               /* props[1] 与 data 回调线程的读同锁 */
    o->props[1] = TV_TRUE;    /* close_requested → data 回调返回 0 */
    uv_mutex_unlock(&g_teb.mu);
    return TV_NULL;
}

/* remoteAddr(): string —— 连接已关 → Error */
static TVal nm_ws_remote_addr(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    struct mg_connection *conn = teb_ws_conn(o, vm, "remoteAddr");
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *addr = (ri && ri->remote_addr[0]) ? ri->remote_addr : "";
    return TV_STR(tstr_new(addr, strlen(addr)));
}

/* ---- 类注册（init 阶段，首脚本编译前） ---- */

static void teb_register_classes(void) {
    /* Web：props[0] = ctx 句柄 */
    TClass *w = teb_cls_new("Tphp\\Teb\\Web");
    teb_cls_add_prop(w, "ctx");
    teb_cls_add_native(w, "route", (void *)nm_web_route, false);
    teb_cls_add_native(w, "get", (void *)nm_web_get, false);
    teb_cls_add_native(w, "post", (void *)nm_web_post, false);
    teb_cls_add_native(w, "put", (void *)nm_web_put, false);
    teb_cls_add_native(w, "patch", (void *)nm_web_patch, false);
    teb_cls_add_native(w, "delete", (void *)nm_web_delete, false);
    teb_cls_add_native(w, "head", (void *)nm_web_head, false);
    teb_cls_add_native(w, "options", (void *)nm_web_options, false);
    teb_cls_add_native(w, "notFound", (void *)nm_web_not_found, false);
    teb_cls_add_native(w, "before", (void *)nm_web_before, false);
    teb_cls_add_native(w, "file", (void *)nm_web_file, false);
    teb_cls_add_native(w, "websocket", (void *)nm_web_websocket, false);
    teb_cls_add_native(w, "stats", (void *)nm_web_stats, false);
    teb_cls_add_native(w, "start", (void *)nm_web_start, false);
    teb_cls_add_native(w, "drain", (void *)nm_web_drain, false);
    teb_cls_add_native(w, "run", (void *)nm_web_run, false);
    teb_cls_add_native(w, "stop", (void *)nm_web_stop, false);
    teb_cls_add_native(w, "__cloneState", (void *)nm_web_clonestate, false);
    tphp_class_register_compile(w);
    g_web_cls = w;

    /* Request：props 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
     *          5=body_cache 6=qp_cache 7=cookie_cache 8=session */
    TClass *rq = teb_cls_new("Tphp\\Teb\\Request");
    teb_cls_add_prop(rq, "conn");
    teb_cls_add_prop(rq, "info");
    teb_cls_add_prop(rq, "form_cache");
    teb_cls_add_prop(rq, "files_cache");
    teb_cls_add_prop(rq, "stream_obj");
    teb_cls_add_prop(rq, "body_cache");
    teb_cls_add_prop(rq, "qp_cache");
    teb_cls_add_prop(rq, "cookie_cache");
    teb_cls_add_prop(rq, "session");
    teb_cls_add_ctor(rq, (void *)nm_req_construct);
    teb_cls_add_native(rq, "method", (void *)nm_req_method, false);
    teb_cls_add_native(rq, "uri", (void *)nm_req_uri, false);
    teb_cls_add_native(rq, "path", (void *)nm_req_path, false);
    teb_cls_add_native(rq, "query", (void *)nm_req_query, false);
    teb_cls_add_native(rq, "header", (void *)nm_req_header, false);
    teb_cls_add_native(rq, "headers", (void *)nm_req_headers, false);
    teb_cls_add_native(rq, "body", (void *)nm_req_body, false);
    teb_cls_add_native(rq, "remoteAddr", (void *)nm_req_remote_addr, false);
    teb_cls_add_native(rq, "remotePort", (void *)nm_req_remote_port, false);
    teb_cls_add_native(rq, "isSsl", (void *)nm_req_is_ssl, false);
    teb_cls_add_native(rq, "httpVersion", (void *)nm_req_http_version, false);
    teb_cls_add_native(rq, "beginStream", (void *)nm_req_begin_stream, false);
    teb_cls_add_native(rq, "queryParams", (void *)nm_req_query_params, false);
    teb_cls_add_native(rq, "queryParam", (void *)nm_req_query_param, false);
    teb_cls_add_native(rq, "routeParams", (void *)nm_req_route_params, false);
    teb_cls_add_native(rq, "routeParam", (void *)nm_req_route_param, false);
    teb_cls_add_native(rq, "cookies", (void *)nm_req_cookies, false);
    teb_cls_add_native(rq, "cookie", (void *)nm_req_cookie, false);
    teb_cls_add_native(rq, "json", (void *)nm_req_json, false);
    teb_cls_add_native(rq, "contentType", (void *)nm_req_content_type, false);
    teb_cls_add_native(rq, "isJson", (void *)nm_req_is_json, false);
    teb_cls_add_native(rq, "clientIp", (void *)nm_req_client_ip, false);
    teb_cls_add_native(rq, "contentLength", (void *)nm_req_content_length, false);
    teb_cls_add_native(rq, "form", (void *)nm_req_form, false);
    teb_cls_add_native(rq, "file", (void *)nm_req_file, false);
    teb_cls_add_native(rq, "files", (void *)nm_req_files, false);
    teb_cls_add_native(rq, "session", (void *)nm_req_session, false);
    tphp_class_register_compile(rq);
    g_req_cls = rq;

    /* Response：props 0=status 1=body 2=headers 3=cookies */
    TClass *rp = teb_cls_new("Tphp\\Teb\\Response");
    teb_cls_add_prop(rp, "status");
    teb_cls_add_prop(rp, "body");
    teb_cls_add_prop(rp, "headers");
    teb_cls_add_prop(rp, "cookies");
    teb_cls_add_ctor(rp, (void *)nm_resp_construct);
    teb_cls_add_native(rp, "header", (void *)nm_resp_header, false);
    teb_cls_add_native(rp, "cookie", (void *)nm_resp_cookie, false);
    teb_cls_add_native(rp, "withHeaders", (void *)nm_resp_with_headers, false);
    teb_cls_add_native(rp, "status", (void *)nm_resp_status, false);
    teb_cls_add_native(rp, "body", (void *)nm_resp_body, false);
    teb_cls_add_native(rp, "json", (void *)nm_resp_json, true);
    teb_cls_add_native(rp, "text", (void *)nm_resp_text, true);
    teb_cls_add_native(rp, "html", (void *)nm_resp_html, true);
    teb_cls_add_native(rp, "redirect", (void *)nm_resp_redirect, true);
    teb_cls_add_native(rp, "file", (void *)nm_resp_file, true);
    tphp_class_register_compile(rp);
    g_resp_cls = rp;

    /* Stream：props 0=conn 1=status 2=headers 3=header_sent 4=ended 5=session */
    TClass *st = teb_cls_new("Tphp\\Teb\\Stream");
    teb_cls_add_prop(st, "conn");
    teb_cls_add_prop(st, "status");
    teb_cls_add_prop(st, "headers");
    teb_cls_add_prop(st, "header_sent");
    teb_cls_add_prop(st, "ended");
    teb_cls_add_prop(st, "session");
    teb_cls_add_ctor(st, (void *)nm_stream_construct);
    teb_cls_add_native(st, "status", (void *)nm_stream_status, false);
    teb_cls_add_native(st, "header", (void *)nm_stream_header, false);
    teb_cls_add_native(st, "write", (void *)nm_stream_write, false);
    teb_cls_add_native(st, "end", (void *)nm_stream_end, false);
    tphp_class_register_compile(st);
    g_stream_cls = st;

    /* UploadedFile：props 0=name 1=type 2=tmp 3=size 4=error 5=moved */
    TClass *uf = teb_cls_new("Tphp\\Teb\\UploadedFile");
    teb_cls_add_prop(uf, "name");
    teb_cls_add_prop(uf, "type");
    teb_cls_add_prop(uf, "tmp");
    teb_cls_add_prop(uf, "size");
    teb_cls_add_prop(uf, "error");
    teb_cls_add_prop(uf, "moved");
    teb_cls_add_ctor(uf, (void *)nm_upfile_construct);
    teb_cls_add_native(uf, "name", (void *)nm_upfile_name, false);
    teb_cls_add_native(uf, "type", (void *)nm_upfile_type, false);
    teb_cls_add_native(uf, "tmpName", (void *)nm_upfile_tmp_name, false);
    teb_cls_add_native(uf, "size", (void *)nm_upfile_size, false);
    teb_cls_add_native(uf, "error", (void *)nm_upfile_error, false);
    teb_cls_add_native(uf, "isMoved", (void *)nm_upfile_is_moved, false);
    teb_cls_add_native(uf, "moveTo", (void *)nm_upfile_move_to, false);
    tphp_class_register_compile(uf);
    g_upfile_cls = uf;

    /* WebSocket：props 0=conn（TV_NULL=已关） 1=close_requested */
    TClass *ws = teb_cls_new("Tphp\\Teb\\WebSocket");
    teb_cls_add_prop(ws, "conn");
    teb_cls_add_prop(ws, "close_requested");
    teb_cls_add_ctor(ws, (void *)nm_ws_construct);
    teb_cls_add_native(ws, "send", (void *)nm_ws_send, false);
    teb_cls_add_native(ws, "ping", (void *)nm_ws_ping, false);
    teb_cls_add_native(ws, "close", (void *)nm_ws_close, false);
    teb_cls_add_native(ws, "remoteAddr", (void *)nm_ws_remote_addr, false);
    tphp_class_register_compile(ws);
    g_ws_cls = ws;

    /* Session：props 0=id 1=data 2=destroyed 3=lock_held 4=lock_sid
     *（3/4 为 per-sid 条带锁内部槽，私有不可见；禁手动 new，见 nm_sess_construct） */
    TClass *ss = teb_cls_new("Tphp\\Teb\\Session");
    teb_cls_add_prop(ss, "id");
    teb_cls_add_prop(ss, "data");
    teb_cls_add_prop(ss, "destroyed");
    teb_cls_add_prop(ss, "lock_held");
    teb_cls_add_prop(ss, "lock_sid");
    teb_cls_add_ctor(ss, (void *)nm_sess_construct);
    teb_cls_add_native(ss, "id", (void *)nm_sess_id, false);
    teb_cls_add_native(ss, "get", (void *)nm_sess_get, false);
    teb_cls_add_native(ss, "set", (void *)nm_sess_set, false);
    teb_cls_add_native(ss, "has", (void *)nm_sess_has, false);
    teb_cls_add_native(ss, "remove", (void *)nm_sess_remove, false);
    teb_cls_add_native(ss, "all", (void *)nm_sess_all, false);
    teb_cls_add_native(ss, "destroy", (void *)nm_sess_destroy, false);
    teb_cls_add_native(ss, "regenerateId", (void *)nm_sess_regenerate, false);
    tphp_class_register_compile(ss);
    g_sess_cls = ss;

    /* Internal：泵类（pump Program 的 `while(\\Tphp\\Teb\\Internal\\pump())`
     * 调用目标；native static，self=NULL。仅此一个方法，无 ctor/props） */
    TClass *it = teb_cls_new("Tphp\\Teb\\Internal");
    teb_cls_add_native(it, "pump", (void *)nm_pool_pump, true);
    teb_cls_add_native(it, "inPoolThread", (void *)nm_internal_in_pool_thread, true);
    tphp_class_register_compile(it);
}

/* ---- init：被 ext registry 调用，首脚本编译前一次 ---- */

#define NS "Tphp\\Teb\\"

/* 注册符号：pkg.c ext_init_sym 约定 tphp_ext_<name>（/→_）
 * 对 tphp/teb → tphp_ext_tphp_teb_init */
void tphp_ext_tphp_teb_init(void) {
    tphp_ext_reg_const_str(NS "VERSION", "0.1.0");
    tphp_ext_reg_const_int(NS "FEATURES_DEFAULT", 0);
    tphp_ext_reg_const_int(NS "FEATURES_CGI", 4);
    teb_register_classes();
}
