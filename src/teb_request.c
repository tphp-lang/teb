/* teb_request.c - body/form/上传/reclaim/cookies/Request 原生(含 routeParam)/UploadedFile */
#include "teb_impl.h"

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
void teb_reclaim_tmp(TObj *req) {
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
/* ---- Tphp\Teb\Request 原生方法（信息读 props[1] 缓存） ---- */

/* 用户不得手动构造（drain 内部经 tphp_obj_alloc 绕过） */
static TVal nm_req_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)args; (void)n;
    tphp_vm_raise(vm, "Error",
                  "Tphp\\Teb\\Request cannot be constructed directly "
                  "(instances are provided to route callbacks)");
}

/* 取 info 缓存键值（借用引用） */
TVal teb_info_get(TObj *o, const char *key) {
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
    const char *name = teb_arg_str(vm, args, n, 0, "Request::header");
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
    const char *k = teb_arg_str(vm, args, n, 0, "Request::queryParam");
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
    const char *k = teb_arg_str(vm, args, n, 0, "Request::routeParam");
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
TArr *teb_parse_cookies(TObj *o) {
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
    const char *k = teb_arg_str(vm, args, n, 0, "Request::cookie");
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
    return teb_str_icontains(tstr_val((TStr *)v.u.p), "json") ? TV_TRUE : TV_FALSE;
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
    } else if (teb_str_icontains(ctype, "multipart/form-data") &&
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
    const char *name = teb_arg_str(vm, args, n, 0, "Request::file");
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
    const char *dest = teb_arg_str(vm, args, n, 0, "UploadedFile::moveTo");
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

/* ---- 类注册：Request / UploadedFile（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_request(void) {
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
}

void teb_register_uploadedfile(void) {
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
