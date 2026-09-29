# 动画与渲染优化假设清单

日期：2026-09-29。状态：**待验证的假设与执行顺序**，不是已实现功能或性能承诺。
动画接口、时间语义和生命周期的现行目标以[动画运行时与 DSL 契约](ANIMATION_RUNTIME_SPEC.md)为准；
独立渲染线程已按[迁移记录](CLIENT_RENDER_THREAD_MIGRATION.md)接入生产；首版 Paint 过渡、
单调时间轴和帧机会协议已接入 SDK，保留层、WM BSP 视觉运动与完整逐帧测量仍待实现。
本清单承接[渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md)末尾的“下一步接口顺序”，
以[原始架构设计](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md)的动画目标为参考。
近期工作在通用动画时钟基础上完善局部状态与呈现接口、测量链路，再实现 Dock、Topbar 的轻量动效；
启动进程、Zygote 和字体共享不作为动画实施的前置条件。

## 1. 当前事实与架构边界

- WM 已按 `needs_frame`/damage 工作，静置时不自发请求下一帧；效果依赖已按采样域
  收窄。SDK 已区分 `None`、surface `State`、像素 `Pixels`，并在可靠 buffer age 下
  使用局部修复。这些是现有基线，不列为新的待实现优化。
- Scene 已按 Layout/Paint/Composite 分类，未变节点的 Render Tree 记录可复用；
  每次需要新像素时仍生成完整 DisplayList，并在修复区域裁剪下按顺序遍历回放。
  v12 的局部修复面积减少 95.24%，**不代表** CPU 命令遍历、GPU 时间或帧数也减少
  95.24%。见[现有测量](RENDER_SCHEDULING_AND_INVALIDATION.md#真实-sdk-修复范围对照)。
- 当前 `Composite` 主要用于 backdrop/input shape 等 surface 状态；DSL 尚无真正的
  节点 transform/opacity 动画和保留图层合成通道。给属性标记 `CompositeDirty`
  并不能自动避免 Skia 重绘。旧装饰器的动画接口没有接入真实 Wayland 窗口的帧需求。
- Music/Dock/Topbar 等客户端 UI 由统一 Host、DSL、Scene 和 Skia 管理。动画语义、
  时间线和属性成本应放在通用 SDK/schema；WM 只处理通用 surface、BSP、材料、
  damage 与输出，不按应用名称实现动画。
- Pi 旧物理输出基线为 1024×600、约 59.821 Hz；现有远程会话是 1280×720、
  90 Hz 虚拟输出，WayVNC 的 120 FPS 是上限。headless 的呈现反馈和 Windows
  Viewer 的更新数都不能代替 HDMI 扫描输出或真实输入到显示的延迟。

## 2. 测量口径与试验隔离

每一帧以同一 `animation_id/frame_id` 关联事件到达、动画采样、属性更新、
Scene Build/Layout、DisplayList 生成与比较、Skia Replay/flush/submit、EGL Swap、
Wayland surface commit 和 WM output commit 的单调时钟时间点，并关联对应的
`wp_presentation` 结果。当前客户端只保留 presented/discarded 结果，尚未记录
`clock_id`、呈现时间戳、refresh/seq/flags；必须先保存这些值并确认其与事件时钟
同域，或做明确的时钟换算，才能计算实际 presented 间隔和输入到呈现延迟。
在此之前，只能记录反馈到达时间和结果；不能把到达时间充当呈现时间。
分别报告 CPU 时间、有效的 presented 间隔、discarded/漏过刷新周期和输入到呈现延迟；
若驱动支持异步 GPU 时间查询，再单列 GPU 完成时间。`Swap` 耗时、WM commit CPU
耗时、`current_fps`、刷新率与 VNC 发送上限均不是 GPU 完成或用户看见的帧率。

同一场景保留静态对照，按 60 Hz 约 16.67 ms、90 Hz 约 11.11 ms 的刷新期限
观察 p50/p95/p99 与长尾，不将单次峰值平均掉。固定分辨率、主题、页面、窗口数、
频率策略和预热条件；每组重复运行，保留逐帧原始数据、CPU/PSS、温度/频率及
GPU 资源缓存使用量。Glass/Clear 分开比较。当前先在 Pi 的 V3D headless、
VNC 断开条件下验证协议和 GPU 路径；再单独比较 VNC 已连接及 Windows Viewer。
物理屏幕恢复后重新测 DRM/HDMI 呈现，不混合三种环境的 FPS。

试验用确定性动画和测试窗口放在 `tests/` 或忽略的 `dist/validation/`，不进入
生产源码或 deb。初始场景为静态桌面、小横线/图标位移、单窗 100/500/1000
静态节点、玻璃区域内外的运动、两窗 BSP 移动/resize、主题/配色切换及
动画中关闭窗口。先验证像素、命中、取消和失败回退，再比较性能。

## 3. P0：先建立动画的正确帧生命周期

| ID | 可证伪假设 | 对照和判定依据 |
| --- | --- | --- |
| A01 活动帧 | 只有可见的活动动画才额外请求下一帧；完成、取消、隐藏或关闭后回到事件等待。 | 用 300 ms 与 1 s 动画对照静态时钟基线，统计结束后额外 Build/Render/Swap、WM 提交与唤醒；不得留下动画自己的周期提交。 |
| A02 时间语义 | 以 `CLOCK_MONOTONIC` 绝对起点采样进度，帧延迟只跳到当前正确进度，不积攒待补画的帧。 | 注入 0/16/33/200 ms 及暂停恢复；最终值精确到目标、取消不再回调，长空闲后无巨大模拟步长。 |
| A03 帧节流 | Wayland frame callback 约束在途像素帧，presentation feedback 只证明实际呈现；二者分开推进状态。 | 记录 requested/submitted/presented/discarded 与对应 ID；慢帧、隐藏、resize、输出切换期间不形成无界队列或重复动画完成事件。 |
| A04 瓶颈定位 | 活动动画的长尾集中在可识别阶段，不能凭 `fps` 或 CPU commit 猜测 GPU/驱动问题。 | 对照逐帧阶段时间、输入到呈现和效果 pass；若最大贡献不在预想阶段，先改假设和优先级，再选实现。 |

首个功能检查点已贯通通用时间线、`Progress.value`/`foreground` Paint 过渡、
帧机会背压和测试场景。仍需用 Pi 对照数据确认 A01–A04 的运行成本与呈现时间。
保留现有单次等待模型，不恢复永久 60/90 Hz 轮询；运行期消费准备时编译的有类型
动画描述，不逐帧重新解析 DSL。

## 4. P1：小范围动效与客户端成本

| ID | 可证伪假设 | 对照和判定依据 |
| --- | --- | --- |
| A05 小范围 Paint | Dock 运行横线、Topbar 横线和图标反馈可在 Paint 路径中连续运动，不触发 Layout/文字 shaping，旧位置与新位置都正确修复。 | 对照强制整窗修复：Build/Layout、命令数、repair 像素、Replay/Swap CPU、呈现长尾和逐像素结果；Glass/Clear 均通过。若小范围仍错过预算，查完整列表路径。 |
| A06 保留内容合成 | 对稳定内容的 translate/scale/opacity，真正的保留图层或等价通道可跳过子树 Paint/Layout/DisplayList/Skia Replay。 | 先实现通用有类型属性和真实提交路径，再与宽高、颜色动画对照；1000 静态节点移动父节点时检查 Build/Replay 次数、旧∪新可见范围、遮挡、裁剪、点击命中及 GPU/PSS。若只是换 dirty 标签仍重画，假设未通过。 |
| A07 完整列表瓶颈 | 对一个小图元的每帧变化，完整 snapshot/DisplayList 比较和 Replay 遍历会随静态节点数增长，成为动画长尾。 | 固定动画面积，用 100/500/1000 节点测构建、比较、访问命令及 p95；仅在斜率和预算证明必要时引入节点增量布局或分块缓存，并记录缓存内存与失效成本。 |
| A08 资源稳定性 | 静态字形和图片不应因动画每帧重复 shaping、解码或 GPU 上传。 | 记录字形 shaping、图片 decode/upload、Skia cache 使用及纹理分配；主题、scale、resize、设备恢复另测首次恢复成本。 |
| A09 输入响应 | 动画的 CPU/GPU 工作不得挤压输入与业务事件，导致点击后才出现明显视觉反馈。 | 同负载比较静态/动画时的 pointer event age、点击到 presented p95/p99、队列长度和取消响应；远程本地绘制光标的体感不代替此指标。 |

A06 属于原始架构的目标，而非当前接口能力。首个真实动效优先 A05；A06 的保留层
通路按目标架构实施，A07 数据只决定分块缓存的先后与预算，不审批 A06。动画属性
仍须通过 DSL schema 和 Scene 暴露，Skia/WM 不解释某个应用的业务动作或主题名称。

## 5. P1/P2：合成、玻璃、BSP 与生命周期

| ID | 可证伪假设 | 对照和判定依据 |
| --- | --- | --- |
| A10 玻璃依赖 | 采样域外的客户端动画不增加该玻璃区域的 capture/blur/material；域内变化只重算受影响区域。 | 同一路径让图标穿过/远离玻璃，分别测 Glass/Clear 的 effect hit/miss、pass、结果像素、GPU 时间和真实画面。移动玻璃本身可能需要重新采样，不能预设零 pass。 |
| A11 BSP 几何 | 在主题、scale、窗口状态与输出不变的受控场景，单纯位置调整应避免客户端 configure、布局和新 buffer；真正 resize 的代价单独衡量。 | 两窗同位移/resize 对照 XDG configure、客户端 Build/Swap、WM damage/effects、实际呈现及输入命中。旧装饰器动画不算已接通。 |
| A12 隐藏与内存 | 不可见窗口的动画可暂停，保留层/Skia 缓存可在内存压力下按级别回收，并在恢复时保持正确画面和可接受延迟。 | 工作区切换、遮挡、取消、主题/配色、resize、关闭及恢复；测 PSS/私有脏页、GPU 缓存、恢复首帧和额外重建。完整遮挡的判定先作正确性验证，不把 `UI_HIDDEN` 直接等同于遮挡。 |

## 6. 暂缓的 Android 类比假设

以下是假设而不是本轮动画前置任务；它们与动画实验使用相同的测量纪律。

| ID | 假设及进入条件 |
| --- | --- |
| Z01 fork seed | 仅当冷启动/补池耗时或多实例私有脏页确实占主导时，试验单线程、CPU/只读资源模板；不得在母体中建立可继承的 Wayland/EGL/GL 活状态。与当前 `posix_spawn` 待命池比较 Preview/Master p50/p95、PSS、故障回收。 |
| Z02 Shader 持久缓存 | 先从 EGL 与首次 Render 子阶段证明着色器编译造成首帧或首次动画停顿，再在应用子进程内比较 Skia 缓存、驱动版本变化、磁盘占用和失败回退；不假定首次 Render 的约 106 ms 都是 shader。 |
| Z03 字体/DSL 共享 | 先量化文件页、解析、shaping 和私有脏页；memfd 共享原始字节不会自动共享 `FT_Face`、字形 atlas 或热主题。主题仍以版本化快照/ACK 更新，fork CoW 不负责向既有子进程广播。 |
| Z04 沙箱 | C ABI 不是权限边界。若引入不可信第三方应用，单独设计 UID/文件网络权限、模块进程隔离和受限 IPC；作为安全工程验收，不以动画 FPS 作为取舍标准。 |

启动基线与边界见[Master 并行加载](MASTER_PARALLEL_LOADING.md#13-第六步真实-music多区域与同管线实机对照)、
[待命 Worker 池](LAUNCHER_WORKER_POOL.md)。现有实机首次 EGL 约 98 ms、首次
Render 调用约 106 ms，但 shader/驱动/GPU 各自占比尚未分离；这不是每帧动画
的已测瓶颈。动画阶段优先在真实帧上找到成本，再决定缓存、预热和共享的价值。

## 7. 执行与更新规则

1. 不可变帧与独立渲染线程已接入生产。[动画运行时与 DSL 契约](ANIMATION_RUNTIME_SPEC.md)
   的首版时间核心、属性覆盖层、帧机会应答与测试夹具已接通；接下来完成逐帧关联、
   真实反馈时钟校验和 A01–A04 的 Pi 对照。测量用于调优，不审批是否采用双线程。
2. 先按[交互与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)建立稳定命中身份、
   输入生命周期、局部状态和呈现属性，再接入 A05 的小范围示例，优先选择 Dock 或
   Topbar 横线的视觉子树，保持固定感应区。视觉曲线、时长和减弱动效策略遵循
   版本化的主题/DSL 契约；组控制和分隔线手势后续接入，不写成当前可用接口。
3. 为 A06 实现真正的保留层与通用合成采样；A07、A10 的实际长尾决定分块缓存、
   效果优化和缓存预算的优先级。随后验证 A11、A12。每项保留同后端对照与内存代价。
4. 每完成一项，把“假设、测法、原始结果、是否证实、适用设备/主题、回退规则”
   写回本文件或对应现行契约。验收后才将属性与示例加入第三方应用 Skill。

本文件不修改已有 v9–v12 的历史结果。零拷贝、物理 direct scanout、GPU 完成时间
和 Windows Viewer 实际显示 FPS 仍需各自证据；不能用某一阶段计数代替另一阶段。

## 8. UI 与渲染线程分离：迁移前设计与验证要求（2026-09-29）

本节保留线程迁移前的架构论证与当时的事实快照。独立线程现已接入生产；
实现与 Pi 正确性验收以[客户端渲染线程迁移](CLIENT_RENDER_THREAD_MIGRATION.md)为准，
动画阶段以[动画运行时与 DSL 契约](ANIMATION_RUNTIME_SPEC.md)为准。

[原始架构设计第 26 节](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md#26-多线程)
提出由 UI 线程生成不可变 `FramePacket`，渲染线程消费。迁移前的生产提交链已用只读
`FramePacket` 传递 Scene 的绘制列表和 surface metadata；当时 Host 的一次 `Pump` 仍在同一
所有者线程中处理业务、Scene、Wayland 事件、Skia 绘制和 EGL Swap，尚无跨线程消费。
工作池只准备纯 CPU 结果。`SetBinding` 后的更新已能在下一次允许的像素提交前合并，
不能把现有工作池误称为渲染线程。

双线程交接是架构选择，不以 Pi 上先测出单线程瓶颈为实施条件。`FramePacket` 是跨线程
正确性交接所需的协议，不要求先交付一个长期运行的单线程快照架构。可以在同一迁移
过程中定义协议、用串行测试夹具验证语义，然后接通线程。逐帧测量用于验收与调优：
额外线程可能增加排队、唤醒、内存和上下文切换，GPU/WM 瓶颈也不会因 CPU 分线程
消失；这些结果决定后续优化，不否定已选的线程所有权边界。

| 所有者 | 目标职责 | 不共享的活对象 |
| --- | --- | --- |
| UI/Host 线程 | 业务状态、DSL/Scene、绑定、样式、布局、文字、输入命中；发布不可变场景版本与动画目标。 | 不把 live Scene 指针交给渲染线程。 |
| 渲染/协议线程 | 独占 WaylandWindow、EGL context、Ganesh、GPU 资源和 surface 像素提交；在可提交帧时消费已发布版本。Wayland 输入事件复制到 UI 队列。 | 不在协议回调中直接调用业务模块或修改 live Scene。 |
| 准备工作池 | 纯 DSL/资源准备与图片 CPU 解码，完成后交回对应所有者。 | 不创建或迁移活 EGL/Wayland/Scene 对象。 |

线程之间使用有界、不可变、带 UI/主题/surface/资源代数的帧快照；它携带
viewport/scale、DisplayList 或保留层引用、资源强引用、候选损伤、surface metadata
和可选动画描述。同一次可见变化的像素与效果、输入区域须使用一致代数并正确排序；
纯 metadata 仍可在旧像素回调未返回时单独 `State` 提交，沿用现有确认语义。
UI、主题、尺寸、scale 或资源代数已失效的快照不能提交，只有损伤历史不足等
可证明的情况才全量重绘。
渲染线程以**最后一次成功提交**为损伤与 buffer age 基线，而非最后一次 UI 生成的
版本。若它自行采样位移/透明度，还要用上次成功提交与本次采样的可见范围计算
旧∪新损伤，再按 buffer age 修复。积压时可合并尚未提交的普通视觉帧，保留最新
状态及所有未提交变化的损伤；
Preview/Master 呈现里程碑、configure/resize、主题事务、资源释放和关闭等有序事件
必须保持各自的确认语义，不能用“只保留最后一帧”抹掉。Master 纯准备可在 Preview
像素提交后启动，安装仍须等对应 Preview 实际 presented。资源引用保留到 renderer
和 WSI 确认不再使用；Swap 返回或 presentation feedback 单独都不是释放证明。
失败和取消也须完成对应清理；稳态中两线程不互等、不持有跨绘制过程的锁。

动画分两类：

1. **布局或内容变化**由渲染线程转发帧机会，UI 线程按单调时间更新 Scene 并生成
   新快照；渲染线程只消费已完成的版本。如果 UI 线程迟到，渲染线程不能凭旧
   DisplayList 推算文字、布局或业务状态，也不能靠多线程保证这类动画永不顿挫。
2. **稳定内容的位移、缩放和透明度**由 UI 线程决定目标、起点、曲线、时长和取消；
   在具备真实保留层之后，渲染线程可按估计的呈现时刻采样这些通用视觉参数，以免 UI
   线程为每个刷新周期发送数值。采样不得执行 DSL 解析、业务回调或重排。输入命中
   须定义与提交版本关联的逆变换和按下到释放的目标捕获；反馈可能晚到，不能声称
   总能精确知道用户屏幕当时显示的是哪一帧。首个横线动效维持静态命中范围，移动
   可点击节点待命中映射与同代裁剪、效果、输入区域同步后再开放。客户端保留层仍需
   在可见动画帧绘制合成结果并 Swap；它省去的是子树重复布局与光栅，不等于零提交。

渲染线程不能自行规定显示器刷新率。Wayland frame callback 是下一次像素提交的
节流信号，不是精确的未来 VSync 时间；presentation feedback 是事后结果。生产链
仍须按现有顺序在像素 Swap 前申请 callback/feedback，并遵守单个待回调像素帧和
反馈容量。活动动画才继续申请帧，完成后回到按事件等待；不能用固定 60/90 Hz
循环替代 compositor 的背压。
若一次采样因像素量化没有可见变化，不能期待新的 frame callback 自动到来；
调度器应计算下一次有意义变化的单次期限，期间不提交空白像素帧。

若由同一渲染线程派发 Wayland 输入并执行可能阻塞的 Swap，输入转发仍可能迟到；
拆线程不自动解决这一长尾。必须分别量输入协议事件年龄、队列等待和 UI 处理时间，
再决定是否需要更独立的协议事件队列。

| ID | 待验证事项 | 验证与验收 |
| --- | --- | --- |
| T01 帧快照正确性 | 不共享 live Scene，仅靠不可变快照保持提交与反馈语义。 | 迁移时用确定性串行夹具和跨线程测试覆盖 Preview/Master、主题、resize、失败回退、损伤和输入；这些正确性门槛必须在生产切换前通过。 |
| T02 性能效果 | 独立线程可能缩短 UI 排队，也可能因排队、唤醒和资源争用增加延迟；不预设帧率收益。 | 切换后在相同 Pi、主题、输出与场景中与旧版本对照；分别报告协议事件年龄、UI 队列等待、presented p95/p99、CPU/PSS 和丢弃帧，以结果调整队列、调度及缓存。 |

上述迁移前计划中的事件队列、资源生命周期与 Wayland/EGL/Ganesh 所有权切换现已完成；
动画帧机会、时间测量和保留层采样仍按新规范分阶段实施。生产链只保留一套线程
所有权和提交路径；旧版本仅用于历史对照。
