<?php

namespace Tphp\Teb;

use FFI;

abstract class Base
{
    /**
     * FFI 实例
     */
    private static ?FFI $ffi = null;

    /**
     * 获取 FFI 实例
     */
    public static function ffi(): FFI
    {
        if (self::$ffi === null) {
            self::getFFI();
        }
        return self::$ffi;
    }

    /**
     * 按当前平台加载 teb 动态库
     *
     * lib/<系统>/<架构>/teb.<扩展名>
     * - Windows: x86_64
     * - Linux:   x86_64 / aarch64
     * - macOS:   x86_64 / arm64
     *
     * @throws \Exception 平台不受支持或动态库不存在时抛出
     */
    protected static function getFFI(): void
    {
        $header =  dirname(__DIR__) . DIRECTORY_SEPARATOR . "include" . DIRECTORY_SEPARATOR . "teb.h";

        self::$ffi = \FFI::cdef(file_get_contents($header), self::libPath());
    }

    /**
     * 当前平台的动态库路径（AsyncBinder 在 Worker 线程里要自己加载一次）
     *
     * @throws \Exception 平台不受支持或动态库不存在时抛出
     */
    public static function libPath(): string
    {
        $root = dirname(__DIR__);
        $isArm = PHP_UNAME["machine"];

        switch (PHP_OS_FAMILY) {
            case 'Windows':
                // 目前只提供 x86_64 产物，arm64 系统由系统仿真运行
                $dir = 'windows' . DIRECTORY_SEPARATOR . 'x86_64';
                $file = 'teb.dll';
                break;
            case 'Linux':
                $dir = 'linux' . DIRECTORY_SEPARATOR . ($isArm ? 'aarch64' : 'x86_64');
                $file = 'teb.so';
                break;
            case 'Darwin':
                $dir = 'macos' . DIRECTORY_SEPARATOR . ($isArm ? 'arm64' : 'x86_64');
                $file = 'teb.dylib';
                break;
            default:
                throw new \Exception(
                    "不支持的操作系统: " . PHP_OS_FAMILY . "\n"
                        . "teb 目前仅支持 Windows、Linux 和 macOS 操作系统。"
                );
        }

        $lib = $root . DIRECTORY_SEPARATOR . "lib"
            . DIRECTORY_SEPARATOR . $dir
            . DIRECTORY_SEPARATOR . $file;

        if (!is_file($lib)) {
            throw new \Exception(
                "teb 动态库不存在: {$lib}\n"
                    . "请先执行对应平台的构建脚本：source/build.cmd（Windows）、"
                    . "source/linux.sh（Linux）、source/macos.sh（macOS）。"
            );
        }

        return $lib;
    }
}
