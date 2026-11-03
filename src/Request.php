<?php

namespace Tphp\Teb;

class Request extends Base
{
    private mixed $info = null;

    public function __construct(mixed $conn)
    {
        $this->info = self::ffi()->mg_get_request_info($conn);
    }

    public function method(): string
    {
        return $this->info->request_method;
    }
}
