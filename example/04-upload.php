<?php
// multipart 文件上传：file()/files() + moveTo
// 运行：tphp run example/04-upload.php
// 试：curl -F "f=@README.md" -F "note=手动填的表单字段" http://127.0.0.1:8084/upload
use Tphp\Teb\Web;
use Tphp\Teb\Request;
use Tphp\Teb\Response;

$tmpDir = __DIR__ . "/_up_tmp";
// civetweb 落盘临时目录必须先存在
if (!is_dir($tmpDir)) {
    mkdir($tmpDir, 0777);
}

$web = new Web();

$web->route("/upload", function (Request $r) use ($tmpDir) {
    // form()：multipart 里的文本字段；files()：字段名 => UploadedFile 表
    $form = $r->form();

    // 单文件：第一个文件字段（文件较大时为落盘 UploadedFile，磁盘路径在 tmpDir）
    $f = $r->file("f");
    $files = $r->files();

    if (!$f) {
        return Response::json(["error" => "没有名为 f 的文件字段"], 400);
    }

    $dest = $tmpDir . "/saved-" . $f->name();
    // moveTo 必须在本次请求回调内完成（回调返回后临时文件会被回收）
    $f->moveTo($dest);

    return Response::json([
        "form"      => $form,
        "clientName"=> $f->name(),          // 上传方原始文件名
        "mime"      => $f->type(),          // 按文件名扩展推断的 MIME
        "size"      => $f->size(),
        "isMoved"   => $f->isMoved(),
        "savedTo"   => $dest,
        "totalFiles"=> count($files),       // files()：字段名 => UploadedFile
    ]);
});

echo "04-upload listening on http://127.0.0.1:8084 (tmp: $tmpDir)\n";
$web->start([
    "listening_ports"  => "8084",
    "upload_tmp_dir"   => $tmpDir,
    "upload_max_bytes" => "20000000",   // 超限直接 413
]);
$web->run();
