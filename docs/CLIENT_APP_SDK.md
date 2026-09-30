# 客户端 SDK：DSL 到 Wayland 窗口

日期：2026-09-29。当前运行时主题接口见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)；渲染线程切换与 Pi 功能验收见 [CLIENT_RENDER_THREAD_MIGRATION.md](CLIENT_RENDER_THREAD_MIGRATION.md#第二步线程切换验收记录)。

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；客户端进程内的 UI 线程持有 Scene、CPU 图片资源与字体整形，独立渲染线程持有 Skia Ganesh GLES、EGL、Wayland 窗口及 GPU 图片。WM 不接触 DSL、Scene 或应用 `$slot`。

2026-09-29 的资源边界拆分后，Scene 使用独立的 `runtime::TextShaper` 完成 FreeType/HarfBuzz 整形；CPU 回放/损伤分析用的 `RasterRenderer` 和生产 GPU `GlesRenderer` 各自持有 Skia 字体/图片资源表，共享带类型的解码图片强租约。PNG 检查与解码由 `runtime::png_codec` 在 CPU 准备层提供。图片登记获得 UI 侧单调版本，帧只记录实际引用的图片版本；各表当前使用同一配置字体路径及本地字体 ID，字体路径在 ConfigureWindow 前固定并同时复制给 UI 与渲染所有者；当前不支持运行期换字体，字体 ID 由同一配置初始化。

直接调用 `RasterRenderer::Shape`、`InspectPng` 或 `DecodePng` 的 C++ 程序需改用 `runtime::TextShaper` 与 `runtime::png_codec`；这些旧 C++ 方法已移除。直接构造 `GlesRenderer` 的程序改为传字体路径，并在该 renderer 上登记图片后再上传；它不再从 `RasterRenderer` 读取图片。统一 Host 的业务模块 C ABI 不受此接口拆分影响。

当前 Scene 到提交阶段经过只读 `FramePacket`：UI 的 `Pump` 轮次构建并将帧放入有界正向队列，Build 的 DisplayList 移入共享存储，未变化时复用；图片登记/释放与帧按序交付，控制消息不能被帧合并越过。连续变更绑定、图片资源或主题时，已发布但未提交的候选通过不可合并的 `InvalidateFrameCommand` 作废，更新后的 Scene 在本次或下一次 UI `Pump` 集中封包；已成功提交的像素与损伤基线仍保留。区域替换旧图片时，失效屏障先于旧图片释放命令入队，防止 worker 消费仍引用该版本的候选。Wayland 输入、提交结果、呈现反馈和图片版本确认通过同一有界反向事件队列回到 UI，按协议顺序处理命中与 Host 里程碑；提交回调只消费已交付的包，无兼容包时保留请求，configure 状态仍可单独提交。损伤始终相对最后成功的像素包计算。`Pump` 只在 UI/Host 线程轮询资源、反向事件与 Host FD；独立渲染线程持有 Wayland、EGL、Ganesh、GPU 图像、上传及损伤历史。输入采用递增序号与 UI 处理确认，渲染线程在确认前不提交旧帧。所有权及关闭握手见[客户端渲染线程迁移设计](CLIENT_RENDER_THREAD_MIGRATION.md)。

平台 host 在 `Open` 前调用 `ApplyTheme` 安装初始快照，随后解析 UI 并创建窗口；在事件循环中调用 `Pump`。业务状态通过带类型的 `SetBinding` 更新，文字可用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。调用成功表示绑定已被接受，不表示独立渲染线程已经提交新画面；需要观察提交/呈现状态时继续驱动 `Pump`。同一 UI 轮次的多次绑定修改会失效中间候选，在下一次封包时取最终 Scene 值；重复相同绑定不制造新像素。`Pump` 合并 Wayland 输入/configure、图片完成通知与调用者 FD 等待；资源就绪后在 UI 线程更新 Scene。静止页面不自行连续提交。`Close` 请求 worker 停止，待其在所属线程按 Ganesh → EGL → Wayland 顺序清理并 Join 后，UI 才释放自己的 Scene 与 CPU 资源。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

2026-09-29 输入升级：SDK 将显式 enter/leave/cancel、主按钮与键盘事件交给 Scene 的
统一输入状态机。普通按钮在同一节点有效按下并释放后才发送一次 `OnAction`；按下仅
建立局部状态，不调用业务。Enter/Space 同样在释放时激活，repeat 不重复提交；Tab 与
Shift+Tab 遍历控件，Escape 取消待定激活。拖出目标释放、隐藏/卸载/禁用、失焦或关闭
会取消相应序列；离开后返回原目标释放允许点击，手势识别器后续接入。

反向输入事件携带协议所有者当时的 `UiLoadId` 和 `shared_ptr<const InputSnapshot>`。
快照包含 Scene 身份与内部版本，来自 worker 最近成功采用的 FramePacket；UI 使用
事件自带的已提交几何命中，绑定修改产生的未提交候选不能提前移动输入范围。首帧成功
提交前没有快照，点击/接触不能激活动作；configure/close 继续处理。成功提交用于输入
排序，真实呈现仍由 presentation feedback 表示。已替换 UI 的输入确认消费后丢弃，
即使新 Scene 存在相同 NodeId/action 也不能承接旧输入。

相邻 motion 只在 UI、窗口、逻辑设备身份/代数与同一 snapshot 对象相同的情况下合并；
touch motion 还必须属于同一 contact，Down/Up/Cancel/Frame 均保持边界。worker 当前
快照、候选帧和排队输入分别持有强引用，有界队列消费/清理后自动释放旧租约，不维护
无限版本历史。Scene 始终以当前节点存活、action、隐藏和禁用状态否决已失效动作。

本轮将真实 touch 的 down/motion/up/cancel/frame 从 WM 经 WaylandWindow 接到同一
Scene 输入状态机；contact 单独捕获，不伪造鼠标点击或持久 hover。生产平台仍只适配
一个 Wayland seat，手势识别和拖动交互继续后续实施。源码与实测边界见
[交互规范第 14 节](INTERACTION_AND_PRESENTATION_SPEC.md#14-第三阶段成功提交输入快照与-touch2026-09-29)。

交互第二阶段的 `InteractionTarget`、`Visual` 与 `.state` 由同一 UI 线程 Scene 消费，
无需新增业务 ABI、周期 tick 或 Shell 专用回调。输入状态解析出有效目标后，现有单调
动画时钟与 frame opportunity 驱动 Paint 样本；不可变帧携带绘制数据、版本与只读输入快照。
Visual 子树不参与输入，它的平移、缩放、整体 opacity 以及 background 动画不改变父目标
的感应范围，不向 WM 发送布局或全屏请求。具体 DSL、优先级和安全范围见
[交互与呈现规范第 13 节](INTERACTION_AND_PRESENTATION_SPEC.md)。

状态规则保留主题引用和基础 binding；主题成功安装取消旧轨迹并取新主题目标，失败
保留原结果。隐藏/取消/UI 替换继续沿已有代数和清理通道处理。当前整体透明度需要普通
绘制图层，变换仍由 UI 采样并回放；尚无 GPU 保留层或渲染线程 Scene 采样。布局变化
使用成功提交命中快照，交互节点整体 transform 的逆变换命中仍未开放。Topbar 横线
只接入反馈，Dock 运行项沿用已有激活动作；没有重打包
或替换现有会话。本阶段验证结果在上述规范统一填写，不能沿用旧包实机验收作为证明。

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

### 平台状态快照

`GetPlatformStatus()` 返回按值复制的 `ClientPlatformStatus`，包含关闭请求、配置/映射状态、当前尺寸与 scale、frame callback 与 presentation 能力/计数、累计等待时间和 surface 提交计数。`IsCloseRequested()`、`IsMapped()`、`ConfigureCount()`、`FrameDoneCount()`、`FrameCallbackPending()`、`HasPresentationFeedback()`、`PresentationCount()`、`WaitDurationNs()` 读取同一份 UI 侧快照；`GetRenderStats()` 将快照中的平台计数与 Scene/GPU 统计合并。`PresentedCount()` 仍是 SDK 的成功 Swap 计数，不等于 presentation feedback 次数。Scene 布局使用与输入同序交付的 `ConfigureEvent` 自带尺寸、scale 和 configure 计数；不能用快照中的最新尺寸去解释队列里较早的事件。

渲染线程在配置、提交、回调、呈现和图片上传等状态变化后发送有序值事件；UI 消费后更新快照。Open 完成时还读取线程安全的初始值副本，使紧随 Open 的能力 getter 可用；关闭等待渲染线程清理后读取最终副本。纯等待时间变化不单独发送状态事件。Host 与业务入口始终不跨线程访问 Wayland 对象；快照描述最近已处理的平台边界，不承诺调用时刻的实时硬件或 compositor 状态。

### 提交与事件等待

`Pump(timeout_ms, std::span<pollfd> wake_fds = {})` 允许调用者提供借用的控制/退出 FD。负 timeout 为无限等待，非负为等待上限；描述符由调用者持有，SDK 只回填 `revents`。SDK 内部加入 `ImageResources::CompletionFd()`、反向事件 FD 与独立终态 FD；正向命令 FD 由渲染线程的 Wayland 等待监听，Host 不排空正向命令。所有 Wayland prepare/read/cancel 配对只在渲染线程的适配器中完成；UI 派发已有事件后返回 Host 重新计算等待源与业务 deadline。

平台提交采用 `None / State / Pixels / Failed` 结果与 prepare/commit/completion 三个阶段；`Deferred` 表示本轮受预算准备让出，`AwaitFrame` 表示等待兼容 UI 包或通知 FD。仅 Pixels 申请帧回调和 presentation feedback；State 不受旧像素 callback 阻塞。Scene 的 `PixelsRevision()` 与 Build generation 独立，纯 Composite 不 Build；首帧/resize 仍允许强制绘制。重复值和仅身份变化的合法主题不推进像素版本。

对应 metadata 随 State/Pixels 成功提交后，经反向事件在 UI 轮次调用 `AcknowledgeComposite()`，且只确认仍匹配的 Scene/主题版本。准备阶段已检查 metadata、请求与上次相同，或可选效果扩展不可用而没有请求可发送时，checked-identical `None` 也可确认 Composite，不额外制造 surface commit。因旧像素 callback 尚未完成而推迟准备的 `None` 不确认状态；待处理像素版本和 Composite 继续保留。ACK 只清 Composite，未提交的 Paint/Layout 不受影响；Failed 不确认内容。

新增计数为 `surface_noops`、`surface_state_commits`、`surface_pixel_commits`、`surface_submission_failures`；它们和 GPU/Swap/实际呈现独立。`FrameCallbackPending()` 提供只读节流状态，不使调用者拥有回调对象。提交失败终止当前连接，不能用销毁客户端 callback 代理来宣称撤回服务端 pending 请求。精确生命周期与验证规范见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 9 节。

### 像素损伤与 buffer 修复

Pixels 准备比较上一成功提交的完整 DisplayList/资源版本与当前列表，产生内容损伤；使用平台实际 buffer age 合并有界成功历史，得到当前 buffer 的修复区域。age 0/未知、首次/尺寸/WSI 变化、历史不足和无法证明的绘制范围全量回退。文字、图标、阴影按实际 Skia ink 计算；装饰变换和 opacity 的旧/新范围按当前通用分析路径处理，结构或不受支持的绘制状态保守回退，具体范围以[渲染失效规范](RENDER_SCHEDULING_AND_INVALIDATION.md)为准。没有增加节点增量布局或显示列表分块缓存。

平台顺序为 QueryBufferAge → SetDamage(repair) → Render → Swap(content)。renderer 清理并按原序列绘制精确修复并集，保留区域外内容；提交时声明内容损伤，不能把扩大后的历史修复误传给 compositor。Swap 成功后推进预分配损伤历史，并移动只读帧引用作为下一次比较基线；State/None 不推进，失败使历史失效并清理。EGL 能力不可用时继续同一后端的完整修复/普通 Swap。当前逻辑与 buffer 坐标为 1:1，非 1 scale 暂全量回退。

`ClientConfig::partial_rendering` 默认 true；false 保持同一 renderer、时钟/业务和内容损伤，只强制完整像素修复，可用于策略回退与对照。该 C++ 配置不改变业务模块 ABI v1。`ClientRenderStats` 新增 `full_pixel_repairs/partial_pixel_repairs/empty_pixel_repairs/pixel_repair_pixels`，它们在成功 Render 后计数；`content_damage_pixels/damage_history_commits` 在成功 Swap 后计数。面积为 clip/union 后的 buffer 像素范围，不是实际 fragment 数或 GPU 耗时。`buffer_age_queries/unknown_buffer_ages/last_buffer_age` 是平台方法请求，-1 表示不可用，0 表示内容未知；能力字段为 `buffer_age_supported/swap_damage_supported/partial_update_supported`。与已有 Render/Swap/实际 presentation 计数分开解读。

公开局部 Render overload 不得扩大调用者已声明的修复域，面积/碎片导致的 Full 策略必须在 SetDamage 之前由 producer 决定。执行规范与验证结果见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 11 节及后续结果。

`buffer_age_supported` 表示可安全使用的保留能力：EXT buffer age，或可实际调用 SetDamage 的 KHR partial update；只有扩展名称而无法声明修复区域不能启用局部绘制。生产 GLES 直接裁剪重放。CPU raster 的 partial Render 为保持软件 AA 的字节一致性，采用完整临时重放后仅复制修复区域的保守回退，不计为 CPU 局部绘制优化。

## 2026-09-27：UI 准备与安装

SDK 新增 `BeginUiLoad`、`CancelUiLoad`、`OpenPrepared` 与 `ReplaceUiPrepared`，均属于
UI 所有者线程。`runtime::PrepareComponent(source, ComponentSource)` 是独立纯 CPU
编译接口，调用者可在工作任务中使用，不需要 ClientApplication、Scene、图片资源表、
字体或图形对象。结果拥有源数据并只提供 const 访问；图片保存符号 URI，主题保留引用。

先在所有者线程发行 UiLoadId，准备完成后携带原令牌回到同一前端安装。过期、取消或
跨前端结果在资源申请前拒绝；成功安装的整份 UI 令牌不重复消费。安装失败保留旧
Scene，当前代可以重试；Open 的连接失败清理未打开的窗口，不取消同代准备结果。
Close 为终态，撤销所有未安装结果。安装诊断是 LoadDiagnostic，不从日志文字提取行号。

旧 Open/ReplaceUi 复用相同准备与安装管线。这些方法本身不创建准备线程；Host 的
异步 Master 使用独立 LoadSession，完成后回到所有者线程调用同一安装接口。

`GetUiPresentation(UiLoadId)` 返回该代的 installed/submitted/presented、提交 ID 和反馈
计数；当前与前一代的状态有界保存。仅成功像素提交触发具名 `OnUiSubmitted` 观察者，
实际 presented/discarded 与该提交对应；State、None、Swap 失败、frame callback 或旧代
反馈不能证明新代已经呈现。未知/已淘汰代数返回 false，Close 清理追踪与观察者。
提交观察者在 UI 轮次处理有序提交事件，只能做短时通知/派发；它不从 Wayland 提交回调中直接调用 Host。

图片任务回滚、分阶段挂载、绑定状态表和多组件 critical 聚合就绪仍按
[加载规范](MASTER_PARALLEL_LOADING.md) 后续步骤实施。

## 2026-09-30：连续手势与系统控制接口

`ClientApplication::OnGesture` 在 UI owner 线程交付拥有数据的 `GestureEvent`，包含
Begin/Update/End/Cancel、进程内不复用 ID、动作、来源、contact、原始 Down serial、
逻辑坐标和输入快照身份。回调可安全替换 UI；旧 UI 的后续移动被过滤，已开始手势
有一次终止通知。视觉反馈仍通过通用 state/animation 接口表达。

Host 的业务 ABI v1 使用 `struct_size` 可选尾部提供 `on_gesture`、`subscribe_layout`、
`on_layout_state`、`control_gesture` 与 `on_layout_control_result`。模块可在真实 Begin
回调中选定来自 WM 快照的目标，Host 管理真实输入凭证、ACK、移动合并与终止顺序。
一般应用只获得本地手势；WM 系统控制需经过 launcher/WM 的实际 Shell 身份校验。

本阶段系统操作是跟踪会话，`applied=false`；组沉浸和分隔线尺寸修改尚未启用。
接口示例、生命周期、权限、上限与验收记录见
[布局控制计划第 8 节](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md#8-第二步源码交付连续手势与-typed-控制会话)。
