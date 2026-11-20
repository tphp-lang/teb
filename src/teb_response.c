/* teb_response.c - 写出机制/Response 原生/Stream 原生 */
#include "teb_impl.h"

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
void teb_write_response(struct mg_connection *conn, TObj *r, TObj *req) {
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

void teb_write_500(struct mg_connection *conn, TObj *req) {
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
void teb_stream_send_headers(TObj *s) {
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
    const char *k = teb_arg_str(vm, args, n, 0, "Response::header");
    const char *v = teb_arg_str(vm, args, n, 1, "Response::header");
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
    const char *k = teb_arg_str(vm, args, n, 0, "Response::cookie");
    const char *v = teb_arg_str(vm, args, n, 1, "Response::cookie");
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
            if (teb_str_icmp(kk, "path")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): path must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (strchr(s, '\r') || strchr(s, '\n'))
                    tphp_vm_raise(vm, "ValueError", "Response::cookie(): path must not contain CR/LF");
                c_path = s;
            } else if (teb_str_icmp(kk, "domain")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): domain must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (strchr(s, '\r') || strchr(s, '\n'))
                    tphp_vm_raise(vm, "ValueError", "Response::cookie(): domain must not contain CR/LF");
                c_domain = s;
            } else if (teb_str_icmp(kk, "expires")) {
                if (v.t != T_INT)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): expires must be an int");
                c_exp = v.u.i; has_exp = true;
            } else if (teb_str_icmp(kk, "max_age")) {
                if (v.t != T_INT)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): max_age must be an int");
                c_ma = v.u.i; has_ma = true;
            } else if (teb_str_icmp(kk, "secure")) {
                if (v.t != T_TRUE && v.t != T_FALSE)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): secure must be a bool");
                c_secure = (v.t == T_TRUE);
            } else if (teb_str_icmp(kk, "httponly")) {
                if (v.t != T_TRUE && v.t != T_FALSE)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): httponly must be a bool");
                c_http = (v.t == T_TRUE);
            } else if (teb_str_icmp(kk, "sameSite") || teb_str_icmp(kk, "same_site")) {
                if (v.t != T_STR)
                    tphp_vm_raise(vm, "TypeError", "Response::cookie(): sameSite must be a string");
                const char *s = tstr_val((TStr *)v.u.p);
                if (!teb_str_icmp(s, "Lax") && !teb_str_icmp(s, "Strict") && !teb_str_icmp(s, "None"))
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
    int64_t code = teb_arg_int(vm, args, n, 0, "Response::status");
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
    const char *url = teb_arg_str(vm, args, n, 0, "Response::redirect");
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
/* Response::file(string $path, int $status = 200, array $headers = []): Response
 * 读整个文件入 body（超 upload_max_bytes / 不存在 / 不可读 → Error）；
 * Content-Type 按扩展名推断（用户 headers 可覆盖） */
static TVal nm_resp_file(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    const char *path = teb_arg_str(vm, args, n, 0, "Response::file");
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
    int64_t code = teb_arg_int(vm, args, n, 0, "Stream::status");
    o->props[1] = TV_INT(code);
    tval_addref(TV_OBJ(o));
    return TV_OBJ(o);
}

/* header(string $k, string $v): self —— 同上 */
static TVal nm_stream_header(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (o->props[3].t == T_TRUE || o->props[4].t == T_TRUE)
        tphp_vm_raise(vm, "Error", "Tphp\\Teb\\Stream::header(): headers already sent");
    const char *k = teb_arg_str(vm, args, n, 0, "Stream::header");
    const char *v = teb_arg_str(vm, args, n, 1, "Stream::header");
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

/* ---- 类注册：Response / Stream（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_response(void) {
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
}

void teb_register_stream(void) {
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
}
