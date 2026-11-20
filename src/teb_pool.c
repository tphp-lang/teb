/* teb_pool.c - 执行池:泵源码 consts/autoload 导入/pool thread/spawn/快照/backfill + 池 TLS 定义 */
#include "teb_impl.h"

/* ---- 执行池状态 ---- */
static Program *g_pump_prog;   /* 泵 Program（main 编译一次，进程级只读共享） */

/* 池线程专属（主线程恒 false/空；_Thread_local 天然隔离；声明见 teb_impl.h） */
_Thread_local bool g_in_pool_thread = false;
_Thread_local bool g_consts_imported = false;
_Thread_local bool g_autoload_imported = false;   /* 批B */
_Thread_local TebLocalCb *g_local_cbs = NULL;
_Thread_local int g_local_n = 0, g_local_cap = 0;
_Thread_local uint64_t g_local_gen = 0;
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
void teb_spawn_pools(void) {
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
char *teb_const_snapshot(uint32_t *out_len) {
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
char *teb_autoload_snapshot(uint32_t *out_len) {
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
void teb_backfill_blobs(void) {
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

/* ---- 类注册：Internal 泵类（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_internal(void) {
    /* Internal：泵类（pump Program 的 `while(\Tphp\Teb\Internal\pump())`
     * 调用目标；native static，self=NULL。仅此一个方法，无 ctor/props） */
    TClass *it = teb_cls_new("Tphp\\Teb\\Internal");
    teb_cls_add_native(it, "pump", (void *)nm_pool_pump, true);
    teb_cls_add_native(it, "inPoolThread", (void *)nm_internal_in_pool_thread, true);
    tphp_class_register_compile(it);
}
