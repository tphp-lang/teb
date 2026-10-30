unsigned mg_init_library(unsigned features);
unsigned mg_exit_library(void);
struct mg_context;
struct mg_connection;
struct mg_header
{
    const char *name;  /* HTTP header name */
    const char *value; /* HTTP header value */
};
struct mg_client_cert
{
    void *peer_cert;
    const char *subject;
    const char *issuer;
    const char *serial;
    const char *finger;
};

struct mg_request_info
{
    const char *request_method; /* "GET", "POST", etc */
    const char *request_uri;    /* URL-decoded URI (absolute or relative,
                                 * as in the request) */
    const char *local_uri_raw;  /* URL-decoded URI (relative). Can be NULL
                                 * if the request_uri does not address a
                                 * resource at the server host. */
    const char *local_uri;      /* Same as local_uri_raw, however, cleaned
                                 * so a path like
                                 *   allowed_dir/../forbidden_file
                                 * is not possible. */
    const char *http_version;   /* E.g. "1.0", "1.1" */
    const char *query_string;   /* URL part after '?', not including '?', or
                                   NULL */
    const char *remote_user;    /* Authenticated user, or NULL if no auth
                                   used */
    char remote_addr[48];       /* Client's IP address as a string. */

    long long content_length; /* Length (in bytes) of the request body,
                                 can be -1 if no length was given. */
    int remote_port;          /* Port at client side */
    int server_port;          /* Port at server side (one of the listening
                                 ports) */
    int is_ssl;               /* 1 if HTTPS or WS is used (SSL/TLS used),
                                 0 if not */
    void *user_data;          /* User data pointer passed to mg_start() */
    void *conn_data;          /* Connection-specific user data */

    int num_headers; /* Number of HTTP headers */
    struct mg_header
        http_headers[64]; /* Allocate maximum headers */

    struct mg_client_cert *client_cert; /* Client certificate information */

    const char *acceptedWebSocketSubprotocol; /* websocket subprotocol,
                                               * accepted during handshake */
};
struct mg_response_info
{
    int status_code;          /* E.g. 200 */
    const char *status_text;  /* E.g. "OK" */
    const char *http_version; /* E.g. "1.0", "1.1" */

    long long content_length; /* Length (in bytes) of the request body,
                                 can be -1 if no length was given. */

    int num_headers; /* Number of HTTP headers */
    struct mg_header
        http_headers[64]; /* Allocate maximum headers */
};

struct mg_callbacks
{
    /* Called when civetweb has received new HTTP request.
       If the callback returns one, it must process the request
       by sending valid HTTP headers and a body. Civetweb will not do
       any further processing. Otherwise it must return zero.
       Note that since V1.7 the "begin_request" function is called
       before an authorization check. If an authorization check is
       required, use a request_handler instead.
       Return value:
         0: civetweb will process the request itself. In this case,
            the callback must not send any data to the client.
         1-999: callback already processed the request. Civetweb will
                not send any data after the callback returned. The
                return code is stored as a HTTP status code for the
                access log. */
    int (*begin_request)(struct mg_connection *);

    /* Called when civetweb has finished processing request. */
    void (*end_request)(const struct mg_connection *, int reply_status_code);

    /* Called when civetweb is about to log a message. If callback returns
       non-zero, civetweb does not log anything. */
    int (*log_message)(const struct mg_connection *, const char *message);

    /* Called when civetweb is about to log access. If callback returns
       non-zero, civetweb does not log anything. */
    int (*log_access)(const struct mg_connection *, const char *message);

    /* Called when civetweb initializes SSL library.
       Parameters:
         ssl_ctx: SSL_CTX pointer.
         user_data: parameter user_data passed when starting the server.
       Return value:
         0: civetweb will set up the SSL certificate.
         1: civetweb assumes the callback already set up the certificate.
        -1: initializing ssl fails. */
    int (*init_ssl)(void *ssl_ctx, void *user_data);

    /* Called when civetweb initializes SSL library for a domain.
       Parameters:
         server_domain: authentication_domain from the domain config.
         ssl_ctx: SSL_CTX pointer.
         user_data: parameter user_data passed when starting the server.
       Return value:
         0: civetweb will set up the SSL certificate.
         1: civetweb assumes the callback already set up the certificate.
        -1: initializing ssl fails. */
    int (*init_ssl_domain)(const char *server_domain,
                           void *ssl_ctx,
                           void *user_data);

    /* Called when civetweb is about to create or free a SSL_CTX.
    Parameters:
         ssl_ctx: SSL_CTX pointer. NULL at creation time, Not NULL when
    mg_context will be freed user_data: parameter user_data passed when starting
    the server. Return value: 0: civetweb will continue to create the context,
    just as if the callback would not be present. The value in *ssl_ctx when the
    function returns is ignored. 1: civetweb will copy the value from *ssl_ctx
    to the civetweb context and doesn't create its own. -1: initializing ssl
    fails.*/
    int (*external_ssl_ctx)(void **ssl_ctx, void *user_data);

    /* Called when civetweb is about to create or free a SSL_CTX for a domain.
    Parameters:
         server_domain: authentication_domain from the domain config.
         ssl_ctx: SSL_CTX pointer. NULL at creation time, Not NULL when
    mg_context will be freed user_data: parameter user_data passed when starting
    the server. Return value: 0: civetweb will continue to create the context,
    just as if the callback would not be present. The value in *ssl_ctx when the
    function returns is ignored. 1: civetweb will copy the value from *ssl_ctx
    to the civetweb context and doesn't create its own. -1: initializing ssl
    fails.*/
    int (*external_ssl_ctx_domain)(const char *server_domain,
                                   void **ssl_ctx,
                                   void *user_data);

    /* Called when civetweb is closing a connection.  The per-context mutex is
       locked when this is invoked.

       Websockets:
       Before mg_set_websocket_handler has been added, it was primarily useful
       for noting when a websocket is closing, and used to remove it from any
       application-maintained list of clients.
       Using this callback for websocket connections is deprecated: Use
       mg_set_websocket_handler instead.
    */
    void (*connection_close)(const struct mg_connection *);

    /* Called after civetweb has closed a connection.  The per-context mutex is
       locked when this is invoked.

    Connection specific data:
    If memory has been allocated for the connection specific user data
    (mg_request_info->conn_data, mg_get_user_connection_data),
    this is the last chance to free it.
 */
    void (*connection_closed)(const struct mg_connection *);

    /* init_lua is called when civetweb is about to serve Lua server page.
       exit_lua is called when the Lua processing is complete.
       Both will work only if Lua support is enabled.
       Parameters:
         conn: current connection.
         lua_context: "lua_State *" pointer.
         context_flags: context type information as bitmask:
           context_flags & 0x0F: (0-15) Lua environment type
    */
    void (*init_lua)(const struct mg_connection *conn,
                     void *lua_context,
                     unsigned context_flags);
    void (*exit_lua)(const struct mg_connection *conn,
                     void *lua_context,
                     unsigned context_flags);

    /* Called when civetweb is about to send HTTP error to the client.
       Implementing this callback allows to create custom error pages.
       Parameters:
         conn: current connection.
         status: HTTP error status code.
         errmsg: error message text.
       Return value:
         1: run civetweb error handler.
         0: callback already handled the error. */
    int (*http_error)(struct mg_connection *conn,
                      int status,
                      const char *errmsg);

    /* Called after civetweb context has been created, before requests
       are processed.
       Parameters:
         ctx: context handle */
    void (*init_context)(const struct mg_context *ctx);

    /* Called when civetweb context is deleted.
    Parameters:
    ctx: context handle */
    void (*exit_context)(const struct mg_context *ctx);

    /* Called when a new worker thread is initialized.
     * It is always called from the newly created thread and can be used to
     * initialize thread local storage data.
     * Parameters:
     *   ctx: context handle
     *   thread_type:
     *     0 indicates the master thread
     *     1 indicates a worker thread handling client connections
     *     2 indicates an internal helper thread (timer thread)
     * Return value:
     *   This function returns a user supplied pointer. The pointer is assigned
     *   to the thread and can be obtained from the mg_connection object using
     *   mg_get_thread_pointer in all server callbacks. Note: A connection and
     *   a thread are not directly related. Threads will serve several different
     *   connections, and data from a single connection may call different
     *   callbacks using different threads. The thread pointer can be obtained
     *   in a callback handler, but should not be stored beyond the scope of
     *   one call to one callback.
     */
    void *(*init_thread)(const struct mg_context *ctx, int thread_type);

    /* Called when a worker exits.
     * The parameters "ctx" and "thread_type" correspond to the "init_thread"
     * call. The  "thread_pointer" parameter is the value returned by
     * "init_thread".
     */
    void (*exit_thread)(const struct mg_context *ctx,
                        int thread_type,
                        void *thread_pointer);

    /* Called when initializing a new connection object.
     * Can be used to initialize the connection specific user data
     * (mg_request_info->conn_data, mg_get_user_connection_data).
     * When the callback is called, it is not yet known if a
     * valid HTTP(S) request will be made.
     * Parameters:
     *   conn: not yet fully initialized connection object
     *   conn_data: output parameter, set to initialize the
     *              connection specific user data
     * Return value:
     *   must be 0
     *   Otherwise, the result is undefined
     */
    int (*init_connection)(const struct mg_connection *conn, void **conn_data);
};

struct mg_context *mg_start(const struct mg_callbacks *callbacks,
                            void *user_data,
                            const char **configuration_options);

void mg_stop(struct mg_context *);

int mg_start_domain(struct mg_context *ctx,
                    const char **configuration_options);

void mg_set_request_handler(struct mg_context *ctx,
                            const char *uri,
                            int (*mg_request_handler)(struct mg_connection *conn, void *cbdata),
                            void *cbdata);
struct mg_websocket_subprotocols
{
    int nb_subprotocols;
    const char **subprotocols;
};
void mg_set_websocket_handler(struct mg_context *ctx,
                              const char *uri,
                              int (*mg_websocket_connect_handler)(const struct mg_connection *,
                                                                  void *),
                              void (*mg_websocket_ready_handler)(struct mg_connection *, void *),
                              int (*mg_websocket_data_handler)(struct mg_connection *,
                                                               int,
                                                               char *,
                                                               unsigned long long,
                                                               void *),
                              void (*mg_websocket_close_handler)(const struct mg_connection *,
                                                                 void *),
                              void *cbdata);
void mg_set_websocket_handler_with_subprotocols(
    struct mg_context *ctx,
    const char *uri,
    struct mg_websocket_subprotocols *subprotocols,
    int (*mg_websocket_connect_handler)(const struct mg_connection *,
                                        void *),
    void (*mg_websocket_ready_handler)(struct mg_connection *, void *),
    int (*mg_websocket_data_handler)(struct mg_connection *,
                                     int,
                                     char *,
                                     unsigned long long,
                                     void *),
    void (*mg_websocket_close_handler)(const struct mg_connection *,
                                       void *),
    void *cbdata);

void mg_set_auth_handler(struct mg_context *ctx,
                         const char *uri,
                         int (*mg_authorization_handler)(struct mg_connection *conn,
                                                         void *cbdata),
                         void *cbdata);
const char *mg_get_option(const struct mg_context *ctx,
                          const char *name);
struct mg_context *mg_get_context(const struct mg_connection *conn);
void *mg_get_user_data(const struct mg_context *ctx);
void *mg_get_user_context_data(const struct mg_connection *conn);
void *mg_get_thread_pointer(const struct mg_connection *conn);
void mg_set_user_connection_data(const struct mg_connection *conn, void *data);
void *mg_get_user_connection_data(const struct mg_connection *conn);
int mg_get_request_link(const struct mg_connection *conn, char *buf, unsigned long long buflen);
struct mg_option
{
    const char *name;
    int type;
    const char *default_value;
};
const struct mg_option *mg_get_valid_options(void);

struct mg_server_port
{
    int protocol;    /* 1 = IPv4, 2 = IPv6, 3 = both */
    int port;        /* port number */
    int is_ssl;      /* https port: 0 = no, 1 = yes */
    int is_redirect; /* redirect all requests: 0 = no, 1 = yes */
    int _reserved1;
    int _reserved2;
    int _reserved3;
    int _reserved4;
};

int mg_get_server_ports(const struct mg_context *ctx,
                        int size,
                        struct mg_server_port *ports);
int mg_modify_passwords_file(const char *passwords_file_name,
                             const char *realm,
                             const char *user,
                             const char *password);
int mg_modify_passwords_file_ha1(const char *passwords_file_name,
                                 const char *realm,
                                 const char *user,
                                 const char *ha1);
const struct mg_request_info *mg_get_request_info(const struct mg_connection *);
const struct mg_response_info *mg_get_response_info(const struct mg_connection *);
int mg_write(struct mg_connection *, const void *buf, unsigned long long len);
int mg_websocket_write(struct mg_connection *conn,
                       int opcode,
                       const char *data,
                       unsigned long long data_len);
int mg_websocket_client_write(struct mg_connection *conn,
                              int opcode,
                              const char *data,
                              unsigned long long data_len);
void mg_lock_connection(struct mg_connection *conn);
void mg_unlock_connection(struct mg_connection *conn);
void mg_lock_context(struct mg_context *ctx);
void mg_unlock_context(struct mg_context *ctx);
int mg_send_chunk(struct mg_connection *conn,
                  const char *chunk,
                  unsigned int chunk_len);
void mg_send_file(struct mg_connection *conn, const char *path);
int mg_send_file_body(struct mg_connection *conn,
                      const char *path);
int mg_send_http_ok(struct mg_connection *conn,
                    const char *mime_type,
                    long long content_length);
int mg_send_http_redirect(struct mg_connection *conn,
                          const char *target_url,
                          int redirect_code);
int mg_send_digest_access_authentication_request(struct mg_connection *conn,
                                                 const char *realm);
int mg_check_digest_access_authentication(struct mg_connection *conn,
                                          const char *realm,
                                          const char *filename);
void mg_send_mime_file(struct mg_connection *conn,
                       const char *path,
                       const char *mime_type);
void mg_send_mime_file2(struct mg_connection *conn,
                        const char *path,
                        const char *mime_type,
                        const char *additional_headers);
long long mg_store_body(struct mg_connection *conn,
                        const char *path);
int mg_read(struct mg_connection *, void *buf, unsigned long long len);
const char *mg_get_header(const struct mg_connection *,
                          const char *name);
int mg_get_var(const char *data,
               unsigned long long data_len,
               const char *var_name,
               char *dst,
               unsigned long long dst_len);
int mg_get_var2(const char *data,
                unsigned long long data_len,
                const char *var_name,
                char *dst,
                unsigned long long dst_len,
                unsigned long long occurrence);
int mg_split_form_urlencoded(char *data,
                             struct mg_header *form_fields,
                             unsigned num_form_fields);
int mg_get_cookie(const char *cookie,
                  const char *var_name,
                  char *buf,
                  unsigned long long buf_len);
void mg_close_connection(struct mg_connection *conn);
struct mg_form_data_handler
{
    /* This callback function is called, if a new field has been found.
     * The return value of this callback is used to define how the field
     * should be processed.
     *
     * Parameters:
     *   key: Name of the field ("name" property of the HTML input field).
     *   filename: Name of a file to upload, at the client computer.
     *             Only set for input fields of type "file", otherwise NULL.
     *   path: Output parameter: File name (incl. path) to store the file
     *         at the server computer. Only used if FORM_FIELD_STORAGE_STORE
     *         is returned by this callback. Existing files will be
     *         overwritten.
     *   pathlen: Length of the buffer for path.
     *   user_data: Value of the member user_data of mg_form_data_handler
     *
     * Return value:
     *   The callback must return the intended storage for this field
     *   (See FORM_FIELD_STORAGE_*).
     */
    int (*field_found)(const char *key,
                       const char *filename,
                       char *path,
                       unsigned long long pathlen,
                       void *user_data);

    /* If the "field_found" callback returned FORM_FIELD_STORAGE_GET,
     * this callback will receive the field data.
     *
     * Parameters:
     *   key: Name of the field ("name" property of the HTML input field).
     *   value: Value of the input field.
     *   user_data: Value of the member user_data of mg_form_data_handler
     *
     * Return value:
     *   The return code determines how the server should continue processing
     *   the current request (See MG_FORM_FIELD_HANDLE_*).
     */
    int (*field_get)(const char *key,
                     const char *value,
                     unsigned long long valuelen,
                     void *user_data);

    /* If the "field_found" callback returned FORM_FIELD_STORAGE_STORE,
     * the data will be stored into a file. If the file has been written
     * successfully, this callback will be called. This callback will
     * not be called for only partially uploaded files. The
     * mg_handle_form_request function will either store the file completely
     * and call this callback, or it will remove any partial content and
     * not call this callback function.
     *
     * Parameters:
     *   path: Path of the file stored at the server.
     *   file_size: Size of the stored file in bytes.
     *   user_data: Value of the member user_data of mg_form_data_handler
     *
     * Return value:
     *   The return code determines how the server should continue processing
     *   the current request (See MG_FORM_FIELD_HANDLE_*).
     */
    int (*field_store)(const char *path, long long file_size, void *user_data);

    /* User supplied argument, passed to all callback functions. */
    void *user_data;
};
int mg_handle_form_request(struct mg_connection *conn,
                           struct mg_form_data_handler *fdh);
int mg_start_thread(void *(*mg_thread_func_t)(void *), void *p);
const char *mg_get_builtin_mime_type(const char *file_name);
const char *mg_get_response_code_text(const struct mg_connection *conn, int response_code);
const char *mg_version(void);
int mg_url_decode(const char *src,
                  int src_len,
                  char *dst,
                  int dst_len,
                  int is_form_url_encoded);
int mg_url_encode(const char *src, char *dst, unsigned long long dst_len);
int mg_base64_encode(const unsigned char *src,
                     unsigned long long src_len,
                     char *dst,
                     unsigned long long *dst_len);
int mg_base64_decode(const char *src,
                     unsigned long long *src_len,
                     unsigned char *dst,
                     unsigned long long **dst_len);
struct mg_match_element
{
    const char *str;        /* First character matching wildcard */
    unsigned long long len; /* Number of character matching wildcard */
};

struct mg_match_context
{
    int case_sensitive;                /* Input: 1 (case sensitive) or 0 (insensitive) */
    unsigned long long num_matches;    /* Output: Number of wildcard matches returned. */
    struct mg_match_element match[32]; /* Output */
};
int mg_strcasecmp(const char *s1, const char *s2);
int mg_strncasecmp(const char *s1, const char *s2, unsigned long long len);
struct mg_connection *
mg_connect_websocket_client(const char *host,
                            int port,
                            int use_ssl,
                            char *error_buffer,
                            unsigned long long error_buffer_size,
                            const char *path,
                            const char *origin,
                            int (*mg_websocket_data_handler)(struct mg_connection *,
                                                             int,
                                                             char *,
                                                             unsigned long long,
                                                             void *),
                            void (*mg_websocket_close_handler)(const struct mg_connection *,
                                                               void *),
                            void *user_data);
struct mg_connection *mg_connect_websocket_client_extensions(const char *host,
                                                             int port,
                                                             int use_ssl,
                                                             char *error_buffer,
                                                             unsigned long long error_buffer_size,
                                                             const char *path,
                                                             const char *origin,
                                                             const char *extensions,
                                                             int (*mg_websocket_data_handler)(struct mg_connection *,
                                                                                              int,
                                                                                              char *,
                                                                                              unsigned long long,
                                                                                              void *),
                                                             void (*mg_websocket_close_handler)(const struct mg_connection *,
                                                                                                void *),
                                                             void *user_data);
struct mg_connection *mg_connect_client(const char *host,
                                        int port,
                                        int use_ssl,
                                        char *error_buffer,
                                        unsigned long long error_buffer_size);
struct mg_client_options
{
    const char *host;
    int port;
    const char *client_cert;
    const char *server_cert;
    const char *host_name;
};
struct mg_connection *mg_connect_client_secure(const struct mg_client_options *client_options,
                                               char *error_buffer,
                                               unsigned long long error_buffer_size);
struct mg_connection *mg_connect_websocket_client_secure(
    const struct mg_client_options *client_options,
    char *error_buffer,
    unsigned long long error_buffer_size,
    const char *path,
    const char *origin,
    int (*mg_websocket_data_handler)(struct mg_connection *,
                                     int,
                                     char *,
                                     unsigned long long,
                                     void *),
    void (*mg_websocket_close_handler)(const struct mg_connection *,
                                       void *),
    void *user_data);
struct mg_connection *mg_connect_websocket_client_secure_extensions(
    const struct mg_client_options *client_options,
    char *error_buffer,
    unsigned long long error_buffer_size,
    const char *path,
    const char *origin,
    const char *extensions,
    int (*mg_websocket_data_handler)(struct mg_connection *,
                                     int,
                                     char *,
                                     unsigned long long,
                                     void *),
    void (*mg_websocket_close_handler)(const struct mg_connection *,
                                       void *),
    void *user_data);
int mg_get_response(struct mg_connection *conn,
                    char *ebuf,
                    unsigned long long ebuf_len,
                    int timeout);
int mg_response_header_start(struct mg_connection *conn,
                             int status);
int mg_response_header_add(struct mg_connection *conn,
                           const char *header,
                           const char *value,
                           int value_len);
int mg_response_header_add_lines(struct mg_connection *conn,
                                 const char *http1_headers);
int mg_response_header_send(struct mg_connection *conn);
unsigned mg_check_feature(unsigned feature);
int mg_get_system_info(char *buffer, int buflen);
int mg_get_context_info(const struct mg_context *ctx, char *buffer, int buflen);
void mg_disable_connection_keep_alive(struct mg_connection *conn);
struct mg_error_data
{
    unsigned code;                       /* error code (number) */
    unsigned code_sub;                   /* error sub code (number) */
    char *text;                          /* buffer for error text */
    unsigned long long text_buffer_size; /* size of buffer of "text" */
};
struct mg_init_data
{
    const struct mg_callbacks *callbacks; /* callback function pointer */
    void *user_data;                      /* data */
    const char **configuration_options;
};
struct mg_context *mg_start2(struct mg_init_data *init,
                             struct mg_error_data *error);
int mg_start_domain2(struct mg_context *ctx,
                     const char **configuration_options,
                     struct mg_error_data *error);