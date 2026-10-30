<?php

namespace Tphp\Teb;

class Web extends Base
{
    private mixed $ctx;

    public function __construct(int $init = 0)
    {
        self::ffi()->mg_init_library($init);
    }

    public function start(int $ports = 8080): self
    {
        $c_str_arry = self::ffi()->new("char *[2]");
        $c_str_arry[0] = "listening_ports";
        $c_str_arry[1] = (string)$ports;
        $this->ctx = self::ffi()->mg_start(null, null, $c_str_arry);
        if ($this->ctx === null) {
            throw new \Exception("Failed to start Teb server");
        }
        return $this;
    }

    public function request(string $uri, callable $cb): self
    {
        $c_cb = function ($conn, $cbdata) use ($cb) {
            $cb($conn);
        };
        self::ffi()->mg_set_request_handler($this->ctx, $uri, $c_cb, null);
    }

    public function run(): self
    {
        echo "Server running at http://localhost:8080\n";
        echo "Press Ctrl+C to stop the server\n";
        return $this;
    }

    public function stop(): void
    {
        self::ffi()->mg_stop($this->ctx);
        self::ffi()->mg_exit_library();
    }

    public function __destruct()
    {
        if ($this->ctx !== null) {
            $this->stop();
        }
    }
}
