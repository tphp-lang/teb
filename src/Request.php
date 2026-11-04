<?php

namespace Tphp\Teb;

/**
 * 请求
 */
class Request extends Base
{
    private mixed $info = null;

    public string $version = ""; // HTTP 版本号

    public function __construct(mixed $conn)
    {
        $this->info = self::ffi()->mg_get_request_info($conn);
        $this->version = $this->info->http_version;
    }

    /**
     * 获取请求方法
     *
     * @return string GET/POST/PUT/DELETE
     */
    public function method(): string
    {
        return $this->info->request_method;
    }

    /**
     * 获取请求路径
     *
     * @return string
     */
    public function uri(): string
    {
        return $this->info->request_uri;
    }

    /**
     * 获取本地请求路径
     * 例如：/index.php
     * 
     * @return string
     */
    public function localUri(): string
    {
        return $this->info->local_uri_raw;
    }

    /**
     * 获取get参数
     *
     * @param string $name
     * @param mixed $default
     * @return mixed
     */
    public function get(string $name = "", mixed $default = null): mixed
    {
        return $this->info->query_string;
    }

    /**
     * 获取客户端IP地址
     *
     * @return string
     */
    public function remoteAddr(): string
    {
        $a = $this->info->remote_addr;
        $n = \FFI::sizeof($a);
        $s = "";
        for ($i = 0; $i < $n; $i++) {
            $b = $a[$i];               // INT8 元素 → int
            if ($b === 0) break;       // NUL 结尾
            $s .= chr($b & 0xFF);      // int8 有符号，掩到 0..255
        }
        return $s;
    }
}
