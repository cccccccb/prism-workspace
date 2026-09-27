# 客户端 SDK：DSL 到 Wayland 窗口

日期：2026-09-26。当前运行时主题接口见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)；本轮测试与部署结果另行记录。

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；SDK 在客户端进程内持有 Scene、图片资源、字体整形、Skia Ganesh GLES、EGL 窗口和 Wayland 对象。WM 不接触 DSL、Scene 或应用 `$slot`。

平台 host 在 `Open` 前调用 `ApplyTheme` 安装初始快照，随后解析 UI 并创建窗口；在事件循环中调用 `Pump`。业务状态通过带类型的 `SetBinding` 更新，文字可用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。`Pump` 合并 Wayland 输入/configure、图片完成通知与调用者 FD 等待；资源就绪后在运行时线程更新 Scene。静止页面不自行连续提交。`Close` 按 Skia → EGL → Wayland 的顺序释放图形对象。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

`tests/probes/skia_gles_wayland_probe.cpp` 使用同一 SDK 生命周期，测试和诊断程序留在 tests，不安装到生产包。早期 GLES 检查点的计数与验收记录见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md)，不作为当前主题功能的验证结论。

五个生产应用均由统一 host 加业务模块/DSL 包运行，旧独立入口、音乐 exec 适配器和 ImGui 生产路径已删除。SDK 使用 xdg-shell；Shell 角色由 launcher 与 WM 的可信控制通道按真实进程登记，普通 app_id 不授予权限。背景 blur 通过通用 surface effects 契约请求 compositor；SDK 绘制 tint 与客户端内容。Slider 拖动语义尚未实现。空 socket 使用当前 `WAYLAND_DISPLAY`；包入口使用严格 assets 根解析，`LoadUiSource` 的模板查找只服务直接 SDK 调用场景。

## 统一运行时与启动目标

统一 host 的初始化分为 PrepareFrontend、初始主题安装、Bind 与 configure 后 EGL。应用以 DSL/资源和版本化业务模块提供状态/动作。ClientApplication 支持同 surface 的 ReplaceUi 和严格 assets 根解析。SetBinding 返回值表示值已被接受，重复相同值不触发重绘；底层 Scene 的 SetBinding 返回值仍表示是否发生变更。实际 FirstPresented 使用 PresentationCount，兼容 PresentedCount 仍仅计 swap。实现与约束见 [APP_HOST_RUNTIME.md](APP_HOST_RUNTIME.md)。

`prism_launch_client` 与 host 模块 Launch API 使用同一个 launcher。待命 worker 已完成公共 CPU 前端准备，收到并确认当前 ThemeSnapshot 后才分配应用；EGL/GPU 仍在绑定窗口后初始化。池规范见 [LAUNCHER_WORKER_POOL.md](LAUNCHER_WORKER_POOL.md)，生产会话与授权规则见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。

## 运行时主题

`ApplyTheme(const ThemeSnapshot&, std::string* diagnostic = nullptr)` 可在 Open 前安装初始主题，也可更新当前 Scene。SDK 保留当前快照供后续 Preview/Master 的 ReplaceUi 使用；切换主题本身不调用 ReplaceUi、不重建 Scene、不关闭 surface/EGL 或业务模块。`ThemeGeneration()` 返回已接受快照的 generation。

返回 true 表示已接受，包括相同合法快照；返回 false 时，旧主题和 Scene 保持。Scene 会先验证引用、布局、效果区域与输入轮廓，再更新节点样式，按实际属性差异请求状态或像素更新；仅身份变化且解析样式相同不请求重绘。`@token` 是保留的主题引用，`material` 选择通用材料，`inputShape` 明确控制透明材料的输入范围。接口与优先级见 [CLIENT_SCENE_RUNTIME.md](CLIENT_SCENE_RUNTIME.md)。

Controls 的变化目前统一保守标记 Paint，即使变化的控件样式未被可见内容使用。新身份或 generation 的候选主题仍执行隔离布局、文字整形及效果/输入预检；这部分 CPU 工作不计入已提交 Scene 的工作计数。零 GPU/Swap 不表示主题安装零 CPU 工作。

下一次提交将同一 Scene 的 DisplayList、SurfaceEffects 和 InputRegions 按各自失效应用于该 surface；纯效果/输入状态复用现有 buffer，不调用 Skia。WM 不接收 UI 语法或业务 binding。全局主题包由 launcher 编译与分发，SDK 不读取第二份 JSON 或静态样式头；包、ACK、失败恢复和跨进程呈现边界见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。应用模块仅通过可选 `select_theme` 请求包 ID，并用 `on_theme_event` 接收实际结果。

0.1.0-5 的物理 Pi 外观与输入验收属于此前静态视觉版本，见 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)；本轮运行时主题的测试与实机结果不能由该记录替代。

### 独立明暗配色

`select_color_scheme("light"|"dark")` 是 Host API 的可选尾字段；主题事件的可选 `color_scheme` 尾字段报告实际配色。主题 ID 请求保留配色，配色请求保留主题 ID。旧事件前缀仍合法，缺少配色尾字段时按 dark 处理；读取前必须检查 `struct_size` 是否覆盖完整字段。界面选中态由平台确认事件更新，不根据点击猜测成功。Preferences 的监控及采样选项属于业务模块，不进入 renderer 或 WM。

### GPU 缓存预算

`ClientConfig::gpu_resource_cache_bytes` 是可选资源策略，统一 host 可通过同名 `HostConfig` 选项转交。未指定时采用 GLES renderer 的默认 32 MiB Ganesh 缓存预算；直接使用后端可传 `GlesRendererOptions`。该值独立于应用 ID、材质和 light/dark，不能写入主题 DSL 控制渲染器缓存。它是可预算资源的软预算，EGL、驱动、在用或非预算资源可超出；不代表全部 DRM resident 的硬上限，也不执行 glFinish 或逐帧 purge。

### 渲染工作计数

`ClientApplication::GetRenderStats()` 返回累计 `ClientRenderStats`：Scene Build 调用/实际新列表/布局次数，GPU Render 与 Swap 尝试/成功，以及 frame callback 次数。跨 Preview/Master UI 替换累计；主题的隔离预检候选不计为实际前端工作。计数只读，不影响 dirty 或提交行为，不向 WM 发送 DSL/Skia 信息。GPU 调用成功及 Swap 成功不代表实际呈现或 GPU 完成；完整计数和调度规范见 [RENDER_SCHEDULING_AND_INVALIDATION.md](RENDER_SCHEDULING_AND_INVALIDATION.md)。


### 提交与事件等待

`Pump(timeout_ms, std::span<pollfd> wake_fds = {})` 允许调用者提供借用的控制/退出 FD。负 timeout 为无限等待，非负为等待上限；描述符由调用者持有，SDK 只回填 `revents`。SDK 内部加入 `ImageResources::CompletionFd()`，由资源队列负责 drain。所有 Wayland prepare/read/cancel 配对归平台适配器；派发已有事件后返回 Host 重新计算等待源与业务 deadline。

平台提交采用 `None / State / Pixels / Failed` 与 prepare/commit/completion 三个阶段。仅 Pixels 申请帧回调和 presentation feedback；State 不受旧像素 callback 阻塞。Scene 的 `PixelsRevision()` 与 Build generation 独立，纯 Composite 不 Build；首帧/resize 仍允许强制绘制。重复值和仅身份变化的合法主题不推进像素版本。

对应 metadata 随 State/Pixels 成功提交后调用 `AcknowledgeComposite()`。准备阶段已检查 metadata、请求与上次相同，或可选效果扩展不可用而没有请求可发送时，checked-identical `None` 也可确认 Composite，不额外制造 surface commit。因旧像素 callback 尚未完成而推迟准备的 `None` 不确认状态；待处理像素版本和 Composite 继续保留。ACK 只清 Composite，未提交的 Paint/Layout 不受影响；Failed 不确认内容。

新增计数为 `surface_noops`、`surface_state_commits`、`surface_pixel_commits`、`surface_submission_failures`；它们和 GPU/Swap/实际呈现独立。`FrameCallbackPending()` 提供只读节流状态，不使调用者拥有回调对象。提交失败终止当前连接，不能用销毁客户端 callback 代理来宣称撤回服务端 pending 请求。精确生命周期与验证规范见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 9 节。

### 像素损伤与 buffer 修复

Pixels 准备比较上一成功提交的完整 DisplayList/资源版本与当前列表，产生内容损伤；使用平台实际 buffer age 合并有界成功历史，得到当前 buffer 的修复区域。age 0/未知、首次/尺寸/WSI 变化、历史不足和无法证明的绘制范围全量回退。文字、图标、阴影按实际 Skia ink 计算；结构、clip、非单位 transform 和资源版本变化保守全量。没有增加节点增量布局或显示列表分块缓存。

平台顺序为 QueryBufferAge → SetDamage(repair) → Render → Swap(content)。renderer 清理并按原序列绘制精确修复并集，保留区域外内容；提交时声明内容损伤，不能把扩大后的历史修复误传给 compositor。Swap 成功后 move 更新预分配历史和列表，State/None 不推进，失败使历史失效并清理。EGL 能力不可用时继续同一后端的完整修复/普通 Swap。当前逻辑与 buffer 坐标为 1:1，非 1 scale 暂全量回退。

`ClientConfig::partial_rendering` 默认 true；false 保持同一 renderer、时钟/业务和内容损伤，只强制完整像素修复，可用于策略回退与对照。该 C++ 配置不改变业务模块 ABI v1。`ClientRenderStats` 新增 `full_pixel_repairs/partial_pixel_repairs/empty_pixel_repairs/pixel_repair_pixels`，它们在成功 Render 后计数；`content_damage_pixels/damage_history_commits` 在成功 Swap 后计数。面积为 clip/union 后的 buffer 像素范围，不是实际 fragment 数或 GPU 耗时。`buffer_age_queries/unknown_buffer_ages/last_buffer_age` 是平台方法请求，-1 表示不可用，0 表示内容未知；能力字段为 `buffer_age_supported/swap_damage_supported/partial_update_supported`。与已有 Render/Swap/实际 presentation 计数分开解读。

公开局部 Render overload 不得扩大调用者已声明的修复域，面积/碎片导致的 Full 策略必须在 SetDamage 之前由 producer 决定。执行规范与验证结果见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 11 节及后续结果。

`buffer_age_supported` 表示可安全使用的保留能力：EXT buffer age，或可实际调用 SetDamage 的 KHR partial update；只有扩展名称而无法声明修复区域不能启用局部绘制。生产 GLES 直接裁剪重放。CPU raster 的 partial Render 为保持软件 AA 的字节一致性，采用完整临时重放后仅复制修复区域的保守回退，不计为 CPU 局部绘制优化。
