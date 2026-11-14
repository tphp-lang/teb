/*
 * tphp/teb - C extension (object API)
 *
 * civetweb HTTP server 原生桥接，2025 master 源码随包静态链入。
 * 对外只暴露五个原生类（无顶层函数）：
 *
 *   Tphp\Teb\Web          服务器生命周期：route()/notFound()/start()/run()/drain()/stop()
 *   Tphp\Teb\Request      路由回调入参：method()/uri()/header()/body()/form()/file()...
 *   Tphp\Teb\Response     回调返回值：new Response(...) + 链式 + 静态工厂 json/text/html/redirect
 *   Tphp\Teb\Stream       流式响应：beginStream() 产物，惰性发头、close 定界
 *   Tphp\Teb\UploadedFile 上传文件：form()/files() 产物，moveTo() 落盘
 *
 * 线程模型（同步阻塞，2026-11-13 冒烟验证）：
 *   civetweb worker 进入 thunk → 入队 → 阻塞等 done；PHP 主线程 Web::run()
 *   内部 drain 循环取批 → 构造 Request → bounded 调用户回调 → 写 Response
 *   → 唤醒 worker。回调期间 conn 归本请求独占（worker 阻塞在 thunk 内）。
 *   stop 先置 shutdown + broadcast 防死锁，worker 醒来自行摘队列释放。
 *   无匹配路由 → notFound 命中入队；无 nf → 最长路由兜底；全无 → thunk
 *   返回 0，civetweb 默认处理（静态文件/404）。
 *   回调抛异常或未返回 Response → C 侧写 500，异常信息照常打印；
 *   已 beginStream 的请求返回值一律忽略（流式收尾契约见 P0-design §1）。
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

typedef struct TebRequest {
    struct mg_connection *conn;
    TebRoute *route;
    struct TebRequest *next;
    bool done;              /* drain 已写完响应（唤醒等待的 worker） */
} TebRequest;

typedef struct {
    bool inited;
    struct mg_context *ctx;
    TebRoute *routes;          /* 注册的路由列表（与 ctx 解耦） */
    TebRoute *nf;              /* notFound 回调（单条，stop 单独释放） */
    TebRequest *req_head;      /* 挂起请求队列头 */
    TebRequest *req_tail;
    uv_mutex_t mu;             /* 保护队列 / done / shutdown */
    uv_cond_t cv;              /* worker↔drain 双向唤醒 */
    bool mu_inited;
    bool shutdown;             /* stop 置位：唤醒全部等待中的 worker */
    bool draining;             /* PHP 主线程：drain 正在处理一批 */
    bool pending_stop;         /* draining 中收到 stop → 批末真停 */
    TVal web_obj;              /* 运行中 Web 对象（+1；real_stop 释放） */
    char upload_tmp_dir[400];  /* 上传 tmp 目录（start 自留键；缺省系统 tmp） */
    int64_t upload_max_bytes;  /* 上传/单字段累计上限（缺省 64M） */
} TebGlobal;

static TebGlobal g_teb;

/* 类句柄（registry 构建于 init，drain 只跑主线程） */
static _Thread_local TClass *g_web_cls;
static _Thread_local TClass *g_req_cls;
static _Thread_local TClass *g_resp_cls;
static _Thread_local TClass *g_stream_cls;
static _Thread_local TClass *g_upfile_cls;

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

/* 匹配顺序（P0-design §3）：精确 → 前缀 → notFound → 最长兜底。
 * 全部落空返回 NULL（thunk 交回 civetweb 默认处理）。 */
static TebRoute *teb_match_route(const char *uri) {
    TebRoute *r = teb_match_exact(uri);
    if (!r) r = teb_match_prefix(uri);
    if (!r) r = g_teb.nf;
    if (!r) r = teb_match_longest();
    return r;
}

/* ---- civetweb request handler thunk（同步阻塞，见文件头） ---- */
static int teb_req_thunk(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri || !ri->local_uri_raw) return 0;

    TebRoute *r = teb_match_route(ri->local_uri_raw);
    if (!r) return 0;   /* 无路由 → civetweb 默认处理 */

    TebRequest *req = tm_malloc(sizeof *req);
    req->conn = conn;
    req->route = r;      /* 只读指针，无并发写风险 */
    req->done = false;
    req->next = NULL;

    teb_lock();
    if (g_teb.shutdown) {            /* stop 与新请求赛跑 */
        uv_mutex_unlock(&g_teb.mu);
        tm_free(req);
        return 0;
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
    tm_free(req);   /* 分配者释放；done 后 drain 不再访问该节点 */
    return ok ? 1 : 0;
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
        mg_write(conn, line, (unsigned long long)ln);
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
    mg_write(conn, line, (unsigned long long)ln);

    if (r->props[2].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)r->props[2].u.p, "", ": ");
    if (r->props[3].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)r->props[3].u.p, "Set-Cookie: ", "=");

    size_t blen = body ? body->len : 0;
    ln = snprintf(line, sizeof line,
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    mg_write(conn, line, (unsigned long long)ln);
    if (body && blen) mg_write(conn, body->c, (unsigned long long)blen);
}

/* 回调异常 / 未返回 Response 的兜底 */
static const char TEB_500[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: text/plain\r\nContent-Length: 21\r\n"
    "Connection: close\r\n\r\nInternal Server Error";

static void teb_write_500(struct mg_connection *conn) {
    mg_write(conn, TEB_500, sizeof TEB_500 - 1);
}

/* ---- 流式响应（P0-design §1） ----
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
    mg_write(conn, line, (unsigned long long)ln);
    if (s->props[2].t == T_ARR)
        teb_emit_pairs(conn, (TArr *)s->props[2].u.p, "", ": ");
    mg_write(conn, "Connection: close\r\n\r\n", 21);
}

/* ---- body 缓存读取（P0-design §4：body()/json()/form() 共用 props[5]） ----
 * 首次调用读满连接（上限 upload_max_bytes，超限截断），缓存后复用。 */
static TStr *teb_body_cached(VM *vm, TObj *o) {
    (void)vm;
    if (o->props[5].t == T_STR) return (TStr *)o->props[5].u.p;
    if (o->props[0].t != T_INT) {
        TStr *s = tstr_new("", 0);
        teb_set_slot(o, 5, TV_STR(s));
        return s;
    }
    struct mg_connection *conn =
        (struct mg_connection *)(uintptr_t)o->props[0].u.i;
    size_t cap = 8192, len = 0;
    int64_t limit = g_teb.upload_max_bytes;
    char *buf = tm_malloc(cap);
    for (;;) {
        if (len == cap) {
            if ((int64_t)len >= limit) break;
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

/* ---- 文件上传（P0-design §2）----
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
        if (n < 0 || (size_t)n >= pathlen)
            return MG_FORM_FIELD_STORAGE_ABORT;   /* tmp 路径超 512B */
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
    /* 上限：单请求累计（文本字段值）超 upload_max_bytes → 中止解析 */
    if (fc->total + (int64_t)valuelen > fc->max_bytes)
        return MG_FORM_FIELD_HANDLE_ABORT;
    if (fc->acc_len + valuelen + 1 > fc->acc_cap) {
        size_t nc = fc->acc_cap ? fc->acc_cap : 256;
        while (nc < fc->acc_len + valuelen + 1) nc *= 2;
        char *na = tm_realloc(fc->acc, nc);
        if (!na) return MG_FORM_FIELD_HANDLE_ABORT;
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
 * 结果缓存 props[2]（文本字段）与 props[3]（文件）。 */
static void teb_form_parse_multipart(TObj *req, struct mg_connection *conn) {
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

    mg_handle_form_request(conn, &fdh);   /* <0 = 无效请求：已得字段照常缓存 */
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

/* ---- Response 静态工厂构建 helper（P0-design §5） ----
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

/* 批末真停（P0-design §6）：shutdown+broadcast → mg_stop join → 清队列 →
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
        if (q->route) {
            /* 构造回调入参 Request（不走 ctor；drain 持有其 rc）。
             * props: 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
             *        5=body_cache 6=qp_cache 7=cookie_cache */
            TObj *req = tphp_obj_alloc(g_req_cls);
            req->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            req->props[1] = TV_ARR(teb_req_info_arr(q->conn));
            TVal argv = TV_OBJ(req);
            TVal out = TV_NULL;
            bool ok = tphp_vm_call_bounded(vm, q->route->callable, &argv,
                                           1, &out);

            TVal sv = req->props[4];   /* beginStream 过？ */
            TObj *stream = (sv.t == T_OBJ && ((TObj *)sv.u.p)->cls == g_stream_cls)
                ? (TObj *)sv.u.p : NULL;
            if (stream) {
                /* 流式契约：返回值一律忽略（含 Response）；未 end 自动收尾；
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
            } else if (ok && out.t == T_OBJ &&
                       ((TObj *)out.u.p)->cls == g_resp_cls) {
                teb_write_response(q->conn, (TObj *)out.u.p);
            } else {
                if (ok)
                    tphp_warn("PHP Warning: Teb route callback must return "
                              "a Tphp\\Teb\\Response instance\n");
                teb_write_500(q->conn);
            }
            tval_release(&out);
            teb_reclaim_tmp(req);   /* 未 moveTo 的上传 tmp 回收 */
            tval_release(&argv);   /* Request 对象（回调捕获则继续存活） */
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

    /* 上传自留键缺省（P0-design §2：upload_tmp_dir / upload_max_bytes） */
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

    o->props[0] = TV_INT((int64_t)(uintptr_t)g_teb.ctx);
    /* 登记运行中实例（real_stop 释放引用并清 props[0]） */
    g_teb.web_obj = TV_OBJ(o);
    tval_addref(TV_OBJ(o));
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
 * 批末由 drain 执行 teb_real_stop（P0-design §6） */
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

/* ---- 解析助手（P0-design §4）----
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

/* ---- 表单/上传（P0-design §2）----
 * form(): GET/HEAD → query 解析；multipart → mg_handle_form_request（耗尽
 * body，勿先调 body()）；其余 → body 缓存 urlencoded 解析。结果缓存
 * props[2]（文本字段）与 props[3]（文件，非 multipart 为空数组）。 */

static TVal nm_req_form(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
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
        teb_form_parse_multipart(o, conn);   /* 同时缓存 props[2]/props[3] */
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

/* ---- Response 静态工厂（P0-design §5；native static，self=NULL） ---- */

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

/* ---- Tphp\Teb\Stream 原生方法（P0-design §1）----
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
    int w = (int)mg_write(conn, cs->c, (unsigned long long)cs->len);
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
                mg_write(conn, last->c, (unsigned long long)last->len);
        }
    }
    o->props[4] = TV_TRUE;
    return TV_NULL;
}

/* ---- Tphp\Teb\UploadedFile 原生方法（P0-design §2）----
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

/* ---- 类注册（init 阶段，首脚本编译前） ---- */

static void teb_register_classes(void) {
    /* Web：props[0] = ctx 句柄 */
    TClass *w = teb_cls_new("Tphp\\Teb\\Web");
    teb_cls_add_prop(w, "ctx");
    teb_cls_add_native(w, "route", (void *)nm_web_route, false);
    teb_cls_add_native(w, "notFound", (void *)nm_web_not_found, false);
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
