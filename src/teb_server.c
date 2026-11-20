/* teb_server.c - 队列/413/405/thunk/WS civetweb 回调/info 物化/dispatch/WS deliver/frag/drain */
#include "teb_impl.h"

_Thread_local int g_teb_drain_depth = 0;   /* 见 teb_impl.h 声明处注释 */

/* ---- 队列入队 + 阻塞等待（thunk 与 WS 回调共用；worker 线程） ---- */

static void teb_req_free(TebRequest *req) {
    if (req->path) tm_free(req->path);
    if (req->ws_data) tm_free(req->ws_data);
    if (req->pvals) {
        for (int i = 0; i < req->n_pvals; i++) tm_free(req->pvals[i]);
        tm_free(req->pvals);
    }
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

    /* 等待 PHP 侧处理完（done）或服务器停止（shutdown 且未被摘走）。
     * taken 的节点必须等 done——消费者已持该 conn，提前放行会双写。 */
    while (!req->done && !(g_teb.shutdown && !req->taken))
        uv_cond_wait(&g_teb.cv, &g_teb.mu);
    bool ok = req->done;
    if (!ok) {
        /* shutdown 且尚未被摘走：把自己从队列摘除 */
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
        if (h->name && h->value && teb_str_icmp(h->name, "Content-Type") &&
            teb_str_icontains(h->value, "multipart/form-data"))
            return true;
    }
    return false;
}

static void teb_write_413(struct mg_connection *conn) {
    teb_lock();
    g_teb.st.requests_total++;   /* 被拒请求同样计数（与 drain 侧同锁） */
    uv_mutex_unlock(&g_teb.mu);
    /* keep-alive 下必须显式关连接：handler 返 1 会触发 civetweb 的
     * discard_unread_request_data，把超大 body 全量读完——413 提前拒绝
     * 的意义尽失；置 must_close 后 discard 被跳过、连接直接关。 */
    mg_disable_connection_keep_alive(conn);
    teb_wr(conn, TEB_413, sizeof TEB_413 - 1);
    teb_stat_resp(413);
}

/* ---- 批A：405 方法不符（thunk 入口直写，镜像 413 的入口口径）----
 * 路径已匹配（T1-T3 有候选）但方法全不符、且无 ANY 路由救场 → 405 + Allow
 * （Allow = 命中候选 methods 并集）。CL:0 无 body → 保持 keep-alive（handler
 * 返 1 后 civetweb discard 无事可丢；与 413 的带 body 拒绝不同）。 */
static const char *TEB_M_NAMES[] = { "GET", "POST", "PUT", "PATCH",
                                     "DELETE", "HEAD", "OPTIONS" };

static void teb_write_405(struct mg_connection *conn, uint32_t allow) {
    teb_lock();
    g_teb.st.requests_total++;   /* 入口拒绝口径与 413 一致 */
    uv_mutex_unlock(&g_teb.mu);
    char allow_buf[128];
    size_t off = 0;
    for (int i = 0; i < 7 && off < sizeof allow_buf - 16; i++) {
        if (allow & (1u << i))
            off += (size_t)snprintf(allow_buf + off, sizeof allow_buf - off,
                                    off ? ", %s" : "%s", TEB_M_NAMES[i]);
    }
    char line[320];
    int ln = snprintf(line, sizeof line,
                      "HTTP/1.1 405 Method Not Allowed\r\n"
                      "Allow: %s\r\nContent-Length: 0\r\n\r\n",
                      off ? allow_buf : "GET");
    teb_wr(conn, line, (unsigned long long)ln);
    teb_stat_resp(405);
}

/* ---- civetweb request handler thunk（同步阻塞，见文件头） ---- */
int teb_req_thunk(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri || !ri->local_uri_raw) return 0;

    /* P1-4 WS 升级请求放行：交回 civetweb 走 websocket handler 握手。
     * 我们的通配 request handler 优先级高于 WS handler，不识别 Upgrade
     * 会把握手请求吞进路由（实测：/ws 落兜底路由回 pong，握手必败）。
     * 普通 GET（无 Upgrade 头）仍走路由/nf。 */
    for (int i = 0; i < ri->num_headers; i++) {
        const struct mg_header *h = &ri->http_headers[i];
        if (h->name && h->value && teb_str_icmp(h->name, "Upgrade") &&
            teb_str_icontains(h->value, "websocket"))
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
        /* 批A：五档匹配（含模板档）+ 方法分发；method_miss → 405（先于
         * 413 预检：先拒方法再谈 body；未入队不跑 before，入口口径计数） */
        TebMatch m;
        teb_match_route(ri->local_uri_raw, teb_method_bit(ri->request_method),
                        &m);
        if (!m.route) {
            if (m.method_miss) {
                teb_write_405(conn, m.allow);
                tm_free(req);
                return 1;
            }
            tm_free(req);   /* 全无命中 → civetweb 默认处理 */
            return 0;
        }
        req->route = m.route;
        req->pvals = m.pvals;
        req->n_pvals = m.n_pvals;
        /* body 超限预检：声明的 Content-Length 已超上限且非 multipart →
         * 413 提前拒绝，省掉无意义的请求体传输（multipart 保留 error=1 语义） */
        if (ri->content_length > g_teb.upload_max_bytes &&
            !ri_is_multipart(ri)) {
            teb_write_413(conn);
            teb_req_free(req);   /* 含 pvals */
            return 1;
        }
    }

    return teb_enqueue_and_wait(req) ? 1 : 0;
}

/* ---- P1-4 WebSocket civetweb 回调（worker 线程：入队 + 阻塞） ---- */

int teb_ws_connect_ok(const struct mg_connection *conn, void *cbdata) {
    (void)conn; (void)cbdata;
    return 0;   /* 放行（握手前拒绝留 P2） */
}

void teb_ws_ready_cb(struct mg_connection *conn, void *cbdata) {
    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->kind = TEQ_WS_OPEN;
    req->conn = conn;
    req->ws_reg = (TebWsReg *)cbdata;
    teb_enqueue_and_wait(req);
}

int teb_ws_data_cb(struct mg_connection *conn, int bits, char *data,
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

void teb_ws_close_cb(const struct mg_connection *conn, void *cbdata) {
    TebRequest *req = tm_malloc(sizeof *req);
    memset(req, 0, sizeof *req);
    req->kind = TEQ_WS_CLOSE;
    req->conn = (struct mg_connection *)conn;   /* drain 内只读比较用 */
    req->ws_reg = (TebWsReg *)cbdata;
    teb_enqueue_and_wait(req);   /* conn 在 close 回调返回后才被 civetweb 释放 */
}
/* ---- 请求信息物化（回调进入时一次性缓存进 Request::$info） ---- */

static TArr *teb_req_info_arr(struct mg_connection *conn, TebRequest *q) {
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

    /* 批A：路径模板参数 → "params" 键（assoc：参数名 → urldecode 值；
     * 名字借 route->segs 的 is_param 段，值在 pvals 按 param 序） */
    if (q && q->kind == TEQ_REQ && q->route && q->route->n_seg &&
        q->pvals) {
        TArr *pa = tarr_new();
        int vi = 0;
        for (int i = 0; i < q->route->n_seg; i++) {
            if (!q->route->segs[i].is_param) continue;
            const char *pn = q->route->segs[i].text;
            TVal k = TV_STR(tstr_intern(pn, strlen(pn)));
            const char *v = (vi < q->n_pvals && q->pvals[vi])
                                ? q->pvals[vi] : "";
            TVal v2 = TV_STR(tstr_new(v, strlen(v)));
            tarr_set(pa, k, v2);
            tval_release(&k);
            vi++;
        }
        TVal pk = TV_STR(tstr_intern("params", 6));
        tarr_set(a, pk, TV_ARR(pa));
        tval_release(&pk);
    }
    return a;
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

/* WS 分片装配 helpers：链表结构走 ws_frag_mu（同连接帧经队列 done 协议
 * 天然串行，节点 buffer 归本连接线程私有——append 免锁） */

static void teb_frag_lock(void) { uv_mutex_lock(&g_teb.ws_frag_mu); }
static void teb_frag_unlock(void) { uv_mutex_unlock(&g_teb.ws_frag_mu); }

static TebWsFrag *teb_ws_frag_find(struct mg_connection *conn) {
    teb_frag_lock();
    TebWsFrag *r = NULL;
    for (TebWsFrag *f = g_teb.ws_frags; f; f = f->next)
        if (f->conn == conn) { r = f; break; }
    teb_frag_unlock();
    return r;
}

static void teb_ws_frag_drop(struct mg_connection *conn) {
    teb_frag_lock();
    for (TebWsFrag **pp = &g_teb.ws_frags; *pp; pp = &(*pp)->next) {
        if ((*pp)->conn == conn) {
            TebWsFrag *f = *pp;
            *pp = f->next;
            teb_frag_unlock();
            tm_free(f->buf);
            tm_free(f);
            return;
        }
    }
    teb_frag_unlock();
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
 * teb_ws_data_cb 醒来后返回 0 → civetweb 断开 */
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
     * 路由/静态全部跳过（外层流式收尾）。
     * 先在 mu 内快照节点指针数组再锁外执行——尾插发布的链表 next 会随
     * 并发注册变化，持锁跨回调也无法安全边走边读；节点到 stop 前不释放。 */
    int n_before = 0;
    teb_lock();
    for (TebRoute *b = g_teb.befores; b; b = b->next) n_before++;
    uv_mutex_unlock(&g_teb.mu);
    TebRoute **bs = n_before
        ? tm_malloc(sizeof(TebRoute *) * (size_t)n_before) : NULL;
    int bi = 0;
    teb_lock();
    for (TebRoute *b = g_teb.befores; b && bi < n_before; b = b->next)
        bs[bi++] = b;
    uv_mutex_unlock(&g_teb.mu);
    n_before = bi;

    for (int i = 0; i < n_before; i++) {
        TebRoute *b = bs[i];
        if (req->props[4].t == T_OBJ) { tm_free(bs); return true; }
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        TVal cb = teb_cb_acquire(vm, b, b->callable, b->cb_blob, b->cb_blob_len);
        bool ok = tphp_vm_call_bounded(vm, cb, &argv, 1, &out);
        tval_release(&cb);
        tval_release(&argv);
        if (!ok) { tm_free(bs); return false; }   /* before 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && teb_is_cls((TObj *)out.u.p, g_resp_cls,
                                         "Tphp\\Teb\\Response")) {
            teb_write_response(conn, (TObj *)out.u.p, req);   /* 短路 */
            tval_release(&out);
            tm_free(bs);
            return true;
        }
        if (out.t != T_NULL) {
            tphp_warn("PHP Warning: Teb before callback must return null "
                      "or a Tphp\\Teb\\Response instance\n");
            tval_release(&out);
            teb_write_500(conn, req);
            tm_free(bs);
            return true;
        }
        tval_release(&out);
    }
    tm_free(bs);
    if (req->props[4].t == T_OBJ) return true;   /* before 内 beginStream */

    if (route) {
        TVal argv = TV_OBJ(req);
        tval_addref(argv);
        TVal out = TV_NULL;
        TVal cb = teb_cb_acquire(vm, route, route->callable,
                                 route->cb_blob, route->cb_blob_len);
        bool ok = tphp_vm_call_bounded(vm, cb, &argv, 1, &out);
        tval_release(&cb);
        tval_release(&argv);
        if (!ok) return false;   /* route 异常：外层按流式状态收尾 */
        if (out.t == T_OBJ && teb_is_cls((TObj *)out.u.p, g_resp_cls,
                                         "Tphp\\Teb\\Response")) {
            teb_write_response(conn, (TObj *)out.u.p, req);
        } else if (req->props[4].t == T_OBJ) {
            /* 流式接管：返回值一律忽略 */
        } else {
            tphp_warn("PHP Warning: Teb route callback must return "
                      "a Tphp\\Teb\\Response instance\n");
            teb_write_500(conn, req);
        }
        tval_release(&out);
        return true;
    }

    if (static_path) {
        /* 会话注入：mg_send_mime_file2 的 additional_headers 挂 Set-Cookie
         * （civetweb 源码零改动；注意 304 分支不透传 → 文档限制） */
        char *cl = teb_sess_cookie_line(teb_sess_of(req));
        mg_send_mime_file2(conn, static_path, NULL, cl);
        if (cl) tm_free(cl);
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
    teb_lock();
    g_teb.st.ws_frames++;
    uv_mutex_unlock(&g_teb.mu);
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
        TVal mcb = teb_cb_acquire(vm, &reg->on_msg, reg->on_msg,
                                  reg->cb_blob[1], reg->cb_blob_len[1]);
        if (!tphp_vm_call_bounded(vm, mcb, args2, 3, &out)) {
            tphp_warn("PHP Warning: Teb WS onMessage callback raised "
                      "(closing connection)\n");
            s->props[1] = TV_TRUE;   /* close_requested → data 回调返回 0 */
        }
        tval_release(&mcb);
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

void teb_drain_mode(VM *vm, int mode) {
    /* mode 0 = 全部节点（主线程 drain/run，含 WS）；mode 1 = 仅 HTTP 节点
     * （池泵专用：WS 回调固定主线程——跨帧/跨连接的 global 状态（房间表等）
     * 与主脚本种子保持同线程，用户"跨回调共享状态用 global"的既有心智不破）。 */
    teb_lock();
    TebRequest *q = NULL;
    bool waited = false;
    for (;;) {
        q = NULL;
        if (mode == 0) {
            q = g_teb.req_head;
            if (q) g_teb.req_head = g_teb.req_tail = NULL;
        } else {
            /* 摘出全部非 WS 节点（保持原序）；WS 留队给主线程 */
            TebRequest *h = NULL, **tp = &h;
            for (TebRequest **pp = &g_teb.req_head; *pp; ) {
                TebRequest *n = *pp;
                if (n->kind == TEQ_REQ || n->kind == TEQ_STATIC) {
                    *pp = n->next;
                    n->next = NULL;
                    *tp = n;
                    tp = &n->next;
                } else {
                    pp = &n->next;
                }
            }
            q = h;
            /* 队尾重算（摘走尾节点时 req_tail 会悬空） */
            g_teb.req_tail = NULL;
            for (TebRequest *r = g_teb.req_head; r; r = r->next)
                if (!r->next) { g_teb.req_tail = r; break; }
        }
        if (q || g_teb.shutdown || waited) break;
        /* 单次泵语义：至多一次 10ms 等待，随后无论有无货都返回——
         * drain() 空队阻塞到"有请求"会卡死所有窗口驱动的用户脚本。 */
        waited = true;
        uv_cond_timedwait(&g_teb.cv, &g_teb.mu, 10 * 1000 * 1000);  /* 10ms */
    }
    /* mu 内整批标记 taken：此后 shutdown 不得提前放行对应 worker */
    for (TebRequest *t = q; t; t = t->next) t->taken = true;
    g_teb_drain_depth++;   /* 线程局部：并发消费者互不清除（见 teb_impl.h） */
    uv_mutex_unlock(&g_teb.mu);

    while (q) {
        TebRequest *next = q->next;

        if (q->kind == TEQ_REQ || q->kind == TEQ_STATIC) {
            teb_lock();
            g_teb.st.requests_total++;   /* 与入口 413 计数（thunk 线程）同锁 */
            g_teb.st.requests_active++;
            uv_mutex_unlock(&g_teb.mu);
            /* 构造回调入参 Request（不走 ctor；消费者持有其 rc）。
             * props: 0=conn 1=info 2=form_cache 3=files_cache 4=stream_obj
             *        5=body_cache 6=qp_cache 7=cookie_cache 8=session */
            TObj *req = tphp_obj_alloc(g_req_cls);
            req->props[0] = TV_INT((int64_t)(uintptr_t)q->conn);
            req->props[1] = TV_ARR(teb_req_info_arr(q->conn, q));
            TVal argv = TV_OBJ(req);
            bool ok = teb_dispatch(vm, req, q->conn,
                                   q->kind == TEQ_REQ ? q->route : NULL,
                                   q->kind == TEQ_STATIC ? q->path : NULL,
                                   q->path_size);

            TVal sv = req->props[4];   /* beginStream 过（before 或 route）？ */
            TObj *stream = (sv.t == T_OBJ &&
                            teb_is_cls((TObj *)sv.u.p, g_stream_cls,
                                       "Tphp\\Teb\\Stream"))
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
                        teb_write_500(q->conn, req);
                    }
                } else if (stream->props[4].t != T_TRUE) {
                    teb_stream_send_headers(stream);   /* 未发头 → 空体响应头 */
                    stream->props[4] = TV_TRUE;
                }
            } else if (!ok) {
                teb_write_500(q->conn, req);   /* 非 流式 + 回调异常 */
            }
            teb_reclaim_tmp(req);      /* 未 moveTo 的上传 tmp 回收 */
            teb_session_flush(req);    /* 会话落盘（dispatch 全分支必经此点） */
            tval_release(&argv);   /* Request 对象（回调捕获则继续存活） */
            teb_lock();
            g_teb.st.requests_active--;
            uv_mutex_unlock(&g_teb.mu);
        } else if (q->kind == TEQ_WS_OPEN) {
            teb_lock();
            g_teb.st.ws_active++;
            uv_mutex_unlock(&g_teb.mu);
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
            rq->props[1] = TV_ARR(teb_req_info_arr(q->conn, q));
            TVal args2[2] = { TV_OBJ(s), TV_OBJ(rq) };
            TVal out = TV_NULL;
            TVal ocb = teb_cb_acquire(vm, &reg->on_open, reg->on_open,
                                      reg->cb_blob[0], reg->cb_blob_len[0]);
            if (!tphp_vm_call_bounded(vm, ocb, args2, 2, &out))
                tphp_warn("PHP Warning: Teb WS onOpen callback raised (ignored)\n");
            tval_release(&ocb);
            tval_release(&out);
            tval_release(&args2[0]);   /* node 持份保留 */
            teb_session_flush(rq);     /* WS 无 HTTP 响应可注入；数据照常落盘 */
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
                        teb_frag_lock();
                        fr->next = g_teb.ws_frags;
                        g_teb.ws_frags = fr;
                        teb_frag_unlock();
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
            teb_lock();
            g_teb.st.ws_active--;
            g_teb.st.ws_closed++;
            uv_mutex_unlock(&g_teb.mu);
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
                TVal ccb = teb_cb_acquire(vm, &reg->on_close, reg->on_close,
                                          reg->cb_blob[2], reg->cb_blob_len[2]);
                if (!tphp_vm_call_bounded(vm, ccb, args2, 1, &out))
                    tphp_warn("PHP Warning: Teb WS onClose callback raised (ignored)\n");
                tval_release(&ccb);
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
    g_teb_drain_depth--;
    bool stop_now = g_teb.pending_stop;
    g_teb.pending_stop = false;
    uv_mutex_unlock(&g_teb.mu);
    if (stop_now && g_teb.ctx) {
        /* 主线程批末 → 直接真停（既有语义）；池线程批末 → 只置信号，
         * 由主线程 run/drain 轮询执行真停（real_stop 会 join 池线程，
         * 在池线程内执行会自 join 死锁）。 */
        uv_thread_t self = uv_thread_self();
        if (uv_thread_equal(&self, &g_teb.main_tid)) {
            teb_real_stop();
        } else {
            teb_lock();
            g_teb.stop_requested = true;
            uv_cond_broadcast(&g_teb.cv);
            uv_mutex_unlock(&g_teb.mu);
        }
    }
}

/* 主线程消费者：全部节点（run/drain/无池模式同路径） */
void teb_drain_once(VM *vm) { teb_drain_mode(vm, 0); }
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
    teb_lock();               /* props[1] 与 data 回调线程的读同锁 */
    o->props[1] = TV_TRUE;    /* close_requested → data 回调返回 0 */
    uv_mutex_unlock(&g_teb.mu);
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

/* ---- 类注册：WebSocket（编排见 teb_core.c teb_register_classes） ---- */
void teb_register_websocket(void) {
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
