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
            $send = $resp->send();
            self::ffi()->mg_write($conn, $send, strlen($send));
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
