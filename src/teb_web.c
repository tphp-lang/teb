/* teb_web.c - real_stop/poll_stop/nm_web_*(start/drain/run/stop/stats/clonestate)/register_web */
#include "teb_impl.h"

/* ---- drain 核心：取批 → 构造 Request → 调回调 → 写响应 → 唤醒 ---- */

/* 批末真停（doc/API.md Web::stop）：shutdown+broadcast → join 执行池 →
 * mg_stop join → 清队列 → 释放 web_obj 引用与 props[0] → 释放配置（含
 * nf 退休链）。不依赖 self。顺序约束：所有消费者（池/主线程）都必须在
 * mg_stop 与配置释放之前退出，否则会踩在飞请求或悬挂配置指针上。 */
void teb_real_stop(void) {
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
        mg_set_websocket_handler(g_teb.ctx, rg->uri, teb_ws_connect_ok,
                                 teb_ws_ready_cb, teb_ws_data_cb, teb_ws_close_cb, rg);
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
    if (g_teb_drain_depth > 0) {   /* 本线程批内（线程局部，见 teb_impl.h） */
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

/* ---- 类注册：Web（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_web(void) {
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
}
