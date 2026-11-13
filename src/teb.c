/*
 * tphp/teb - C extension (object API)
 *
 * civetweb HTTP server 原生桥接，2025 master 源码随包静态链入。
 * 对外只暴露三个原生类（无顶层函数）：
 *
 *   Tphp\Teb\Web      服务器生命周期：route() / start() / run() / drain() / stop()
 *   Tphp\Teb\Request  路由回调入参：method()/uri()/header()/body()/remoteAddr()...
 *   Tphp\Teb\Response 回调返回值：new Response(body, status, headers) +
 *                     header()/cookie()/status() 链式；HTTP 头拼装在 C 侧完成
 *
 * 线程模型（同步阻塞，2026-11-13 冒烟验证）：
 *   civetweb worker 进入 thunk → 入队 → 阻塞等 done；PHP 主线程 Web::run()
 *   内部 drain 循环取批 → 构造 Request → bounded 调用户回调 → 写 Response
 *   → 唤醒 worker。回调期间 conn 归本请求独占（worker 阻塞在 thunk 内）。
 *   stop 先置 shutdown + broadcast 防死锁，worker 醒来自行摘队列释放。
 *   无匹配路由 → thunk 返回 0，civetweb 默认处理（静态文件/404）。
 *   回调抛异常或未返回 Response → C 侧写 500，异常信息照常打印。
 */
#include "pkg.h"            /* tphp_ext_reg_const_str / tphp_ext_reg_fn */
#include "value.h"          /* TVal / TV_* / tstr_* / tm_* */
#include "opcode.h"         /* VM / BuiltinFn */
#include "class.h"          /* TClass / TObj / tphp_obj_alloc / register_compile */
#include "tphp.h"           /* tphp_warn */

#include <uv.h>             /* uv_mutex_t / uv_cond_t */

/* civetweb 头（teb 包内 civetweb/ 目录） */
#include "civetweb/civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows MSVC：strdup 不是标准函数 */
#if defined(_MSC_VER) && !defined(__clang__)
static inline char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = tm_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}
#define strdup xstrdup
#endif

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
    TebRequest *req_head;      /* 挂起请求队列头 */
    TebRequest *req_tail;
    uv_mutex_t mu;             /* 保护队列 / done / shutdown */
    uv_cond_t cv;              /* worker↔drain 双向唤醒 */
    bool mu_inited;
    bool shutdown;             /* stop 置位：唤醒全部等待中的 worker */
} TebGlobal;

static TebGlobal g_teb;

/* 类句柄（registry 构建于 init，drain 只跑主线程） */
static _Thread_local TClass *g_web_cls;
static _Thread_local TClass *g_req_cls;
static _Thread_local TClass *g_resp_cls;

/* 首次初始化必须发生在 Web::start（PHP 主线程、mg_start 创建 worker 之前） */
static void teb_lock(void) {
    if (!g_teb.mu_inited) {
        uv_mutex_init(&g_teb.mu);
        uv_cond_init(&g_teb.cv);
        g_teb.mu_inited = true;
    }
    uv_mutex_lock(&g_teb.mu);
}

/* ---- 路由匹配 ---- */
static TebRoute *teb_match_route(const char *uri) {
    /* 精确匹配优先 */
    for (TebRoute *r = g_teb.routes; r; r = r->next)
        if (strcmp(r->uri, uri) == 0) return r;
    /* 前缀匹配 */
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        size_t lu = strlen(uri), lr = strlen(r->uri);
        if (lr == 0) continue;
        if (lu >= lr && strncmp(r->uri, uri, lr) == 0 &&
            (uri[lr] == '/' || uri[lr] == 0))
            return r;
    }
    /* 兜底：返回 URI 最长的一条 */
    TebRoute *best = NULL;
    size_t best_len = 0;
    for (TebRoute *r = g_teb.routes; r; r = r->next) {
        size_t l = strlen(r->uri);
        if (l > best_len) { best_len = l; best = r; }
    }
    return best;
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
    S("uri", ri->request_uri);
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
        struct mg_header *h = &ri->http_headers[i];
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

/* ---- drain 核心：取批 → 构造 Request → 调回调 → 写响应 → 唤醒 ---- */

static void teb_drain_once(VM *vm) {
    teb_lock();
    TebRequest *q = g_teb.req_head;
    if (!q && !g_teb.shutdown)
        uv_cond_timedwait(&g_teb.cv, &g_teb.mu, 10 * 1000 * 1000);  /* 10ms */
    q = g_teb.req_head;
    g_teb.req_head = g_teb.req_tail = NULL;
    uv_mutex_unlock(&g_teb.mu);

    while (q) {
        TebRequest *next = q->next;
        if (q->route) {
            /* 构造回调入参 Request（不走 ctor；drain 持有其 rc） */
            TObj *req = tphp_obj_alloc(g_req_cls);
            req->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            req->props[1] = TV_ARR(teb_req_info_arr(q->conn));
            TVal argv = TV_OBJ(req);
            TVal out = TV_NULL;
            bool ok = tphp_vm_call_bounded(vm, q->route->callable, &argv,
                                           1, &out);
            if (ok && out.t == T_OBJ &&
                ((TObj *)out.u.p)->cls == g_resp_cls) {
                teb_write_response(q->conn, (TObj *)out.u.p);
            } else {
                if (ok)
                    tphp_warn("PHP Warning: Teb route callback must return "
                              "a Tphp\\Teb\\Response instance\n");
                teb_write_500(q->conn);
            }
            tval_release(&out);
            tval_release(&argv);   /* Request 对象（回调捕获则继续存活） */
        }
        teb_lock();
        q->done = true;
        uv_cond_broadcast(&g_teb.cv);   /* 唤醒该请求的 worker */
        uv_mutex_unlock(&g_teb.mu);
        /* q 节点由等待中的 worker 自己释放 */
        q = next;
    }
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
    r->uri = strdup(uri);
    r->callable = cb;
    tval_addref(cb);
    r->next = g_teb.routes;
    g_teb.routes = r;
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}

/* start(array $options): self —— civetweb 配置项直通 */
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
    uv_mutex_unlock(&g_teb.mu);

    /* assoc 数组展平成 key1,val1,...,NULL 的 const char** */
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
        char *kc = tm_malloc(ks->len + 1);
        memcpy(kc, ks->c, ks->len); kc[ks->len] = 0;
        opts[k++] = kc;

        TVal val = arr->data[bk->val_off];
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

    /* 统一 thunk 挂所有 URI：命中路由入队阻塞，未命中交 civetweb 默认 */
    mg_set_request_handler(g_teb.ctx, "/*", teb_req_thunk, NULL);

    o->props[0] = TV_INT((int64_t)(uintptr_t)g_teb.ctx);
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

/* stop(): void —— 唤醒全部等待 worker 防死锁，再 mg_stop join */
static TVal nm_web_stop(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    if (!g_teb.ctx) return TV_NULL;
    if (o->props[0].t != T_INT ||
        o->props[0].u.i != (int64_t)(uintptr_t)g_teb.ctx)
        tphp_vm_raise(vm, "Error",
                      "Tphp\\Teb\\Web::stop(): this instance is not the "
                      "running server");

    teb_lock();
    g_teb.shutdown = true;
    uv_cond_broadcast(&g_teb.cv);
    uv_mutex_unlock(&g_teb.mu);

    mg_stop(g_teb.ctx);   /* join 全部 worker；此后队列必为空 */
    g_teb.ctx = NULL;
    g_teb.req_head = g_teb.req_tail = NULL;
    o->props[0] = TV_NULL;

    /* 释放路由表 */
    for (TebRoute *r = g_teb.routes; r; ) {
        TebRoute *nx = r->next;
        tm_free(r->uri);
        tval_release(&r->callable);
        tm_free(r);
        r = nx;
    }
    g_teb.routes = NULL;
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
    const char *name = arg_str(vm, args, n, 0, "Request::header");
    TVal hs = teb_info_get((TObj *)self, "headers");
    if (hs.t != T_ARR) return TV_NULL;
    TArr *a = (TArr *)hs.u.p;
    /* 精确查（interned 键可指针相等快路） */
    TVal *v = tarr_get(a, TV_STR(tstr_intern(name, strlen(name))));
    if (v && v->t != T_UNDEF) { tval_addref(*v); return *v; }
    /* 大小写不敏感线性兜底 */
    tarr_sync_buckets(a);
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t == T_NULL || bk->val_off == UINT32_MAX) continue;
        if (bk->key.t != T_STR) continue;
        TStr *k = (TStr *)bk->key.u.p;
        if (tstr_case_eq(tstr_val(k), name)) {
            TVal val = a->data[bk->val_off];
            tval_addref(val);
            return val;
        }
    }
    return TV_NULL;
}

/* headers(): array */
static TVal nm_req_headers(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return teb_info_ret((TObj *)self, "headers");
}

/* body(int $maxLen = 65536): string */
static TVal nm_req_body(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    if (o->props[0].t != T_INT) return TV_STR(tstr_new("", 0));
    int64_t max_len = (n >= 1 && args[0].t == T_INT) ? args[0].u.i : 65536;
    if (max_len <= 0) max_len = 65536;
    struct mg_connection *conn =
        (struct mg_connection *)(uintptr_t)o->props[0].u.i;
    char *buf = tm_malloc((size_t)max_len);
    int rc = mg_read(conn, buf, (unsigned long long)max_len);
    if (rc <= 0) { tm_free(buf); return TV_STR(tstr_new("", 0)); }
    TStr *s = tstr_new(buf, (size_t)rc);
    tm_free(buf);
    return TV_STR(s);
}

/* ---- Tphp\Teb\Response 原生方法 ----
 * props: 0=status(int) 1=body(str) 2=headers(arr) 3=cookies(arr) */

/* 数组槽 COW 后写入（rc>1 时分离，防改到共享数组）+ addref（tarr_set 不
 * addref，见 vm.c op_assign 约定） */
static void teb_arr_set_owned(TObj *o, int slot, TVal key, TVal val) {
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
    teb_arr_set_owned(o, 2, TV_STR(tstr_intern(k, strlen(k))),
                          TV_STR(tstr_new(v, strlen(v))));
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* cookie(string $k, string $v): self */
static TVal nm_resp_cookie(VM *vm, void *self, TVal *args, int n) {
    const char *k = arg_str(vm, args, n, 0, "Response::cookie");
    const char *v = arg_str(vm, args, n, 1, "Response::cookie");
    TObj *o = (TObj *)self;
    teb_arr_set_owned(o, 3, TV_STR(tstr_intern(k, strlen(k))),
                          TV_STR(tstr_new(v, strlen(v))));
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* withHeaders(array $headers): self */
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
        teb_arr_set_owned(o, 2, bk->key, in->data[bk->val_off]);
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

/* ---- 类注册（init 阶段，首脚本编译前） ---- */

static void teb_register_classes(void) {
    /* Web：props[0] = ctx 句柄 */
    TClass *w = teb_cls_new("Tphp\\Teb\\Web");
    teb_cls_add_prop(w, "ctx");
    teb_cls_add_native(w, "route", (void *)nm_web_route, false);
    teb_cls_add_native(w, "start", (void *)nm_web_start, false);
    teb_cls_add_native(w, "drain", (void *)nm_web_drain, false);
    teb_cls_add_native(w, "run", (void *)nm_web_run, false);
    teb_cls_add_native(w, "stop", (void *)nm_web_stop, false);
    tphp_class_register_compile(w);
    g_web_cls = w;

    /* Request：props[0] = conn 句柄，props[1] = info 缓存 */
    TClass *rq = teb_cls_new("Tphp\\Teb\\Request");
    teb_cls_add_prop(rq, "conn");
    teb_cls_add_prop(rq, "info");
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
    tphp_class_register_compile(rp);
    g_resp_cls = rp;
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
