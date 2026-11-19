<?php
// 批B fixture：池内 autoload 的懒加载类（主脚本绝不提前使用 → start 前
// 不进 master/类表，池线程 class miss 必须经 spl_autoload 快照加载）
class PoolLazyCls
{
    public function tag(): string
    {
        return "lazy-ok";
    }
}
