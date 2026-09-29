# Prism 动画运行时与 DSL 契约

日期：2026-09-29。状态：**v1 Paint Transition 已验证；交互第二阶段状态规则与装饰子树呈现已通过 Pi 源码和隔离验证，完整动画系统仍按本规范分阶段建设**。当前可用接口以 [DSL schema](../prism/runtime/dsl_schema.cpp)、[客户端 SDK](CLIENT_APP_SDK.md) 和源码为准；旧版已安装包不会因源码变化自动获得动画能力。[动画与渲染优化假设](ANIMATION_RENDERING_HYPOTHESES.md)记录实验与验收，[客户端渲染线程迁移](CLIENT_RENDER_THREAD_MIGRATION.md)记录已经完成的线程所有权切换。

| 范围 | 当前源码状态 | 后续工作 |
| --- | --- | --- |
| 时间核心 | 可注入单调时钟、绝对时间时长曲线与标量弹簧求值、重定向/暂停/取消 | 弹簧接入 Scene 和 DSL；WM、装饰复用同一核心 |
| DSL 与 Scene | `.transition`、`.state` 编译为 typed 描述符；Progress.value、指定 foreground 与 Visual 的局部呈现属性在 Scene 覆盖层求值 | 交互节点整体移动、效果/输入同代协议；显式 Clip/关键帧另行设计 |
| SDK 提交 | UI 线程采样，独立动画期限收窄 `Pump` 等待；worker 发帧机会，UI 应答后才放行像素候选；Pi V3D headless 功能门槛已通过 | 物理输出/VNC 对照、精确呈现测量、通用保留层与渲染线程合成采样 |
| WM/BSP | 未接入新时间核心 | 通用窗口视觉几何适配及 configure/输入一致性 |

## 1. 目标与边界

- 同一份运动数学和时间语义供客户端 Scene、主题装饰和 WM BSP 视觉几何复用；各系统拥有自己的目标对象与提交适配器。WM 不解析客户端组件、不接收业务绑定或客户端 Scene。
- DSL 在纯准备阶段编译出带类型的运动描述。运行期不重新解析 DSL，不通过组件名称或应用名称选择动画实现；Skia 只消费已准备的绘制或保留层命令。
- UI/Host 线程拥有绑定、主题、目标属性、Scene、命中和动画生命周期。渲染/协议线程独占 Wayland、EGL、Ganesh、GPU 资源与最后成功提交基线。没有可见动画时，两线程回到事件等待。
- 初次安装 Preview/Master 默认直接取目标值。进入动画必须由声明的状态变化或显式触发启动，不延迟已有启动里程碑。

当前 `Scene` 有 RenderTree 复用和 `Layout/Paint/Composite` 失效类别，Visual 装饰子树已提供平移、缩放、原点与整体 opacity；尚无 GPU 保留子树。当前动画仍按 Paint 构造 DisplayList 并回放；只有引入并验收真正的保留层之后，稳定内容的运动才能称为渲染线程采样的合成动画。旧 `MotionController::Step(dt)` 服务旧装饰路径，不作为新时间语义或 native Wayland 动画驱动。

## 2. 时间与运动的定义

所有时间点与期限来自同一个 `CLOCK_MONOTONIC` 纳秒域。`AnimationClock` 提供当前时间；测试可注入虚拟时钟。Wayland frame callback 是像素提交许可和节流信号，presentation feedback 是事后呈现结果，两者均不代替动画时钟。独立的 `ScalarTimeline` 提供 `Start / Retarget / Sample / Pause / Resume / Cancel` 与请求代数；Scene 在每个 UI load 的自身生命周期中按 `(NodeId, DslProperty)` 管理呈现轨迹。WM 将来以自己的对象身份接入；时间核心不依赖 Scene、Wayland 或 Skia。

对时长型轨迹，在任意采样时刻 `t`：

```text
u(t) = clamp((t - start_ns - delay_ns) / duration_ns, 0, 1)
value(t) = Interpolate(from, to, Easing(u(t)))
```

`duration_ns = 0` 立即使用终值；首版 DSL 的 `delay_ns = 0`。渲染机会晚到时直接采样该时刻的值；不累计每帧位移、不补交错过的采样，也不以固定帧数定义完成。最终样本必须精确写入目标值，以免浮点残差使时间线继续唤醒。样本变为同一可见像素值时可跳过该次提交，但结束期限仍须处理。例如 `Progress.value` 从 `0.2` 到 `0.8`，时长 `180 ms`、`easeOutCubic`，开始后 `90 ms` 的值是 `0.725`；若下一次机会在 `200 ms` 才到，直接取 `0.8`，不补交中间帧。

弹簧轨迹保存 `from`、`initial_velocity`、`target`、`start_ns`、刚度与阻尼比，按从起点起的实际经过时间求位置和速度。优先采用阻尼谐振子的解析解；若以后引入无法解析的非线性模型，使用独立于渲染帧的固定步长及明确的大间隔快进/归位策略，不能把一段长时间压进少量增大的子步。弹簧以位置和速度阈值判定稳定，设有有界的最长运行时间，归位时写精确目标值。弹簧参数不是贝塞尔 `duration` 的别名。

同一属性运动中收到新目标时，在同一个 `now` 先求旧轨迹的当前值和速度，再以此为新轨迹起点与初速度。时长型曲线重设起点和开始时间；弹簧保留速度。输入重复或新目标校验失败时不创建多余轨迹；新目标恰等于当前呈现值时直接归位。时间核心提供独立请求代数和 `Superseded` 终态；首版 Scene 只消费时长轨迹，不把这些终态暴露给应用。

## 3. 属性、绑定与呈现层

属性按三层求值：

1. **源值**：DSL 常量、主题令牌、业务绑定及现有优先级。这些数据保存作者意图，主题事务与绑定校验仍在这里进行。
2. **目标值**：一次成功事务后解析出的带类型结果。业务读取与下一次重定向使用目标值，不把暂时的呈现样本回写为业务状态。
3. **呈现值**：活跃轨迹在给定时间的样本；没有轨迹时等于目标值。Build、SurfaceEffects、InputRegions 和命中必须读取同一个呈现代数的值。

`Scene::SetProperty` 把目标值写入 retained properties、标记 explicit、移除对应主题引用，并推进事务版本。呈现样本由 Scene 的独立覆盖层提供，不逐帧调用公开 `SetBinding` 或 `SetProperty` 回写业务状态。每个 Scene 属于一个 UI load，覆盖层按 `(NodeId, DslProperty)` 标识轨迹；作者值、主题引用和绑定映射始终保留。对同一键最多有一条生效轨迹，新目标重定向该轨迹。初次安装不自动从属性默认值过渡。

现有 `PropertySpec` 是属性类型、范围、失效类别和允许动画组件的同一 schema 来源；首版只开放下表的安全 Paint 属性。后续扩展要在此基础上明确实际更新成本、是否影响命中/效果、可见量化精度和保留层资格，避免另建一套按控件名称查询的动画属性表。数值只接受有限值；颜色先在线性 sRGB、预乘 alpha 空间插值，再按渲染契约量化。布尔、字符串、资源 ID、`action`、`source`、`material` 和 `visible` 是离散值，不用数值插值冒充动画。

| 属性范围 | 当前源码处理 | 后续能力和条件 |
| --- | --- | --- |
| `Progress.value` | 已接入 `0..1` 数值 Paint 轨迹与 DSL。 | 同一机制扩展到经 schema 允许的边框与阴影等 Paint 属性。 |
| 直接绘制节点的 `foreground` | `Text`、`Icon`、`IconButton`、`Progress`、`Toggle` 已接入颜色 Paint 轨迹与 DSL。 | `Button.foreground` 会转给生成的文字子节点，描述符未随子节点传递前拒绝。 |
| `background` | 仅 Visual 开放 Paint 过渡；装饰子树明确不参与输入区域。 | 普通节点的 alpha 仍参与输入区域，未开放其过渡。 |
| `width/height/spacing` | 保留 Layout 成本标记，不纳入首批效果。 | 每个样本在 UI 线程重排并同步命中；可证明安全时再使用旧/新几何的保留层视觉过渡。 |
| `backdropBlur`、圆角与输入轮廓 | 保留当前效果和输入约束。 | 只有像素、surface 效果和输入区域可在同一呈现代数中提交时才开放动画；玻璃域须单独计成本。 |
| `Visual.opacity/translateX/translateY/scaleX/scaleY` | 第二阶段新增安全装饰子树的 Paint 过渡；目标命中范围固定，opacity 是整组合成。 | 交互节点移动必须先完成同代命中；GPU 保留层与渲染线程采样后续接入。 |
| `Visual.originX/originY` | 归一化原点，静态和状态覆盖可用；不允许 Transition。 | 更丰富的变换组合和动画需单独扩展 schema 与损伤校验。 |

呈现覆盖层变化会推进节点绘制 revision，以免 RenderTree 复用旧视觉记录。当前只开放不改变输入轮廓或 surface 效果的属性；`SurfaceEffects()` 与 `InputRegions()` 仍读取 live Node。将来扩展会影响它们的动画属性时，必须让像素、效果与输入读取同一份呈现状态，不能只改变 DisplayList。

## 4. DSL v1：目标变化触发 Transition

第一版只定义目标变化触发的自动过渡，不定义 `from/to`、重复、延迟或应用回调。当前编译器接受以下语法；组件所在的 UI 需要用包含这次源码实现的 Host/SDK 构建运行：

```prism
Progress(value: $progress)
    .transition(property: "value", durationMs: 180, easing: "easeOutCubic")
```

`transition` 只接受恰好三个命名参数 `property`、`durationMs`、`easing`。`property` 是该组件在 schema 中明确允许的属性名；v1 提供 Progress.value/指定 foreground，交互第二阶段增加上表中的 Visual 属性。`durationMs` 为 `0..10000` 的有限整数毫秒，`0` 立即取终值；第一版仅接受字面量，不从业务绑定或主题令牌读取时长。`easing` 只接受 `linear`、`easeInCubic`、`easeOutCubic`、`easeInOutCubic`，分别为 `u`、`u³`、`1-(1-u)³`、`u<0.5 ? 4u³ : 1-4(1-u)³`。未知/重复参数、重复目标、非法属性/类型/范围或未知曲线，均在纯准备阶段给出行号诊断。描述符受现有 Scene 节点上限与准备期内存预算约束；准备结果必须把描述符计入 retained bytes。并发活跃轨迹的容量和降级策略在 Pi 的 100/500/1000 节点对照后确定，不能暗中减少对象数量或阻塞 UI。

`.transition(...)` 已有独立语义分支，校验后的 `TransitionSpec` 沿 `PreparedNode → Blueprint → Scene Node` 传递，并计入准备结果的 retained bytes；区域安装、结构比较和事务复制保留描述符，detached candidate 不得覆盖 retained 轨迹元数据。renderer 不通过字符串查找 `"value"` 或重读 DSL。业务模块继续只调用带类型的 `SetBinding("progress", value)`；一次成功的目标变化触发过渡，重复值不启动。动画由 SDK 驱动；业务无须为了过渡安排周期 tick。

第一版主题切换先完成候选验证和原子安装，然后取消旧轨迹并直接取新目标；失败则保持原主题、目标和呈现状态。主题运动令牌与主题切换过渡仍待后续；局部状态规则的当前优先级见下节。显式关键帧片段将由带类型的 `AnimationClip { tracks, duration, repeat, fill }` 表示，每条 track 引用同一属性 schema 和时间域；显式触发、关键帧 DSL、轨迹优先级与取消策略需单独版本化。当前不把示意关键帧语法加入第三方应用指南或声称可用。

### 4.1 交互第二阶段的 StateRule

`.state(when: "hovered", scope: "target", scaleX: 1.15)` 将局部状态映射为目标值；
`.transition` 继续决定目标变化时如何运动，两者独立。状态退出回到当前基础来源，业务
binding 与主题引用不会被临时样本覆盖；一次输入轮次先解析状态目标，再按单调时间采样。
无 Transition 的合法状态属性立即生效，originX/Y 当前只能立即变化。

声明节点必须位于 InteractionTarget 的 Visual 子树内，scope 只能为 `"target"`。普通状态
优先级为 disabled > pressed > captured > hovered；focused/focusVisible 使用独立属性，
与不同状态写同一属性时编译拒绝。合法条件、值范围、主题预检和结构限制详见
[交互与呈现规范第 13 节](INTERACTION_AND_PRESENTATION_SPEC.md)。

Visual 子树整体 opacity、变换与阴影进入普通 Paint 回放，仍会消耗 Build/Render/Swap；
它们不改变父目标命中范围，不涉及 WM 全屏或 BSP 操作。Topbar/Dock 当前接入使用已有
主题颜色/尺寸和字面量运动参数，没有 MotionSpec/MotionPolicy 或关键帧 DSL。

## 5. Timer Driver、帧机会与背压

SDK 独立管理客户端 Scene 的动画期限，不占用 `ModuleSession::schedule_tick` 的单个业务定时槽。当前实现以 `CLOCK_MONOTONIC` 纳秒记录期限，并收窄 `ClientApplication::Pump` 的 `poll` 等待；毫秒超时向上取整以避免提前空转，没有单独的 timerfd 驱动。首次目标变化按 `now` 建立轨迹；后续从帧机会采样，有可见像素变化才请求帧，否则设置下一次单次期限。像素 callback/反馈容量阻塞时等待提交许可，不积攒待画帧。样本量化后确无可见变化时，为下次机会或精确终点设期限；无法证明更早变化点时允许保守提前唤醒，但仍禁止空白 Build/Render/Swap。无活动轨迹时撤销期限并恢复事件等待，不固定 `poll(0)` 或周期性 60/90 Hz 唤醒。

渲染线程在像素 frame callback 解除且反馈容量可用时，向 UI 发送带 `UiLoadId`、worker epoch、configure count 与机会 ID 的专门 `FrameOpportunityEvent`。现有可合并的状态快照仅供诊断，不作为动画帧许可。每个 UI/worker 代数最多一个未应答机会。**活动动画期间，worker 不得在 callback 中直接重交旧像素候选**；目标开始或重定向时沿用失效屏障撤销尚未提交的旧候选。UI 通过有序 `AnswerFrameOpportunityCommand` 携带不可变 `FramePacket` 应答；若本次没有新像素，则以空包表示 `NoVisibleChange`。worker 只在对应机会获应答后放行 Pixels 提交。空应答消耗此次机会，不提交旧候选，并保留空闲的提交许可；下次单次期限或业务变化可直接发布新包，不需等待不存在的新 callback。`State` 提交仍可按现有协议独立推进。UI 在一个工作轮次内处理目标/主题/输入，按当前 `now` 对 UI 绘制轨迹只采样一次，发布至多一份兼容 `FramePacket` 再答复机会；过期的 UI/configure/worker 代数或机会 ID 一律丢弃。量化后暂时无可见变化及最终归位使用单次期限保证进度；callback 暂停时不积攒待画帧。动画样本推进 Scene 的像素 revision；不能每次通过公开 `SetBinding` 制造失效屏障和两份中间帧。

当前 `FramePacket` 携带 UI 已完成样本的 DisplayList、scene/pixels revision、序号和 `AnimationSampleStamp { revision, time_ns }`。Scene 仅在可见动画呈现样本变化时推进该 revision，`time_ns` 是该次采样的 `CLOCK_MONOTONIC` 时刻；`revision == 0` 表示本 Scene 尚无可见动画样本，不能仅凭 `time_ns == 0` 判断，因为虚拟时钟的真实样本也可能发生在零时刻。封包只复制 Scene 最后的样本标记，不把封包时刻冒充采样时刻。它尚不等于真实屏幕呈现时间或完整端到端测量链。待真实保留层实现后，才可扩展只读轨迹供渲染线程自行采样。渲染线程继续核对 UI load、configure、尺寸、scale、资源代数和输入事件确认；积压时只保留最新普通视觉候选，不丢安装、资源和主题顺序屏障。像素回调仍限制在途提交，损伤继续相对**最后成功的像素提交**计算。动画正在运行不意味着每次计时唤醒都必须 Render/Swap；静止、不可见或样本量化后无变化时无需新像素。

以后具备真实 GPU 保留层时，UI 决定轨迹和目标并发布只读参数，渲染线程只对可合成的位移/缩放/透明度按同一个单调时间域采样。它不解析 DSL、不处理业务绑定、不重排。每次提交以最后成功样本和当前样本的旧/新可见范围并集计算 damage，再结合 buffer age 修复；保留层节省子树重复构建与光栅，不省去可见动画帧的提交。

## 6. 生命周期、交互与策略

本节同时列出已接入的 v1 行为与后续必须保持的契约。当前 Scene 只提供 Paint
Transition，不提供应用动画回调、显式暂停控制、减弱动效设置、交互位移或 WM 动画。
局部状态、输入捕获、状态规则和桌面控制区的当前范围与后续契约见
[通用交互状态与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)。Topbar 横线面向组控制，
未来分隔线横线面向边界/窗口控制；首批运动只改变固定感应区中的视觉子树。

- 每个 Scene 隶属一个 UI load，v1 轨迹键为节点身份和属性；`ScalarTimeline` 单独维护请求代数。UI 替换、区域卸载、节点删除与 Close 取消旧轨迹，并在仍有效的目标上归位；区域保留同一节点时以当前样本为起点继续或重定向。迟到的 frame opportunity 不能修改新代 UI。以后若向应用发布完成通知，通知也必须携带对应代数并执行同样校验。
- 时间核心有 `Running / Finished / Cancelled / Superseded` 状态及终态事件；Scene v1 不向应用公开每条轨迹的终态。`Finished` 表示最终目标样本已应用并发布为 UI/提交候选，若像素本就相同则无需提交；它不等于显示器已呈现。需要视觉里程碑时另用精确 submission 与 presentation feedback 关联。首版取消将仍有效的节点归位到逻辑目标，卸载/关闭则清理呈现覆盖层；重定向以取消前的当前样本为新起点，不先归位。
- DSL 节点显式 `visible: false` 时取消并归位，重新显示不重播。窗口遮挡或工作区隐藏目前没有可靠的 SDK 可见性通知，不能从 keyboard focus 或 mapped 推断；callback 停滞时不空转，恢复后按绝对时间直接跳到当前或终值。时间核心已经支持显式暂停；Scene 的公开暂停接口与可见性驱动暂停待建立正式通知契约后再实现。
- 后续减弱动效策略由平台/主题上层决定，可使过渡立即达到目标；业务目标与动作不变。策略切换、取消和快速重复操作均不得留下定时器或未释放的资源。
- 当前 Visual 横线运动保持 InteractionTarget 的静态命中范围。可点击节点运动需要在同一呈现代数中包含视觉变换、逆变换命中、裁剪、surface 输入区域和按下到释放的目标捕获；未满足前不能开放交互位移属性。
- WM BSP 动画适配器接入后，拓扑立即产生目标槽位，适配器只运动通用窗口的视觉几何；平移可复用客户端 buffer，真实尺寸变化仍要按 Wayland configure 与客户端新 buffer 处理。WM 与客户端分别调度和取消自己的轨迹。

## 7. 验收与实施顺序

1. **时间核心，源码已实现**：注入时钟、时长曲线与弹簧求值、重定向、取消、完成和绝对期限；保持确定性测试比较 0/16/33/200 ms 等不等间隔采样、长暂停、连续改目标和精确终值。同一时刻的结果必须与采样次数无关。
2. **属性与 DSL，v1 源码已接入**：单一 schema、准备结果和 Scene 呈现覆盖层已贯通 `Progress.value` 与指定节点的 `foreground`。持续验证主题/绑定源值未被覆盖、重复值不建轨迹、主题原子安装并归位、非法 DSL 在准备期拒绝、初次 Preview/Master 不自动动画。
3. **生产调度，Pi V3D headless 功能门槛已通过**：SDK 独立单次期限、专门 frame opportunity 与应答、一个 UI 轮次合并封包。继续验收慢帧直接跳到时间对应位置且不撑满双向队列、扣留 callback、填满 feedback 槽、只提交 State、resize/安装后旧机会到达、discarded/乱序 feedback；再做 VNC 与物理输出对照。测试与探针保留在 `tests/`，不进入生产包。
4. **交互与视觉扩展，分阶段实施**：[交互与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)的节点命中身份、输入生命周期与局部状态已进入源码；第二阶段已加入状态规则、固定 InteractionTarget 与 Visual 子树 transform/opacity，完整验证结果见该规范第 13 节。首批控制横线保持固定感应区，在 Pi V3D 下对照完整修复与局部损伤；后续贯通 touch、交互变换的同代命中、权威布局控制，再加入保留层与 WM BSP 视觉运动。每一项保留原始帧记录和资源成本，按 [A01–A12](ANIMATION_RENDERING_HYPOTHESES.md)核对。

`AnimationSampleStamp` 已记录 Scene 最近一次可见样本的修订号与单调采样时间，并进入 `FramePacket`。后续测量还需把目标/呈现代数、FramePacket 序号、成功提交 ID、feedback 及 UI/Render/Swap/WM 阶段耗时关联起来。客户端目前只保存 presentation 的结果；需先保存并确认 `wp_presentation` 时钟 ID、实际时间戳、refresh、sequence 和 flags，才能报告真实呈现间隔或输入到呈现延迟。Swap 成功计数、feedback 到达时间和 VNC 更新数均不能替代该测量。

2026-09-29 在 Pi 上完整构建、CTest **58/58**、代码风格检查与 `git diff --check` 通过。隔离 V3D headless WM 的 `prepared_ui_probe.py` 四项门槛通过，其中动画门槛使用生产 SDK、Skia GLES 和 `Progress.value` 过渡。探针先验证静置，再以 600 ms 曲线改变目标并重定向：成功像素提交由 2 次增至 42 次，Scene 布局始终为 2 次，动画期间 39 次为局部修复；结束后再次观察，Build、Render、Swap 均无增长。随后增加动画中隐藏与重新显示，隐藏后停止继续绘制，重新显示只提交当前目标。原始日志位于忽略的 `dist/validation/animation-v1-20260929/`，最终取消场景记录在其 `cancellation/` 子目录。这些计数证明本场景的功能和停帧，不是显示器呈现 FPS，也不能代替物理输出或 VNC 体验测试。
