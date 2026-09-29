# 图片资源链路与租约

日期：2026-09-29。客户端 DSL 的 `Image("uri", width: ..., height: ...)` 在 Scene 中保存 `ResourceId`。图片检查、PNG 解码和任务预算位于 CPU 准备层；WM 不读取 URI、图片字节或 DisplayList。资源路径的包边界见[客户端 SDK](CLIENT_APP_SDK.md)，完整线程所有权见[迁移设计](CLIENT_RENDER_THREAD_MIGRATION.md)。

`ImageResources::Request` 对同一 URI 复用 ID，并在新请求时分配独立的、非零且单调增长的 UI 资源 generation。检查和有界解码由共享 TaskScheduler 的资源任务执行；完成通知 FD 唤醒 SDK，UI 线程 `Poll()` 后注册图片并调用 `Scene::ImageReady()`。CPU 未就绪时没有 `DrawImage` 命令。CPU 就绪后 Scene 可以生成命令，但该版本的 GPU 上传完成前，提交侧不会提交引用它的像素帧。

`Retain(id)` 返回 `ImageLease = shared_ptr<const DecodedImage>`：别名指针指向解码结果，控制块继续持有任务输出及预算。生产的 Raster/GLES 资源表从同一份不可变 RGBA 数据创建 Skia 图片，资源表和正向 `RegisterImage` 命令都可持有租约；UI 删除 `ImageResources` 条目后，仍有引用的字节不会悬空。直接渲染测试保留独立的复制登记接口。PNG 上限为单张 4096×4096、64 MiB，默认解码预算为 128 MiB；请求、取消和 URI 去重由 `ImageResources` 管理。

`RegisterImage(version, lease)`、`Frame(packet)` 和 `ReleaseImage(version)` 走同一条有界有序命令队列。只有连续且同 UI load 的帧可合并；登记、释放及跨 UI load 帧均为屏障。帧只保存 DisplayList 实际引用图片的 `(ResourceId, UI generation)` 列表，不在每个已提交帧里重复持有全部解码字节。提交侧的 GLES/CPU `ResourceEpoch` 是本地变更计数，不可当作这个跨侧身份。绘制前必须找到匹配版本并确认 GPU 图片已上传；否则保留待处理像素请求。

上传成功后渲染侧按 `ImageVersion` 发送 `ImageUploadedEvent`；UI 消费该有序确认且版本仍为当前资源时，才把它记为可用。确认说明 Skia 已建立真实纹理并提交上传，**不是 GPU 完成栅栏**。上传失败走独立的终态失败信号，不会伪造成功确认。渲染线程按版本去重上传任务，UI 按版本保存已确认结果；重复推进分阶段安装不会重复上传或重复发送确认。已配置窗口的分阶段安装复用渲染线程上传队列，并在收到对应确认后才提交候选 UI；初次尚未配置的窗口仍可先安装 Scene，待 configure 后上传。

释放命令只对完全匹配的 `(ResourceId, generation)` 执行 GLES 注销，并在资源表和渲染侧租约退出后发送 `ImageReleasedEvent`。旧版本释放若晚于新版本登记，只确认旧版本已退役，不会注销新版本。这个确认表示渲染器不再持有或使用该版本，并不表示先前已提交 GPU 工作完成，也不是物理显存回收的栅栏。UI 按完整版本清理待释放记录，旧上传确认不会让已释放或已更新的图片重新变为可用。

当前 UI 生产有序资源命令，独立渲染线程消费并独占 Skia/EGL 和 Wayland；图片强租约跨队列持有，释放确认只在渲染所有者移除该代资源后发送。Close 通过独立终态信号唤醒 worker，在其清理 Ganesh、EGL、Wayland 并确认后释放桥接状态。本地驱动调用永久阻塞时，等待期限不能保证线程可强制终止；此情形需进程级故障策略。测试与 fixture 保留在 `tests/`，不进入客户端生产包。相关 `image_resources_test`、`image_scheduler_test`、`render_command_test` 和 Pi 隔离 V3D 启动探针覆盖 URI/版本、租约、队列屏障、上传确认及释放后重新请求；这些正确性检查不是 GPU 内存或动画 FPS 测量。
