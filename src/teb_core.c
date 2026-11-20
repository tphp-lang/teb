/* teb_core.c - 全局态/锁/基础 helpers/类构建器/回调快照 + 注册编排与 init 符号 */
#include "teb_impl.h"

/* 统一分配器：tm_malloc + memcpy。勿用 libc strdup —— stop 释放路由表走
 * tm_free，strdup 块与之混搭会 mi_free invalid pointer（2026-11-14 冒烟实测）。 */
char *teb_dup(const char *s) {
    size_t n = strlen(s);
    char *p = tm_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

TebGlobal g_teb;

/* 类句柄（registry 构建于 init，drain 只跑主线程） */
_Thread_local TClass *g_web_cls;
_Thread_local TClass *g_req_cls;
_Thread_local TClass *g_resp_cls;
_Thread_local TClass *g_stream_cls;
_Thread_local TClass *g_upfile_cls;
_Thread_local TClass *g_ws_cls;
_Thread_local TClass *g_sess_cls;

/* 首次初始化必须发生在 Web::start（PHP 主线程、mg_start 创建 worker 之前） */
void teb_lock(void) {
    if (!g_teb.mu_inited) {
        uv_mutex_init(&g_teb.mu);
        uv_cond_init(&g_teb.cv);
        g_teb.mu_inited = true;
    }
    uv_mutex_lock(&g_teb.mu);
}

/* ws_frag_mu + per-sid 条带锁一次性初始化（start 内、建线程前调用，
 * 单次赋值无竞态窗口） */
void teb_aux_locks_init(void) {
    if (g_teb.aux_locks_inited) return;
    uv_mutex_init(&g_teb.ws_frag_mu);
    for (int i = 0; i < 64; i++) uv_mutex_init(&g_teb.sess_locks[i]);
    g_teb.aux_locks_inited = true;
}

/* per-sid 条带锁（open 持有 → flush 释放；覆盖整个请求的读-改-写段，
 * 同会话请求串行 = PHP file session 独占锁同语义，防丢更新） */
void teb_sess_lock(const char *sid, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)sid[i]; h *= 16777619u; }
    uv_mutex_lock(&g_teb.sess_locks[h & 63]);
}
void teb_sess_unlock(const char *sid, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)sid[i]; h *= 16777619u; }
    uv_mutex_unlock(&g_teb.sess_locks[h & 63]);
}

/* ---- 基础设施 helpers ---- */

/* P1-1 统一写出口：全部 PHP 侧响应写出走此（bytes_sent 累加）。
 * 主线程（drain/回调）与 thunk 线程（入口 413）都会调 → 计数加锁。 */
int teb_wr(struct mg_connection *conn, const void *buf, size_t len) {
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
void teb_stat_resp(int64_t code) {
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
bool teb_is_cls(TObj *o, TClass *c, const char *nm) {
    if (!o || !o->cls) return false;
    return o->cls == c || tstr_case_eq(tstr_val(o->cls->name), nm);
}

/* ASCII 大小写不敏感整串比较 */
bool teb_str_icmp(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* ASCII 大小写不敏感子串 */
bool teb_str_icontains(const char *hay, const char *needle) {
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
void teb_set_slot(TObj *o, int slot, TVal v) {
    tval_release(&o->props[slot]);
    o->props[slot] = v;
}

/* ---- libuv 同步 fs（UTF-8 路径安全；loop=NULL = 当前 loop） ---- */

bool teb_fs_unlink(const char *path) {
    uv_fs_t r;
    int rc = uv_fs_unlink(NULL, &r, path, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

bool teb_fs_rename(const char *from, const char *to) {
    uv_fs_t r;
    int rc = uv_fs_rename(NULL, &r, from, to, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

bool teb_fs_copyfile(const char *from, const char *to) {
    uv_fs_t r;
    int rc = uv_fs_copyfile(NULL, &r, from, to, 0, NULL);
    uv_fs_req_cleanup(&r);
    return rc == 0;
}

/* 请求头大小写不敏感查找（返回 info 缓存内借用引用；未找到 TV_NULL） */
TVal teb_header_ci(TObj *o, const char *name) {
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
TArr *teb_parse_urlencoded(const char *s, size_t len) {
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
/* ---- 参数 helper ---- */

const char *teb_arg_str(VM *vm, TVal *args, int n, int i, const char *who) {
    if (n <= i || args[i].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "%s() expects argument %d to be a string", who, i + 1);
    return tstr_val((TStr *)args[i].u.p);
}

int64_t teb_arg_int(VM *vm, TVal *args, int n, int i, const char *who) {
    if (n <= i)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "%s() expects at least %d argument(s)", who, i + 1);
    return tval_to_int(args[i]);
}

/* ---- 类构建 helpers（worker.c 同款模式） ---- */

TClass *teb_cls_new(const char *fqcn) {
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

void teb_cls_add_native(TClass *cls, const char *name, void *fn,
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

void teb_cls_add_ctor(TClass *cls, void *fn) {
    cls->m_ctor = (int16_t)cls->n_methods;
    teb_cls_add_native(cls, "__construct", fn, false);
}

void teb_cls_add_prop(TClass *cls, const char *name) {
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
/* ---- 执行池：配置 blob 快照与回调本地化 ----
 * 发布点（main，mu 内或 start）把回调 serialize 成 C 字节 blob；池线程
 * 按 (节点指针, config_gen) 缓存本地闭包副本（本线程堆/gc_list 独享），
 * 换代整体作废重建。主线程永远直用共享 callable（g_in_pool_thread=false）。 */

/* serialize 一个回调进节点 blob（覆盖前释放旧的）。vm 传当前主线程——
 * 'C' 格式顺带 freeze owner Program + 进程级注册（池线程按 id 解析）。 */
void teb_snapshot_cb(TVal cb, char **blob_out, uint32_t *len_out) {
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
TVal teb_cb_acquire(VM *vm, const void *key, TVal shared,
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
void teb_local_cache_free(void) {
    for (int i = 0; i < g_local_n; i++) tval_release(&g_local_cbs[i].local);
    tm_free(g_local_cbs);
    g_local_cbs = NULL;
    g_local_n = g_local_cap = 0;
    g_local_gen = 0;
}
/* P1-3 同步读整个文件（uv_fs；UTF-8 路径安全）。
 * max_sz = 读取字节上限（Response::file 传 upload_max_bytes、session 传
 * TEB_SESS_MAX_BYTES——耦合 g_teb 会让小上限配置打挂会话读取）。
 * 返回 0 成功（out/outlen 归调用方 tm_free）；1 不存在/不可读；2 超上限。 */
int teb_read_file_max(const char *path, int64_t max_sz,
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
/* ---- 类注册（init 阶段，首脚本编译前）：编排 8 个域注册函数 ---- */

static void teb_register_classes(void) {
    teb_register_web();
    teb_register_request();
    teb_register_response();
    teb_register_stream();
    teb_register_uploadedfile();
    teb_register_websocket();
    teb_register_session();
    teb_register_internal();
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
