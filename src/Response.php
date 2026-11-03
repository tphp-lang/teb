<?php

namespace Tphp\Teb;

/**
 * 响应
 */
class Response extends Base
{
    private string $body = "";
    private int $status = 200;
    private array $headers = [];
    private array $cookies = [];

    public function __construct(
        string $body = "",
        int $status = 200,
        array $headers = [
            "Content-Type" => " text/plain"
        ]
    ) {
        $this->body = $body;
        $this->status = $status;
        $this->headers = $headers;
    }

    /**
     * 设置http头
     *
     * @param string $key 
     * @param string $value
     * @return self
     */
    public function header(string $key, string $value): self
    {
        $this->headers[$key] = $value;
        return $this;
    }

    /**
     * 设置cookie
     *
     * @param string $key
     * @param string $value
     * @return self
     */
    public function cookie(string $key, string $value): self
    {
        $this->cookies[$key] = $value;
        return $this;
    }

    /**
     * 设置http头
     *
     * @param array $headers
     * @return self
     */
    public function withHeaders(array $headers): self
    {
        $this->headers = $this->headers + $headers;
        return $this;
    }

    /**
     * 设置cookie
     *
     * @param array $cookies
     * @return self
     */
    public function withCookies(array $cookies): self
    {
        $this->cookies = $this->cookies + $cookies;
        return $this;
    }

    /**
     * 发送响应
     *
     * @return string
     */
    public function send(): string
    {
        $resp = "HTTP/1.1 {$this->status} OK\r\n";
        foreach ($this->headers as $key => $value) {
            $resp .= "{$key}: {$value}\r\n";
        }
        $resp .= "Content-Length: " . strlen($this->body) . "\r\n";
        $resp .= "Connection: close\r\n\r\n";
        $resp .= $this->body;
        return $resp;
    }
}
