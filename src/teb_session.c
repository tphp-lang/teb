/* teb_session.c - sid/fs/GC/open/flush/cookie_line/Session 原生/nm_req_session */
#include "teb_impl.h"

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
TObj *teb_sess_of(TObj *req) {
    if (!req) return NULL;
    return (req->props[8].t == T_OBJ) ? (TObj *)req->props[8].u.p : NULL;
}

/* Set-Cookie 整行（含 \r\n，直接写出）：destroyed → 清除形态（Max-Age=0）。
 * tm_malloc，调用方 tm_free；无会话返回 NULL。 */
char *teb_sess_cookie_line(TObj *sess) {
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
void teb_session_flush(TObj *req) {
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
TVal nm_req_session(VM *vm, void *self, TVal *args, int n) {
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
    const char *k = teb_arg_str(vm, args, n, 0, who);
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

/* ---- 类注册：Session（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_session(void) {
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
}
