/* teb_route.c - 路由匹配五档/模板解析/静态匹配/注册原生(route/快捷/nf/before/file/websocket) */
#include "teb_impl.h"

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
uint32_t teb_method_bit(const char *m) {
    if (!m) return 0;
    if (teb_str_icmp(m, "GET"))     return TEB_M_GET;
    if (teb_str_icmp(m, "POST"))    return TEB_M_POST;
    if (teb_str_icmp(m, "PUT"))     return TEB_M_PUT;
    if (teb_str_icmp(m, "PATCH"))   return TEB_M_PATCH;
    if (teb_str_icmp(m, "DELETE"))  return TEB_M_DELETE;
    if (teb_str_icmp(m, "HEAD"))    return TEB_M_HEAD;
    if (teb_str_icmp(m, "OPTIONS")) return TEB_M_OPTIONS;
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

/* 匹配结果（批A）TebMatch 定义见 teb_impl.h */

/* 五档匹配（README 路由规则，批A 扩展）：T1 精确字面量 → T2 模板全匹配
 * → T3 前缀字面量 → （T1-T3 全扫无 HIT 且 method_miss → 405，提前返回）
 * → T4 notFound → T5 最长兜底（模板参与，无方法门——保「任意未命中 URI
 * 落最长注册路由」既有契约）。每档扫全档找方法相容候选（后注册优先，
 * get+post 同路径共存）。整段在 mu 内；节点发布后不改不删，锁外可用。 */
void teb_match_route(const char *uri, uint32_t mbit, TebMatch *out) {
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
bool teb_static_match(const char *uri, char **out_path, long long *out_size) {
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
/* ---- Tphp\Teb\Web 原生方法 ---- */

/* 统一注册入口（批A，镜像 webman addRoute 先例）：route() 走 mask=0（任意
 * 方法），7 个方法快捷走各自位。模板解析在节点 malloc 前（raise 零泄漏）。 */
static TVal teb_route_add(VM *vm, void *self, const char *who,
                          TVal *args, int n, uint32_t mask) {
    const char *uri = teb_arg_str(vm, args, n, 0, who);
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
TVal nm_web_route(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::route", args, n, 0);
}

/* 方法快捷注册（批A）：get/post/put/patch/delete/head/options 同款链式 self */
TVal nm_web_get(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::get", args, n, TEB_M_GET);
}
TVal nm_web_post(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::post", args, n, TEB_M_POST);
}
TVal nm_web_put(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::put", args, n, TEB_M_PUT);
}
TVal nm_web_patch(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::patch", args, n, TEB_M_PATCH);
}
TVal nm_web_delete(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::delete", args, n, TEB_M_DELETE);
}
TVal nm_web_head(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::head", args, n, TEB_M_HEAD);
}
TVal nm_web_options(VM *vm, void *self, TVal *args, int n) {
    return teb_route_add(vm, self, "Web::options", args, n, TEB_M_OPTIONS);
}

/* notFound(callable $cb): self —— 无路由命中时的回调；单条，重复调用替换。
 * 替换走「新节点 + 退休链」：thunk 可能正持有旧节点指针，就地改写 callable
 * 会撕引用计数——旧节点留到 stop 统一释放。 */
TVal nm_web_not_found(VM *vm, void *self, TVal *args, int n) {
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
/* P1-2 before(callable $cb): self —— 全局中间件（按注册序执行；单条可重复注册） */
TVal nm_web_before(VM *vm, void *self, TVal *args, int n) {
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
/* P1-3 file(string $uriPrefix, string $dir): self —— 静态目录映射（多条） */
TVal nm_web_file(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *prefix = teb_arg_str(vm, args, n, 0, "Web::file");
    const char *dir = teb_arg_str(vm, args, n, 1, "Web::file");
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
TVal nm_web_websocket(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Web::websocket() expects (string $uri, callable $onOpen"
                      ", ?callable $onMessage = null, ?callable $onClose = null)");
    const char *uri = teb_arg_str(vm, args, n, 0, "Web::websocket");
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
        mg_set_websocket_handler(g_teb.ctx, rg->uri, teb_ws_connect_ok,
                                 teb_ws_ready_cb, teb_ws_data_cb, teb_ws_close_cb, rg);
    }
    tval_addref(TV_OBJ((TObj *)self));
    return TV_OBJ((TObj *)self);
}
