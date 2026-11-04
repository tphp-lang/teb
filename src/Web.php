<?php

namespace Tphp\Teb;

class Web extends Base
{
    private mixed $ctx = null;
    private int $port = 8080;
    private int $num_threads = 50;

    /**
     * 初始化
     *
     * @param Features $init
     */
    public function __construct(Features $init = Features::Default)
    {
        self::ffi()->mg_init_library($init->value);
    }

    /**
     * 设置选项
     *
     * @param string $doc_root 文档根目录
     * @param int $ports 端口
     * @param int $num_threads 线程数
     * @param int $timeout_ms 超时时间
     * @param string|null $error_log_file 错误日志文件
     * @param string|null $access_log_file 访问日志文件
     * @param string|null $ssl_certificate SSL证书
     * @param string $authentication_domain 认证域名 
     * @param string|null $protect_uri 保护路径
     * @return self
     */
    public function options(
        string $doc_root,
        int $ports = 8080,
        int $num_threads = 50,
        int $timeout_ms = 30000,
        string|null $error_log_file = null,
        string|null $access_log_file = null,
        string|null $ssl_certificate = null,
        string $authentication_domain = "mydomain.com",
        string|null $protect_uri = null
    ): self {
        $num = 11;
        if ($error_log_file !== null) {
            $num += 2;
        }
        if ($access_log_file !== null) {
            $num += 2;
        }
        if ($ssl_certificate !== null) {
            $num += 2;
        }
        if ($protect_uri !== null) {
            $num += 2;
        }
        $c_str_arry = self::ffi()->new("char *[$num]");
        $c_str_arry[0] = "document_root";
        $c_str_arry[1] = $doc_root; // 文档根目录
        $c_str_arry[2] = "listening_ports";
        $c_str_arry[3] = (string)$ports; // 端口
        $c_str_arry[4] = "num_threads";
        $c_str_arry[5] = (string)$num_threads; // 线程数
        $c_str_arry[6] = "request_timeout_ms";   // civetweb 不认 timeout_ms（unknown option 直接 mg_start 失败，2026-11-04 实测）
        $c_str_arry[7] = (string)$timeout_ms; // 超时时间
        $c_str_arry[8] = "authentication_domain";
        $c_str_arry[9] = $authentication_domain; // 认证域名
        if ($error_log_file !== null) {
            $c_str_arry[10] = "error_log_file";
            $c_str_arry[11] = $error_log_file; // 错误日志文件
        }
        if ($access_log_file !== null) {
            $c_str_arry[12] = "access_log_file";
            $c_str_arry[13] = $access_log_file; // 访问日志文件
        }
        if ($ssl_certificate !== null) {
            $c_str_arry[14] = "ssl_certificate";
            $c_str_arry[15] = $ssl_certificate; // SSL证书
        }
        if ($protect_uri !== null) {
            $c_str_arry[16] = "protect_uri";
            $c_str_arry[17] = $protect_uri; // 保护路径
        }
        $c_str_arry[$num - 1] = null;
        $this->ctx = self::ffi()->mg_start(null, null, $c_str_arry);
        if ($this->ctx === null) {
            throw new \Exception("Failed to start Teb server");
        }
        $this->port = $ports;
        $this->num_threads = $num_threads;
        return $this;
    }

    /**
     * 路由
     *
     * @param string $uri 路径
     * @param callable(Request):Response $cb 回调
     * @return self
     */
    public function route(string $uri, callable $cb): self
    {
        $c_cb = function ($conn, $cbdata) use ($cb) {
            $request_info = new Request($conn);
            $resp = $cb($request_info);
            if (!$resp instanceof Response) {
                throw new \Exception("Callback must return an instance of Response");
            }
            $send = $resp->send();
            self::ffi()->mg_write($conn, $send, strlen($send));
            return 1;
        };
        self::ffi()->mg_set_request_handler($this->ctx, $uri, $c_cb, null);
        return $this;
    }

    /**
     * 运行
     * 阻塞常驻
     *
     * @return self
     */
    public function run(): self
    {
        echo "--------------------- TEB ----------------------\n";
        echo "Server running at http://localhost:{$this->port}\n";
        echo "Threads: {$this->num_threads}\n";
        echo "Press Ctrl+C to stop the server\n";
        echo "--------------------- TEB ----------------------\n";
        while (true) {
            sleep(1);
        }
        return $this;
    }

    /**
     * 停止
     *
     * @return void
     */
    public function stop(): void
    {
        if ($this->ctx === null) {
            return;
        }
        self::ffi()->mg_stop($this->ctx);
        self::ffi()->mg_exit_library();
        $this->ctx = null;
    }

    public function __destruct()
    {
        $this->stop();
    }
}
