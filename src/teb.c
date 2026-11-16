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
 *   Tphp\Teb\Stream       流式响应：beginStream() 产物，惰性发头、close 定界
 *   Tphp\Teb\UploadedFile 上传文件：form()/files() 产物，moveTo() 落盘
 *   Tphp\Teb\WebSocket    WS 连接（P1）：websocket() 回调入参，send/ping/close/remoteAddr
 *
 * WS 帧层（RFC6455 子集，drain 侧解码）：civetweb data 回调给已解 mask 的
 * 单帧载荷（FIN|opcode 在 bits）——控制帧不出回调：PING 自动回 PONG、PONG
 * 忽略、CLOSE 回 CLOSE 并断连；数据帧 FIN=0 分片按 conn 装配后交付
 * onMessage（bits=消息 opcode，1=文本 2=二进制），协议错/超限断连。
 *
 * 线程模型（同步阻塞，2026-11-13 冒烟验证）：
 *   civetweb worker 进入 thunk/WS 回调 → 入队 → 阻塞等 done；PHP 主线程
 *   Web::run() 内部 drain 循环取批 → 构造 Request → before 链 → 路由/静态/WS
 *   分发 → 写响应 → 唤醒 worker。回调期间 conn 归本请求独占。
 *   路由匹配：精确 → 前缀 → 静态文件（前缀+存在性，thunk 内判定）→ notFound
 *   → 最长路由兜底 → return 0 交 civetweb 默认处理。
 *   回调异常/非 Response → C 侧写 500（异常信息照常打印）；流式回调返回值
 *   一律忽略（收尾契约见 doc/API.md Stream 节）。
 *   stats 计数全部在 PHP 主线程写（drain/回调），thunk/WS 回调线程不写 → 无锁。
 */
#include "pkg.h"            /* tphp_ext_reg_const_str / tphp_ext_reg_fn / tphp_json_encode_c */
#include "value.h"          /* TVal / TV_* / tstr_* / tm_* */
#include "opcode.h"         /* VM / BuiltinFn */
#include "class.h"          /* TClass / TObj / tphp_obj_alloc / register_compile */
#include "tphp.h"           /* tphp_warn */

#include <uv.h>             /* uv_mutex_t / uv_cond_t / uv_fs_* / uv_os_* */

/* civetweb 头（teb 包内 civetweb/ 目录） */
#include "civetweb/civetweb.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

typedef struct TebRoute {
    char *uri;
    TVal callable;
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
    struct TebWsReg *next;
} TebWsReg;

/* P1-4 WS 连接登记（node 持 obj +1，生命周期 = 连接；mu 保护） */
typedef struct TebWsSock {
    TObj *obj;
    struct mg_connection *conn;
    struct TebWsSock *next;
} TebWsSock;

/* WS 分片装配（FIN=0 数据帧累积；conn 键；仅 drain 读写 → 单线程无锁） */
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
    char *path;                 /* TEQ_STATIC：文件路径（thunk 分配，drain 释放） */
    long long path_size;        /* TEQ_STATIC：文件大小（bytes_sent 近似用） */
    TebWsReg *ws_reg;           /* TEQ_WS_*：所属注册 */
    char *ws_data;              /* TEQ_WS_MSG：帧载荷拷贝（civetweb 已解 mask，出回调即失效） */
    size_t ws_data_len;
    int ws_bits;                /* TEQ_WS_MSG：帧首字节原值（FIN|RSV|opcode），drain 解码 */
    struct TebRequest *next;
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
    TebRoute *routes;          /* 注册的路由列表（与 ctx 解耦） */
    TebRoute *nf;              /* notFound 回调（单条，stop 单独释放） */
    TebRoute *befores;         /* P1-2 before 中间件链（按注册序 = 尾插） */
    TebStatic *statics;        /* P1-3 静态目录映射（多条，头插） */
    TebWsReg *ws_regs;         /* P1-4 WS 注册（头插；stop 释放） */
    TebWsSock *ws_socks;       /* P1-4 WS 连接登记（mu 保护） */
    TebWsFrag *ws_frags;       /* WS 分片装配（drain 独占） */
    TebRequest *req_head;      /* 挂起请求队列头 */
    TebRequest *req_tail;
    uv_mutex_t mu;             /* 保护队列 / done / shutdown / ws_socks */
    uv_cond_t cv;              /* worker↔drain 双向唤醒 */
    bool mu_inited;
    bool shutdown;             /* stop 置位：唤醒全部等待中的 worker */
    bool draining;             /* PHP 主线程：drain 正在处理一批 */
    bool pending_stop;         /* draining 中收到 stop → 批末真停 */
    TVal web_obj;              /* 运行中 Web 对象（+1；real_stop 释放） */
    char upload_tmp_dir[400];  /* 上传 tmp 目录（start 自留键；缺省系统 tmp） */
    int64_t upload_max_bytes;  /* 上传/单字段累计上限（缺省 64M） */
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

/* 首次初始化必须发生在 Web::start（PHP 主线程、mg_start 创建 worker 之前） */
static void teb_lock(void) {
    if (!g_teb.mu_inited) {
        uv_mutex_init(&g_teb.mu);
        uv_cond_init(&g_teb.cv);
        g_teb.mu_inited = true;
    }
    uv_mutex_lock(&g_teb.mu);
}

/* ---- 基础设施 helpers ---- */

/* Request info 缓存读取（定义于 Request 方法区；此处前置） */
static TVal teb_info_get(TObj *o, const char *key);

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

/* 精确匹配优先 */
static TebRoute *teb_match_exact(const char *uri) {
    for (TebRoute *r = g_teb.routes; r; r = r->next)
        if (strcmp(r->uri, uri) == 0) return r;
    return NULL;
}

/* 前缀匹配（"/hello" 命中 "/hello" 与 "/hello/xxx"） */
static TebRoute *teb_match_prefix(const char *uri) {
    size_t lu = strlen(uri);
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        size_t lr = strlen(r->uri);
        if (lr == 0) continue;
        if (lu >= lr && strncmp(r->uri, uri, lr) == 0 &&
            (uri[lr] == '/' || uri[lr] == 0))
            return r;
    }
    return NULL;
}

/* 兜底：返回 URI 最长的一条 */
static TebRoute *teb_match_longest(void) {
    TebRoute *best = NULL;
    size_t best_len = 0;
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        size_t l = strlen(r->uri);
        if (l > best_len) { best_len = l; best = r; }
    }
    return best;
}

/* 匹配顺序（README 路由匹配规则）：精确 → 前缀 → notFound → 最长兜底。
 * 全部落空返回 NULL（thunk 交回 civetweb 默认处理）。 */
static TebRoute *teb_match_route(const char *uri) {
    TebRoute *r = teb_match_exact(uri);
    if (!r) r = teb_match_prefix(uri);
    if (!r) r = g_teb.nf;
    if (!r) r = teb_match_longest();
    return r;
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
 * 拒绝 ".."（防穿越）；条目按注册序，首个命中即用。 */
static bool teb_static_match(const char *uri, char **out_path, long long *out_size) {
    if (!uri) return false;
    size_t lu = strlen(uri);
    for (TebStatic *ts = g_teb.statics; ts; ts = ts->next) {
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

    /* 等待 PHP 侧处理完（done）或服务器停止（shutdown） */
    while (!req->done && !g_teb.shutdown)
        uv_cond_wait(&g_teb.cv, &g_teb.mu);
    bool ok = req->done;
    if (!ok) {
        /* shutdown 且尚未被 drain 摘走：把自己从队列摘除（drain 与 stop
         * 同在 PHP 主线程，不存在"处理中"竞态） */
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
    teb_wr(conn, TEB_413, sizeof TEB_413 - 1);
    teb_stat_resp(413);
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
        req->route = teb_match_route(ri->local_uri_raw);
        if (!req->route) {   /* 全无命中 → civetweb 默认处理 */
            tm_free(req);
            return 0;
        }
        /* body 超限预检：声明的 Content-Length 已超上限且非 multipart →
         * 413 提前拒绝，省掉无意义的请求体传输（multipart 保留 error=1 语义） */
        if (ri->content_length > g_teb.upload_max_bytes &&
            !ri_is_multipart(ri)) {
            teb_write_413(conn);
            tm_free(req);
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

static TArr *teb_req_info_arr(struct mg_connection *conn) {
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

/* Response 对象（props: 0=status 1=body 2=headers 3=cookies）→ 写出 */
static void teb_write_response(struct mg_connection *conn, TObj *r) {
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
    if (r->props[3].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)r->props[3].u.p, "Set-Cookie: ", "=");

    size_t blen = body ? body->len : 0;
    ln = snprintf(line, sizeof line,
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    teb_wr(conn, line, (unsigned long long)ln);
    if (body && blen) teb_wr(conn, body->c, (unsigned long long)blen);
}

/* 回调异常 / 未返回 Response 的兜底 */
static const char TEB_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: text/plain\r\nContent-Length: 21\r\n"
    "Connection: close\r\n\r\nInternal Server Error";

static void teb_write_500(struct mg_connection *conn) {
    teb_wr(conn, TEB_500, sizeof TEB_500 - 1);
    teb_stat_resp(500);
}

/* ---- 流式响应（doc/API.md Stream 节） ----
 * Stream props: 0=conn 1=status 2=headers 3=header_sent 4=ended */

/* 惰性头发送：HTTP/1.1 <status> <reason> + 用户头 + Connection: close。
 * 不带 Content-Length、不用 chunked（close 定界）。幂等。 */
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
    if (s->props[2].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)s->props[2].u.p, "", ": ");
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
    int seq;
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
        /* 文件字段：记字段名/申报名，生成 <tmp_dir>/tphp_up_<pid>_<seq> */
        snprintf(fc->cur_fkey, sizeof fc->cur_fkey, "%s", key ? key : "");
        snprintf(fc->cur_fname, sizeof fc->cur_fname, "%s", filename);
        int n = snprintf(path, pathlen, "%s/tphp_up_%lu_%d",
                         fc->tmp_dir, (unsigned long)uv_os_getpid(), fc->seq++);
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
        if (uf->cls != g_upfile_cls) continue;
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

/* 批末真停（doc/API.md Web::stop）：shutdown+broadcast → mg_stop join → 清队列 →
 * 释放 web_obj 引用与 props[0] → 释放 routes + nf。不依赖 self。 */
static void teb_real_stop(void) {
    teb_lock();
    g_teb.shutdown = true;
    uv_cond_broadcast(&g_teb.cv);
    uv_mutex_unlock(&g_teb.mu);

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
        tval_release(&r->callable);
        tm_free(r);
        r = nx;
    }
    g_teb.routes = NULL;
    if (g_teb.nf) {
        tval_release(&g_teb.nf->callable);
        tm_free(g_teb.nf);
        g_teb.nf = NULL;
    }
    /* P1-2 before 链 */
    for (TebRoute *b = g_teb.befores; b; ) {
        TebRoute *nx = b->next;
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
        tval_release(&rg->on_open);
        tval_release(&rg->on_msg);
        tval_release(&rg->on_close);
        tm_free(rg);
        rg = nx;
    }
    g_teb.ws_regs = NULL;
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

/* WS 分片装配 helpers（仅 drain 调用：单线程无锁） */
static TebWsFrag *teb_ws_frag_find(struct mg_connection *conn) {
    for (TebWsFrag *f = g_teb.ws_frags; f; f = f->next)
        if (f->conn == conn) return f;
    return NULL;
}

static void teb_ws_frag_drop(struct mg_connection *conn) {
    for (TebWsFrag **pp = &g_teb.ws_frags; *pp; pp = &(*pp)->next) {
        if ((*pp)->conn == conn) {
            TebWsFrag *f = *pp;
            *pp = f->next;
            tm_free(f->buf);
            tm_free(f);
            return;
        }
    }
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

/* P1-2 跑 before 链 + 分发（REQ/STATIC 共用）。返回 false = 用户回调抛异常
 * （响应由外层按流式状态收尾）；true = 响应已写出或流式接管。 */
static bool teb_dispatch(VM *vm, TObj *req, struct mg_connection *conn,
                         TebRoute *route, const char *static_path,
                         long long static_size) {
    /* before 链（按注册序）；任一 before 内 beginStream → 后续 before 与
     * 路由/静态全部跳过（外层流式收尾） */
    for (TebRoute *b = g_teb.befores; b; b = b->next) {
        if (req->props[4].t == T_OBJ) return true;
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        bool ok = tphp_vm_call_bounded(vm, b->callable, &argv, 1, &out);
        tval_release(&argv);
        if (!ok) return false;   /* before 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && ((TObj *)out.u.p)->cls == g_resp_cls) {
            teb_write_response(conn, (TObj *)out.u.p);   /* 短路 */
            tval_release(&out);
            return true;
        }
        if (out.t != T_NULL) {
            tphp_warn("PHP Warning: Teb before callback must return null "
                      "or a Tphp\\Teb\\Response instance\n");
            tval_release(&out);
            teb_write_500(conn);
            return true;
        }
        tval_release(&out);
    }
    if (req->props[4].t == T_OBJ) return true;   /* before 内 beginStream */

    if (route) {
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        bool ok = tphp_vm_call_bounded(vm, route->callable, &argv, 1, &out);
        tval_release(&argv);
        if (!ok) return false;   /* route 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && ((TObj *)out.u.p)->cls == g_resp_cls) {
            teb_write_response(conn, (TObj *)out.u.p);
        } else if (req->props[4].t == T_OBJ) {
            /* 流式接管：返回值一律忽略 */
        } else {
            tphp_warn("PHP Warning: Teb route callback must return "
                      "a Tphp\\Teb\\Response instance\n");
            teb_write_500(conn);
        }
        tval_release(&out);
        return true;
    }

    if (static_path) {
        mg_send_mime_file(conn, static_path, NULL);
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
    g_teb.st.ws_frames++;
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
        if (!tphp_vm_call_bounded(vm, reg->on_msg, args2, 3, &out)) {
            tphp_warn("PHP Warning: Teb WS onMessage callback raised "
                      "(closing connection)\n");
            s->props[1] = TV_TRUE;   /* close_requested → data 回调返回 0 */
        }
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

static void teb_drain_once(VM *vm) {
    teb_lock();
    TebRequest *q = g_teb.req_head;
    if (!q && !g_teb.shutdown)
        uv_cond_timedwait(&g_teb.cv, &g_teb.mu, 10 * 1000 * 1000);  /* 10ms */
    q = g_teb.req_head;
    g_teb.req_head = g_teb.req_tail = NULL;
    g_teb.draining = true;
    uv_mutex_unlock(&g_teb.mu);

    while (q) {
        TebRequest *next = q->next;

        if (q->kind == TEQ_REQ || q->kind == TEQ_STATIC) {
            teb_lock();
            g_teb.st.requests_total++;   /* 与入口 413 计数（thunk 线程）同锁 */
            uv_mutex_unlock(&g_teb.mu);
            g_teb.st.requests_active++;
            /* 构造回调入参 Request（不走 ctor；drain 持有其 rc）。
             * props: 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
             *        5=body_cache 6=qp_cache 7=cookie_cache */
            TObj *req = tphp_obj_alloc(g_req_cls);
            req->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            req->props[1] = TV_ARR(teb_req_info_arr(q->conn));
            TVal argv = TV_OBJ(req);
            bool ok = teb_dispatch(vm, req, q->conn,
                                   q->kind == TEQ_REQ ? q->route : NULL,
                                   q->kind == TEQ_STATIC ? q->path : NULL,
                                   q->path_size);

            TVal sv = req->props[4];   /* beginStream 过（before 或 route）？ */
            TObj *stream = (sv.t == T_OBJ && ((TObj *)sv.u.p)->cls == g_stream_cls)
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
                        teb_write_500(q->conn);
                    }
                } else if (stream->props[4].t != T_TRUE) {
                    teb_stream_send_headers(stream);   /* 未发头 → 空体响应头 */
                    stream->props[4] = TV_TRUE;
                }
            } else if (!ok) {
                teb_write_500(q->conn);   /* 非 流式 + 回调异常 */
            }
            teb_reclaim_tmp(req);   /* 未 moveTo 的上传 tmp 回收 */
            tval_release(&argv);   /* Request 对象（回调捕获则继续存活） */
            g_teb.st.requests_active--;
        } else if (q->kind == TEQ_WS_OPEN) {
            g_teb.st.ws_active++;
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
            rq->props[1] = TV_ARR(teb_req_info_arr(q->conn));
            TVal args2[2] = { TV_OBJ(s), TV_OBJ(rq) };
            TVal out = TV_NULL;
            if (!tphp_vm_call_bounded(vm, reg->on_open, args2, 2, &out))
                tphp_warn("PHP Warning: Teb WS onOpen callback raised (ignored)\n");
            tval_release(&out);
            tval_release(&args2[0]);   /* node 持份保留 */
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
                        fr->next = g_teb.ws_frags;
                        g_teb.ws_frags = fr;
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
            g_teb.st.ws_active--;
            g_teb.st.ws_closed++;
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
                if (!tphp_vm_call_bounded(vm, reg->on_close, args2, 1, &out))
                    tphp_warn("PHP Warning: Teb WS onClose callback raised (ignored)\n");
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
    if (stop_now && g_teb.ctx) teb_real_stop();
}

/* ---- Tphp\Teb\Web 原生方法 ---- */

/* route(string $uri, callable $cb): self —— start 前后均可注册 */
static TVal nm_web_route(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *uri = arg_str(vm, args, n, 0, "Web::route");
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::route() expects (string, callable)");
    TVal cb = args[1];
    switch (cb.t) {
    case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
    default:
        tphp_vm_raise(vm, "TypeError",
                      "Web::route(): callable expected as argument 2");
    }

    TebRoute *r = tm_malloc(sizeof *r);
    r->uri = teb_dup(uri);
    r->callable = cb;
    tval_addref(cb);
    r->next = g_teb.routes;
    g_teb.routes = r;
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* notFound(callable $cb): self —— 无路由命中时的回调；单条，重复调用替换 */
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
    if (g_teb.nf) {
        tval_release(&g_teb.nf->callable);
        g_teb.nf->callable = cb;
        tval_addref(cb);
    } else {
        TebRoute *r = tm_malloc(sizeof *r);
        r->uri = NULL;
        r->callable = cb;
        tval_addref(cb);
        r->next = NULL;
        g_teb.nf = r;
    }
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
    uv_mutex_unlock(&g_teb.mu);

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

    /* assoc 数组展平成 key1,val1,...,NULL 的 const char**；
     * upload_tmp_dir/upload_max_bytes 为 teb 自留键——识别后摘除不透传
     * （透传 civetweb 会 unknown option 启动失败） */
    TArr *arr = (TArr *)args[0].u.p;
    tarr_sync_buckets(arr);
    int64_t n_buckets = arr->bcap;
    const char **opts = tm_malloc(sizeof(char *) * (size_t)(n_buckets * 2 + 1));
    int k = 0;
    for (int64_t bi = 0; bi < n_buckets; bi++) {
        ABucket *bk = &arr->buckets[bi];
        if (bk->key.t == T_NULL) continue;
        if (bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;        /* civetweb 配置键必须是字符串 */

        TStr *ks = (TStr *)bk->key.u.p;
        const char *kc = tstr_val(ks);
        TVal val = arr->data[bk->val_off];

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
    opts[k] = NULL;

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
    return TV_NULL;
}

/* run(): void —— 阻塞主循环；stop() 或析构置 ctx=0 时退出 */
static TVal nm_web_run(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    while (g_teb.ctx) teb_drain_once(vm);
    return TV_NULL;
}

/* stop(): void —— 幂等；draining 中只挂 pending_stop（回调内 stop 合法），
 * 批末由 drain 执行 teb_real_stop（doc/API.md Web::stop） */
static TVal nm_web_stop(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (!g_teb.ctx) return TV_NULL;
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
    /* 尾插：按注册序执行 */
    if (!g_teb.befores) {
        g_teb.befores = b;
    } else {
        TebRoute *t = g_teb.befores;
        while (t->next) t = t->next;
        t->next = b;
    }
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
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
    ts->next = g_teb.statics;   /* 头插：thunk 只读指针，注册即挂 */
    g_teb.statics = ts;
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
    rg->next = g_teb.ws_regs;
    g_teb.ws_regs = rg;
    /* 运行中注册：立即挂（start 前注册者由 start 批量挂） */
    if (g_teb.ctx) {
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
 *                5=body_cache 6=qp_cache 7=cookie_cache
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

/* cookie(string $k, string $v): self */
static TVal nm_resp_cookie(VM *vm, void *self, TVal *args, int n) {
    const char *k = arg_str(vm, args, n, 0, "Response::cookie");
    const char *v = arg_str(vm, args, n, 1, "Response::cookie");
    TObj *o = (TObj *)self;
    TVal key = TV_STR(tstr_intern(k, strlen(k)));
    TVal val = TV_STR(tstr_new(v, strlen(v)));
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
 * 返回 0 成功（out/outlen 归调用方 tm_free）；1 不存在/不可读；2 超上限。 */
static int teb_read_file(const char *path, char **out, size_t *outlen) {
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
    if (sz > g_teb.upload_max_bytes) {
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
    int rc = teb_read_file(pathcopy, &data, &dlen);
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

/* ---- Tphp\Teb\Stream 原生方法（doc/API.md Stream 节）----
 * props: 0=conn 1=status 2=headers 3=header_sent 4=ended */

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
    o->props[1] = TV_TRUE;   /* close_requested → data 回调返回 0 */
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
    teb_cls_add_native(w, "notFound", (void *)nm_web_not_found, false);
    teb_cls_add_native(w, "before", (void *)nm_web_before, false);
    teb_cls_add_native(w, "file", (void *)nm_web_file, false);
    teb_cls_add_native(w, "websocket", (void *)nm_web_websocket, false);
    teb_cls_add_native(w, "stats", (void *)nm_web_stats, false);
    teb_cls_add_native(w, "start", (void *)nm_web_start, false);
    teb_cls_add_native(w, "drain", (void *)nm_web_drain, false);
    teb_cls_add_native(w, "run", (void *)nm_web_run, false);
    teb_cls_add_native(w, "stop", (void *)nm_web_stop, false);
    tphp_class_register_compile(w);
    g_web_cls = w;

    /* Request：props 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
     *          5=body_cache 6=qp_cache 7=cookie_cache */
    TClass *rq = teb_cls_new("Tphp\\Teb\\Request");
    teb_cls_add_prop(rq, "conn");
    teb_cls_add_prop(rq, "info");
    teb_cls_add_prop(rq, "form_cache");
    teb_cls_add_prop(rq, "files_cache");
    teb_cls_add_prop(rq, "stream_obj");
    teb_cls_add_prop(rq, "body_cache");
    teb_cls_add_prop(rq, "qp_cache");
    teb_cls_add_prop(rq, "cookie_cache");
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

    /* Stream：props 0=conn 1=status 2=headers 3=header_sent 4=ended */
    TClass *st = teb_cls_new("Tphp\\Teb\\Stream");
    teb_cls_add_prop(st, "conn");
    teb_cls_add_prop(st, "status");
    teb_cls_add_prop(st, "headers");
    teb_cls_add_prop(st, "header_sent");
    teb_cls_add_prop(st, "ended");
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
