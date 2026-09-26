# 第四步检查点：图片资源链路

依据 [Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md) 第 12 节，客户端 DSL 的 `Image("uri", width: ..., height: ...)` 经语义转换请求 `ResourceId`；Scene 只保存句柄。旧 `.prismb` 节点枚举和格式版本没有扩展，避免把新客户端图片语义耦合进旧生产编译器。

`ImageResources` 的单个后台线程接收 URI，由可注入解码器产生直通 RGBA 数据。它只把结果放入完成队列，不触碰 Scene。运行时线程调用 `Poll()`，检查成功/失败及预算后注册到 Skia，再调用 `Scene::ImageReady()`：显式宽高只触发 Paint；需要图片原生尺寸时触发 Layout 与 Paint。图片尚未完成时没有 `DrawImage` 命令，静止页面也不会持续重建 DisplayList。相同 URI 在同一资源表中复用 ID。

目前解码器只支持 PNG，限制每张图片最大 4096×4096、64 MiB；资源表默认总解码预算 128 MiB，单表最多 4096 个 URI。工作线程最多保留一个待消费的解码结果，再等待运行时线程 `Poll()`，避免完成队列绕过预算无限增长。Skia 渲染器将 RGBA 像素复制为自己的 `SkImage`，以 `ResourceId` 查找并绘制。无效或未注册句柄会在绘制前拒绝整个 DisplayList。后台线程与 Skia 对象之间没有共享可变像素内存。

当前尚未实现资源取消、逐出与 GPU 资源预算、动图、JPEG/WebP、包内 URI 解析、图片缓存重载、损坏资源的占位 UI 和正式 SDK 集成。资源完成现在由诊断事件循环轮询 `Poll()`；正式 SDK 需要把完成通知接入事件循环唤醒机制，避免静态页面定时轮询。生产环境不能直接信任 DSL 指定的任意文件路径；包资源解析与权限限制须在客户端 SDK 接入时完成。测试 fixture 的文件路径仅用于验证。

测试代码与 PNG fixture 位于 `tests/`。`image_resources_test` 覆盖异步完成、URI 去重、失败与 Scene 失效；`skia_raster_test` 检查 PNG 像素和 DrawImage；手工 probe 在 headless WM 上验证完成通知后的重新提交。GPU 与实际显示性能仍需另行实测。
