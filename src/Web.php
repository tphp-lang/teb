<?php

namespace Tphp\Teb;

class Web extends Base
{
    private mixed $ctx = null;
    private int $port = 8080;

    public function __construct(int $init = 0)
    {
        self::ffi()->mg_init_library($init);
    }

    public function start(int $ports = 8080): self
    {
        $c_str_arry = self::ffi()->new("char *[3]");
        $c_str_arry[0] = "listening_ports";
        $c_str_arry[1] = (string)$ports;
        $this->ctx = self::ffi()->mg_start(null, null, $c_str_arry);
        if ($this->ctx === null) {
            throw new \Exception("Failed to start Teb server");
        }
        $this->port = $ports;
        return $this;
    }

    public function request(string $uri, callable $cb): self
    {
        // civetweb 在 worker 线程回调此闭包，经跨线程 marshaling 投递到主线程执行。
        // 契约：$cb 返回响应体字符串；本层拼 HTTP 响应头并 mg_write；
        // 返回 1（已处理）交还 civetweb，否则它按未处理回 404。
        $c_cb = function ($conn, $cbdata) use ($cb) {
            $body = $cb($conn);
            if (!is_string($body)) {
                $body = "";
            }
            $resp = "HTTP/1.1 200 OK\r\n"
                  . "Content-Type: text/plain\r\n"
                  . "Content-Length: " . strlen($body) . "\r\n"
                  . "Connection: close\r\n\r\n"
                  . $body;
            self::ffi()->mg_write($conn, $resp, strlen($resp));
            return 1;
        };
        self::ffi()->mg_set_request_handler($this->ctx, $uri, $c_cb, null);
        return $this;
    }

    // 阻塞常驻
    public function run(): self
    {
        echo "Server running at http://localhost:{$this->port}\n";
        echo "Press Ctrl+C to stop the server\n";
        while (true) {
            sleep(1);
        }
        return $this;
    }

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
