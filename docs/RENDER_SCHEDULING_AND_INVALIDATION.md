# 渲染调度与失效传播

日期：2026-09-27。先减少不必要的帧和效果工作，再收窄损伤范围，为后续动画建立可观测的帧预算。

## 1. 执行顺序

1. 按需 WM 输出调度、显式效果失效、绘制/提交/缓存工作计数。已完成并现场验收。
2. 毛玻璃依赖收窄到实际采样范围，维护视觉内容版本，避免不相交的下层变化传染所有上层效果。已完成，结果见第 8 节。
3. 区分 SDK 像素更新和 surface 状态提交；Host 统一等待 Wayland、控制通道、资源及 timer，减少固定轮询。已完成并现场验收，结果见第 10 节。
4. 补齐 buffer-age/damage 历史，实现安全局部像素绘制。已完成实现与自动实机门槛，结果见第 12 节。
5. 设计活动动画、单调帧时间和停止条件接口，再划分布局/绘制/合成更新路径。后续执行。

节点增量布局、分块缓存和视觉动画尚未进入本阶段。WM 不接收客户端 DSL、应用名绘制规则或 Skia 绘制列表。

第三阶段的提交与等待契约见第 9 节。像素帧节流、状态提交和实际呈现反馈保持各自语义；通过真实协议对象和 GPU 工作计数验证。

## 2. WM 帧需求规范

- 成功提交本身不请求下一帧。原生 scene 的内容 damage、可见 frame callback 和 backend 需求使用 wlroots 调度；布局/主题/效果变化有显式唤醒入口。
- 本机依赖 wlroots 0.18.2；不能调用新版才有的 `wlr_scene_output_needs_frame`。本轮按照该版本 scene commit 的 `output.needs_frame` / `pending_commit_damage` 门槛判断工作。
- callback-only commit 仍必须得到服务。wlroots 0.18 scene surface 对 enabled 且有 primary output 的待回调 surface 请求帧；WM 成功处理后继续派发 frame_done，不能仅以像素 damage 是否为空决定回调。
- headless backend 可能周期发 frame 事件；没有工作时提前退出，不扫描效果、不提交输出。不能把 backend 事件计数当作实际绘制次数。
- 普通鼠标移动交由 wlroots 的硬件光标更新或软件光标 damage；不每次重设相同光标、不主动强制所有输出提交。客户端 hover/点击带来的视觉变化由客户端提交。
- 每输出分别记录工作时间；长空闲间隔不作为旧物理模拟的巨大步长。未来动画必须明确活动需求、单调时间原点和结束后的停止条件，不能恢复永久帧循环。
- 输出启动、布局/显隐/层级、focus、主题、模式、HUD 和效果协议变化必须能在静态输出上唤醒。

## 3. 毛玻璃工作规范

`SurfaceEffects` 提供通用 `MarkDirty`、合并的 wake handler、`NeedsUpdate` 和 `UpdateResult`。WM 观察 compositor 创建的所有 surface，覆盖 toplevel、subsurface 和 popup；视觉提交、几何/层级/显隐、focus/theme 等标记效果失效。效果协议自身的 commit/destruction 也通知唤醒。

wlroots 0.18 的 surface 状态位不包含子 surface 位置/排序；父提交必须比较当前子表的节点、位置、层和顺序。XDG 几何比较包括局部原点，不能仅比较 BSP 的全局宽高。map/unmap 也显式失效，不依赖 commit 观察器执行时 `current_outputs` 已经更新。第一阶段保守接受所有视觉提交的失效，避免首次映射和 unmap 的监听器顺序遗漏；下一阶段再根据真实可见性和采样依赖缩小范围。

callback/input-only 状态不单独标记毛玻璃失效。没有 pending work 时不做 scene 遍历和缓存 hash。多个效果节点按客户端前方的最终序列幂等排列，不逐帧来回交换再恢复。

第一阶段沿用原 lower-scene 缓存键和下层效果 generation，保证背景正确性；仍可能在其他视觉事件触发检查时受不相交下层提交或旧 metadata seq 影响。第一阶段中所有效果失效保守唤醒输出；第二阶段按照第 7 节缩小依赖和输出范围。没有新增 GPU 等待、逐帧清缓存或降低已验收材质质量。

## 4. 计数接口与解释

`get_status.performance.scheduling` 为会话累计计数：

- `frame_events` / `idle_skips`：收到输出事件及提前退出次数。
- `scene_commit_calls` / `scene_commit_noops`：调用 scene commit，以及返回成功但 output commit_seq 没有变化的次数。
- `output_commits` / `output_buffer_commits`：实际 output commit 事件及其中包含 buffer 状态的事件；包含模式/光标/backend 状态，不等同全屏像素重画。
- `frame_done_dispatches`：派发函数调用次数，不是收到回调的客户端数量。
- `needs_frame_events` / `damage_events`：wlroots output 状态事件，不把它们当作全部 scene damage。
- `schedule_requests`：WM 主动布局、效果和模式唤醒次数；合并请求不等于同次数提交。
- `surface_commits` / `surface_buffer_commits` / `surface_callback_commits` / `surface_nonvisual_commits`：真实 surface 状态提交，不能冒充客户端 Skia 绘制次数。

`performance.effects_work` 包含实际缓存检查/命中/失效、capture/blur/material pass 尝试与成功提交、分配、发布结果区域/像素和排序次数。像素是结果 buffer 的完整范围（含 padding）；pass 成功提交不代表 GPU 已完成，分配时清理 pass 不计入效果绘制 pass。

SDK 独立提供只读 `ClientApplication::GetRenderStats()`，统计 Scene Build 调用/新列表/布局、GPU Render 和 Swap 调用/成功以及 frame callback。跨 Preview/Master 和 UI 替换累计，隔离主题预检不算实际前端绘制。该接口不引入 WM/SDK 交叉依赖，不扩展业务模块 ABI。

按需输出的 presentation interval 是相邻新内容呈现的时间差；静态画面保持期间的长间隔不意味着显示器停扫或动画丢帧。显示器模式刷新率、客户端绘制频率、WM 提交频率和活动动画的呈现间隔须分别报告。已有 `fps` 是工作事件估计值，不用作动画验收指标。

## 5. 验证与发布

- 测试和 raw Wayland probe 放在 tests，生产包不安装。独立静态 fixture 不启动周期时钟：观察实际输出提交、效果 pass/排序停止；callback-only 可继续 drawing；effect-only、双区域、focus/theme、模式、显隐和输入唤醒需有真实协议门槛。
- 功能门槛通过后，再比较相同主题、页面、采样和新会话的完整实机 CPU/PSS、提交数与效果工作量；停止构建、截图及其他探针后计时。
- 真实 GPU 完成耗时尚未新增；现有 CPU 回调耗时不能替代 GPU 时间。测试注入指针不作为真实硬件端到端延迟结论。

## 6. 第一阶段结果

2026-09-27 完成，生产版本 **0.1.0-9 arm64** 已安装到 Pi 4B Rev 1.5。没有改变 DSL 视觉设计或业务模块 ABI。以下为第一阶段完成时的记录，第二阶段规范见第 7 节。

- 最终 CTest **32/32** 通过。独立 headless 输出上的真实 V3D 调度 probe **25 个场景**通过，涵盖静态停止、六次 callback-only、恢复绘制、effect-only、双区域排序、下层内容、子 surface 首次映射/位置/排序/unmap/remap、XDG 原点变化及真实 Wayland 指针事件。
- 无周期业务的静态 fixture：1 秒观察中实际 output commit、效果检查、绘制和排序均为零增量。六次 callback-only 完成六次输出处理，客户端 surface buffer 提交为零，效果 update/check/render 均不增加。XDG 原点变化触发一次检查和实际输出提交，三个缓存检查命中且不重绘材质，符合自身不属于背景采样的边界。
- 安装后的真实 DRM/V3D 首帧与 Ready、同实例激活、新实例、取消回收、保留 Shell 拒绝和实例流通过。八种材质/明暗配色及错误事务保持当前主题通过；HUD 显隐可在静态输出上更新。SDK GLES probe 记录 Build 4、布局 3、Render/Swap 调用及成功各 4、收到帧回调 3，与其真实调用边界一致。
- 生产包检查：九个平台入口、五个业务包，不含 tests/probes/fixtures/ImGui；WM 无客户端 Scene、DSL/theme 前端和 Skia 符号。旧会话十个 PID 回收，`dpkg -V` 无差异。发布详情见 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)。

### 同条件实机短测

先对已安装的 0.1.0-8 建立新会话基线，再部署 0.1.0-9 建立新会话。均为 Glass dark、Preferences 外观页、监控开启 1s、Music 暂停，HDMI-A-2 1024×600 / 59.821 Hz，完整 session.scope。每组约 12 秒、13 次采样；停止构建、截图和功能探针后计时。指针发生器实际约 125 Hz、测量窗口各收到 1500 个事件、不发送点击。

| 条件 | 版本 | 会话 CPU（单核 100%） | WM CPU（单核 100%） | PSS 峰值 MiB | 实际呈现次数 |
| --- | --- | ---: | ---: | ---: | ---: |
| 空闲 | 0.1.0-8 | 4.97% | 2.75% | 217.09 | 717 |
| 空闲 | 0.1.0-9 | 2.15% | 0.42% | 218.54 | 12 |
| 125 Hz 指针 | 0.1.0-8 | 7.77% | 4.25% | 217.64 | 718 |
| 125 Hz 指针 | 0.1.0-9 | 6.07% | 3.33% | 218.57 | 718 |

0.1.0-9 两组均只有 12 次客户端 buffer 提交、12 次效果更新、48 次区域检查（36 hit / 12 miss）、12 capture / 24 blur / 12 material pass、497664 个结果 buffer 像素；分配、排序、失效区域和提交失败均为零增量。空闲时 clock 每秒更新一次，仍需呈现新内容。指针时有 717 次原生 needs_frame 事件，实际 buffer output commit 仍约 60Hz，但效果更新没有随着指针增长。这证明移除了 WM 自发循环与逐帧效果扫描，不能把鼠标期间的 718 次提交声称为已经消除，也不能将这些提交等同全屏所有像素重画。

本轮不是内存优化，PSS 基本持平。起止 CPU 频率均为 1.8GHz，温度约 43–46°C，历史 mask 0x50000 起止不变，起止无当前低压/降频标记。每个条件只有一次短测，CPU 值不代表长期分布；效果 pass 是 CPU 发起成功，不是 GPU 完成时间。按需输出的 240 样本窗口保留更久的启动/主题历史，不能与旧版约四秒滚动窗口直接比较 P95。

另在功能门槛后，通过真实 uinput 键盘启动 Music 播放，再测 12 秒：会话 CPU 5.66%、PSS 峰值 222.18MiB、36 次客户端像素/输出提交和效果更新、144 次区域检查（84 hit / 60 miss）、60 capture / 120 blur / 60 material pass、3034560 个结果像素，提交失败/分配/排序均零增量。播放图标与进度前后截图确认变化，之后暂停并恢复 Preferences 焦点。这是 v9 的动态功能负载记录，不与 v8 旧会话数据构成匹配比较，也不包含真实媒体解码。

用户随后静置并手动复核 Play/Pause、Dock 激活与明暗切换，现场答复“操作正常，鼠标更顺畅”。主观改善已验收，真实设备端到端延迟尚未量化。

### 后续重点

继续阶段 2 的采样范围、内容版本和效果依赖收窄，保证未来动画只更新真正受影响的背景。指针实测另说明原生光标/输出路径仍会请求显示帧：需要记录光标平面、实际 damage 和同一时间轴上的输入/呈现，定位硬件或软件光标路径及真实鼠标体感，不能通过降低交互刷新率掩盖，也不能因本轮 CPU 降低就宣布卡顿已修复。Host 固定轮询及状态/像素提交拆分按阶段 3 实施。

证据保存在 `dist/validation/prism-v9-render-scheduling/`：最终构建/CTest 日志、`gles-scheduling.json`、安装与物理协议记录、SDK 计数、配色事务、HUD、截图及 `performance-summary.json`/逐秒原始采样。初次构建的 probe 类型转换错误、初次 supervisor 认证错误与无效聚合测试保留在日志和 `run-notes.json`，不计入最终通过结果。上一轮历史性能结果见 [PREFERENCES_AND_PERFORMANCE.md](PREFERENCES_AND_PERFORMANCE.md)。

## 7. 第二阶段：采样依赖与内容版本

### 依赖边界

- 保持现有半分辨率 capture、两次 blur、完整结果 material pass 的视觉算法。每区域使用原有 padding 后的 capture 位置与实际分配范围；奇数尺寸按 `2 × ceil(result / 2)` 扩展，再在四边留 2 逻辑像素 guard。此范围是保守采样域，不宣称最小高斯核范围或精确圆角遮罩。
- 背景 hash 和 capture 使用相同的空间筛选及下层遍历终点；只纳入 enabled 祖先下与采样域相交的叶节点；不做 opaque 遮挡剔除。树仅用于遍历，范围外叶节点、自己的效果节点不影响键。相交叶节点的绘制顺序、实际位置、尺寸、裁剪、变换、滤波和透明度继续影响结果。
- 下层缓存效果只在与当前采样域相交时纳入其结果 generation，并从下到上计算。无 blur 的边框/阴影材料不依赖背景内容。主题按实际解析值失效，同值的新主题事务不单独重绘材料。

### 像素版本与 damage 历史

WM 在每次 surface commit 应用状态后、筛选 metadata 前，将有效逻辑 damage 和映射状态交给通用效果引擎。damage 深拷贝，不能延迟读取 wlroots 每次提交会覆盖的临时区域。销毁时清理 surface 身份，避免地址复用导致旧内容命中。

- 每 surface 有独立身份、内容 revision 和 mapping epoch；buffer/texture 地址及 callback seq 不是像素版本。
- 保留最近 32 次内容提交，每次最多 16 个逻辑矩形；超过数量合并保守包围盒。首次内容、由无 buffer 重新映射建立完整记录；相同映射的新 buffer 按客户端声明的 damage 判断内容变化，客户端必须正确报告所有改变的像素。
- 每个材料独立保存已观察 revision 和真正影响采样的 revision。范围外 damage 可推进观察位置但保持采样版本；相交 damage、映射变化、历史缺口或无法证明的映射均保守失效。
- 简单 scene clip 与有效逻辑 damage 在同一空间映射；有效 damage 已包含 buffer scale/transform/viewport，不能再应用一次。复杂 viewport 与 clip 组合、非整数 viewport source 均保守完整失效：wlroots 0.18 的有效 damage 对 source 有取整，不能据此证明范围外像素未变。OFFSET 提交也保守推进 mapping epoch；不自行将 offset 加到原生 scene 坐标。
- Capture 使用与原生 scene 相同的逆 buffer transform 和 filter mode；逻辑 damage 与真实采样必须对应，覆盖 90°/270° 旋转边界。
- 候选依赖只在缓存命中或整个区域成功生成后发布；失败时保留旧观察，下一次检查不得吞掉仍未绘制的更新。
- callback/input/opaque metadata 不独立推进像素版本；几何、层级、显隐、focus、效果协议及实际样式变化继续通知检查。

### 检查与输出调度

效果失效合并为一个 Wayland idle 工作项。检查若只命中缓存，不请求所有输出呈现；新增、移除、移动或替换效果节点通过原生 scene damage 唤醒相交输出。backend frame 若先到，则先处理待检查工作并取消 idle。idle 注册失败才保守调度已启用输出，Stop 在销毁客户端前解除 wake handler 并移除 idle。

callback-only 的原生 `needs_frame` 路径及 frame_done 保持第一阶段语义。idle 内的效果 CPU 仍计入 `effects_cpu`，但不计入输出回调的 `frame_cpu`；backend 先触发的检查则同时包含在回调耗时中。不能仅凭 `frame_cpu` 降低宣称总耗时下降，应比较完整会话 CPU 和实际工作计数。

新增 `effects_work` 累计计数：`dependency_leaves_checked/included/skipped`、`content_revisions`、`metadata_commits`、`damage_history_fallbacks`、`partial_damage_cache_hits`、`capture_nodes`。叶节点计数按区域依赖检查记录，capture nodes 为实际排入 pass 的 draw，含失败尝试；范围外 damage 的 cache hit 只在成功复用时记录。`metadata_commits` 表示此次未创建内容 revision，也包含空 damage 的 buffer 替换或仅 epoch 变化；`damage_history_fallbacks` 记录历史/映射回退，不包含所有未知 generic buffer 的保守 miss。它们不是 GPU 耗时、遍历树次数或内存占用。

### 验证门槛

纯算法测试覆盖有界历史、观察推进、身份/映射变化、失败重试、分数边缘与奇数 capture 尺寸。真实 Wayland/V3D probe 验证同一大 surface 的范围外/范围内局部 damage、新 buffer、metadata 后续更新、子 surface、padding 边缘和下层材料传播，并直接读取测试输出的小块像素作为画面门槛。探针与素材只在 tests，不安装进生产包。

先在新会话测已安装 v9 的空闲与 Music 更新负载，再用同样流程测 v10；每组约 12 秒，停止构建、截图和其他功能探针后计时。CPU 按单核 100% 表示，PSS 不包含全部 GPU 显存；DRM resident 按设备/client 去重，共享 BO 跨 client 仍可能重复，不能作为唯一物理显存总量。短测只说明此负载下的工作量，不构成长期分布或真实媒体解码性能结论。节点增量布局、分块缓存、局部像素绘制和动画仍按后续阶段推进。

## 8. 第二阶段结果

2026-09-27，**0.1.0-10 arm64** 已安装到同一 Pi 4B。保持已验收的材质算法、模板和会话启动链；增加空间依赖、有界内容历史和 idle 检查。下一阶段为 SDK 状态/像素提交分离及 Host 事件等待。

### 功能与像素验证

- 最终 CTest **33/33** 通过。真实 V3D headless probe **50 个功能场景**通过，另有 4 条图案/坐标诊断记录（报告共 54 条）。19 个静态观察窗的真实 output commit、效果 update/check/pass/reorder 均零增量；6 次 callback-only 完成 6 次 output commit/frame_done，无客户端 buffer 提交或效果更新。
- 同一 Desktop surface 换新 buffer，只更新范围外小块：每次 3 hit、0 pass，玻璃 ROI 完全不变；两个有实际背景依赖的区域推进观察记录。近处 damage 只产生 1 capture / 2 blur / 1 material，其余两区域命中，ROI 确实改变。padding 内、面板外的变化也更新正确边缘，未将依赖裁到面板边界。
- 非对称图案在普通、90°、270° 下的玻璃 ROI 均为 RGB `[177,81,102]`；旋转后远处逻辑 damage 保持玻璃像素，近处只更新一个采样区域，裸 Desktop 的颜色位置也一致。Capture 的 inverse transform/filter 与原生 scene 对齐。
- OFFSET 无 buffer/像素 revision 增量，只有 metadata 和 mapping epoch；产生 2 次保守 capture，证明映射失效独立于内容版本。几何 reserve 仅检查、3 hit、无输出提交；局部原点变化仍 3 hit、无材料重画，但原生客户端采样位置改变并实际提交一次。
- 下层 App 的缓存阴影在同一 buffer 内改色，可信透明 Dock 的 ROI 从 `[82,65,82.83]` 到 `[113,65,51.5]`，客户端 buffer 提交没有增加。该检查 16 个叶节点中只纳入 5 个，证明下层 paint generation 继续传播至相交的上层效果。
- 安装后的 DRM 首帧/Ready、同实例激活、新实例、取消回收、保留 Shell 拒绝及真实实例流通过；八种材质/明暗配色及拒绝事务通过。物理 Glass dark 截图确认双窗、Dock、玻璃、阴影和图标正常；用户现场确认“画面和操作都正常”，上述主要操作及玻璃/阴影边缘验收通过。

### 同条件实机短测

先启动已安装 v9 的新会话建立基线，再安装 v10 启动新会话。均为 Glass dark、Preferences 外观页、监控开启 1s、Music 暂停或模拟进度更新，HDMI-A-2 1024×600 / 59.821Hz，完整 session.scope。每组约 12 秒、13 次采样；测量期间无构建、截图或其他功能探针，未注入鼠标运动。

| 条件 | 版本 | 会话 CPU（单核 100%） | WM CPU（单核 100%） | PSS 峰值 MiB | 呈现次数 | capture / blur / material |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 空闲 | 0.1.0-9 | 2.46% | 0.42% | 217.83 | 12 | 12 / 24 / 12 |
| 空闲 | 0.1.0-10 | 2.60% | 0.33% | 218.09 | 12 | 0 / 0 / 0 |
| Music 更新 | 0.1.0-9 | 5.29% | 1.17% | 218.59 | 36 | 60 / 120 / 60 |
| Music 更新 | 0.1.0-10 | 4.49% | 0.50% | 218.83 | 36 | 0 / 0 / 0 |

v10 空闲仍检查 48 个区域且全部命中；Music 检查 144 个区域且全部命中。依赖叶节点分别检查 192/576 个、纳入 120/360 个、跳过 72/216 个。两组的分配、排序、失败和完整效果结果像素均零增量；客户端 buffer 与真实输出仍分别更新 12/36 次，因此没有冻结时钟或进度来得到零效果工作。两组 `partial_damage_cache_hits` 均为零：本负载改变的是采样域外整棵客户端叶节点，首先被几何筛选跳过；同一大 surface 的局部历史复用由上面的 raw Wayland 像素门槛证明。

本次 Music 会话 CPU 观察值约降低 15%，WM CPU 约降低 57%；空闲会话 CPU **未降低**。PSS 基本持平。四组温度起止约 41–44°C、历史 mask 0x50000 不变且无当前低压/降频标记。使用 ondemand governor；v10 空闲结束采样为 1.1GHz，其余起止采样为 1.8GHz，不能宣称全程固定频率或把空闲差值归因于单一代码路径。每条件只有一次短测，不构成长期分布、真实媒体解码或鼠标端到端延迟结论。idle prepass 改变了 `frame_cpu` 的计时边界，不能与第一阶段直接比较该项 P95。

本阶段消除了这两种负载下的额外玻璃 pass，尚未消除依赖检查、客户端像素绘制或输出呈现。相交背景动画仍需完整重做受影响区域；节点增量布局、分块缓存、精确核范围、局部绘制和动画时间接口继续后续设计。

证据位于 `dist/validation/prism-v10-spatial-effects/`：最终构建/CTest、`gles-final.json`、包清单/哈希、部署回收、物理协议与配色、实际截图、`performance-summary.json` 和逐秒原始记录。早期 reserve 强制提交预期、旋转 fixture 方向及 OFFSET 错误计数预期已修正；原始失败/诊断与处理理由保留在 `run-notes.json` 和日志，不计入最终通过结果。生产包继续不包含 tests/probes/fixtures/ImGui；WM 无客户端 Scene、DSL/theme 前端及 Skia 符号。

现场操作期间（11:08:25）另记录一次 libinput 报告 Razer Orochi V2 的事件处理积压约 50ms。该时段在两组性能短测之后，尚未归因；后续需把输入到达、WM 事件循环工作及真实呈现放到同一时间轴，区分主题切换/驱动等待与稳定负载，不能将其混入短测 CPU 数值或宣称鼠标端到端延迟已经解决。


## 9. 第三阶段：SDK 提交与 Host 等待契约

### 四种提交结果

平台适配器提供 `SubmitResult::{None,State,Pixels,Failed}`，准备阶段与实际像素提交分开。准备阶段解析当前通用状态并按需生成显示列表/Render；只有确定为 Pixels 后才能申请 frame callback 和 presentation feedback，再执行 EGL Swap。

- **None**：没有需要提交的 surface 状态或像素，不 commit、不申请 callback/feedback、不 Render/Swap。
- **State**：configure ACK、输入区域、背景效果等 metadata 复用当前 buffer，只做 `wl_surface_commit`；没有新像素回调或 feedback。旧像素 callback 未返回也不阻塞 metadata 提交。
- **Pixels**：首帧、真实尺寸变化、可见像素/布局失效才执行 Build/Render/Swap，并为本次像素提交安排 callback 与真实 presentation feedback。同尺寸 configure 仍确认，但不销毁旧像素 callback 或强制重画。
- **Failed**：提交路径终止，按 Skia → EGL → Wayland 释放资源。失败不得确认未成功提交的内容或继续循环重试。`wl_callback_destroy` 只销毁客户端代理，不能撤回服务端 pending request；Swap 失败后的未提交请求必须随 surface/连接清理，不允许随下一次 State 意外提交。

Scene 的 `PixelsRevision()` 与显示列表 generation、surface commit、callback seq 独立。可见 Paint/Layout 改变推进像素版本；Composite-only 不 Build、不推进像素版本。Build 消耗 Layout/Paint，保留 Composite；平台成功提交后 `AcknowledgeComposite()`。已校验且 metadata 请求相同或可选扩展不可用的 None，也可确认对应 Composite；未经准备的 None 和 Failed 不能确认。SDK 记录最后成功提交的像素版本，已构建列表不能被误认为已经 Swap。重复 binding、相同解析样式的新主题 generation、同尺寸 viewport 不单独重画；主题身份仍更新，引用、效果/输入预检及失败保持旧主题的语义保留。

Ganesh 的释放必须使用创建它的 EGL 上下文；SDK 清理先切换到本实例上下文，不可删除另一个客户端同编号的 GL 资源。上下文不可用时先 abandon 再析构，随后释放 EGL 与 Wayland。后端同时校验 Render/Close 的上下文身份，支持同一线程交替使用多个客户端。

本轮 Composite-only 只覆盖属性表中已实现的 backdropBlur/inputShape 等 surface metadata。未来客户端内部 transform、opacity 或离屏 layer 合成须定义独立的合成需求与像素失效，不可仅因标为 Composite 就跳过 GPU 或误当 Wayland State。

本阶段仍完整绘制有像素失效的 surface，不声称局部绘制。EGL/WSI 需要的首帧或 resize 可以复用没有改变的显示列表，不以 Build 次数代替像素提交次数。

主题候选仍执行独立的解析、引用、布局/效果预检；相同解析样式不重画不等于零 CPU 预检。控件规则改变当前保守地使整棵 Scene Paint 失效，进一步可见性收窄留待后续测量。

### 统一等待

`WaylandWindow`、`ClientApplication`、`AppHost` 使用 `Pump(timeout_ms, wake_fds)`；负 timeout 表示无限等待，非负值为调用者等待上限。外部 descriptor 借用，不由 SDK 读取或关闭；`revents` 回填调用者。SDK 内部加入资源完成 FD，Host 加入 launcher 消息 FD，生产主循环加入控制和退出通知 FD。

- Wayland 适配器独占 `prepare_read / flush / poll / read_events 或 cancel_read`。所有 prepare 路径均有对应 read/cancel；外部唤醒后先解除读准备，再执行资源或业务回调。flush EAGAIN 才等待 POLLOUT，不永久关注可写 socket。
- 已派发的 Wayland pending 输入可能创建 launcher 通道、修改 timer 或新增待写消息；派发后返回 Host 重新收集 FD 和 deadline，不用过期集合进入无限等待。不能用固定轮询作为遗漏唤醒的兜底。
- 图片 worker 只解码，完成队列与 nonblocking CLOEXEC eventfd 的 push/signal、swap/drain 在同一锁下完成；所有资源注册和 Scene 更新仍在运行时线程。销毁先 join worker 再关闭通知 FD；旧 UI 的完成结果继续按资源身份过滤。
- 消息流每轮的帧数上限保持。完整用户态缓存帧视为立即工作；仅不完整片段必须继续等 socket，避免剩余消息滞留和片段空转。
- 业务 ABI v1 已有 one-shot `schedule_tick`，保持接口不扩展。等待上限取业务 tick、Host 启动 10 秒 deadline、launcher watchdog/事务超时与调用者上限的最早值。到期只执行一次，模块主动重排下一次；Music 暂停停止重排，恢复播放重新安排更新。
- SIGTERM/SIGINT、子进程退出通过可轮询通知唤醒。处理检查 flag 到等待之间的竞态，也覆盖信号落到资源线程的情况。控制取消、主题分发/ACK、实例事件和预热池回收继续沿用现有启动协议。

### 计数与验收

SDK 累计 `surface_noops/surface_state_commits/surface_pixel_commits/surface_submission_failures`，保留 Build、Render、Swap 与 callback/实际 presentation 的独立计数。None 计数是检查结果，不是 GPU 工作或唤醒次数；State commit 不代表新增内容呈现，Swap 成功也不代表 GPU 完成。

必要门槛：真实协议 fixture 扣留 pixel callback，验证 State 仍可提交；None/准备失败不申请对象，像素提交失败关闭并清理 pending 请求；随后像素更新与 resize 不被误跳过。资源完成 FD 的就绪/耗尽、one-shot timer、缓存完整帧边界单独验证；真实 GLES 验证重复值/纯状态零 Render/Swap，像素改变恢复绘制。安装后复测 Preview/Master、Ready、激活、取消、主题/配色和预热链。

同条件实机比较使用新会话、Glass dark、Preferences 外观页、监控开启 1s、Music 暂停/模拟播放，各约 12 秒。停止构建、截图和其他探针后计时；报告完整会话 CPU、PSS、实际输出及效果工作。测试 fixture 与 probe 只留在 tests，生产包不安装；不把减少等待空醒声称为输入端到端延迟或局部像素绘制已经完成。

## 10. 第三阶段结果（0.1.0-11）

2026-09-27 在同一 Pi 4B ARM64 完成构建、部署与现场验收。生产链仍为 session → launcher → 统一 host；模块 ABI v1 和 Preview/Master 同 surface 保留。发布包、安装与计数证据位于 `dist/validation/prism-v11-sdk-event-wait/`。

### 正确性门槛

- 完整 CTest **36/36**，增加真实 Wayland 提交 fixture、资源 FD、one-shot timer 与信号等待门槛。服务端扣留 pixel callback 时，State 仍 commit；None/准备失败零对象，Swap 失败实际已发出的请求随连接清理，终态不重试。旧生命周期测试继续验证第二帧、输入、关闭与真实 resize。
- 真实 **V3D 4.2.14.0** SDK 验证通过：静置、重复 binding/解析样式和仅主题身份变化零 Build/Render/Swap；纯 backdrop metadata 包括 pixel callback pending 时零 Render/Swap；像素变化、BSP resize、关闭第二客户端后的尺寸恢复继续绘制。两次外部通知均能打断无限等待，且不制造像素帧。独立图片门槛为 `images=1/1`。
- 修复该多客户端验证暴露的上下文清理错误：交替使用两个 EGL 上下文后，关闭 peer 不能在另一个上下文删除 Ganesh 对象。当前先切本实例上下文；失败则 abandon，后端也校验身份。
- Host、launcher 池及完整 session 回归通过：Preview/真实首帧/Ready、错误 ABI、warm/cold、缓存与畸形消息、公共/模块启动、取消、崩溃、启动 watchdog、父退出和回收。合法自报 Failed 允许 250ms 自行清理退出，随后 TERM → 1s KILL；取消、watchdog、shutdown 仍立即 TERM，Exited 保持真实 waitpid 状态。修正旧 failed-worker fixture 的主题 ACK 后，原自报失败/忽略 TERM 的阻塞清理门槛通过。
- 带继承 helper 的正常、WM/launcher/Shell 崩溃四种会话清理全部通过。WM 真实 GLES 回归为 **50 功能场景 + 4 诊断记录**。
- 安装后 DRM 首帧、V3D/Ready、激活、新实例、取消、Shell 保留身份拒绝及实例流通过；八种材质/明暗组合及错误事务通过。旧十个进程身份已回收，`dpkg -V` 无差异，服务正常。物理截图确认图标、双窗、玻璃与阴影；用户确认“画面和操作都正常”，覆盖 Music、Dock、主题及明暗操作。

### 同条件实机短测

先重启安装的 v10 建立基线，再安装 v11 启动新会话。两版均为 Glass dark、Preferences 外观页、监控开启 1s、Music 暂停或模拟进度更新，HDMI-A-2 1024×600 / 59.821Hz；完整 session.scope。每条件约 12 秒、13 次采样，无并行构建、截图、其他功能探针或鼠标注入。

| 条件 | 版本 | 会话 CPU（单核 100%） | WM CPU（单核 100%） | PSS 峰值 MiB | 实际输出提交 | capture / blur / material |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 空闲 | 0.1.0-10 | 2.10% | 0.17% | 218.26 | 12 | 0 / 0 / 0 |
| 空闲 | 0.1.0-11 | 0.76% | 0.25% | 218.08 | 12 | 0 / 0 / 0 |
| Music 更新 | 0.1.0-10 | 4.56% | 0.50% | 219.04 | 36 | 0 / 0 / 0 |
| Music 更新 | 0.1.0-11 | 2.98% | 0.50% | 218.62 | 36 | 0 / 0 / 0 |

会话 CPU 观察值分别约降低 64%/35%，WM 未观察到收益，PSS 基本持平。两版客户端 buffer 更新和输出提交均保留 12/36 次，效果依赖检查/缓存命中仍为 48/144 个区域，分配/失败/额外玻璃 pass 均零增量。真实 backend present 事件分别为 v10 的 12/36、v11 的 12/35，discarded 均零；各计数按测量端点取差，不把 36 次提交写成 36 次完成呈现。

另在每版暂停静置时单独观察约 12 秒主线程自愿上下文切换，进程身份稳定：

| 进程 | v10 次数 | v11 次数 |
| --- | ---: | ---: |
| supervisor | 599 | 0 |
| launcher | 240 | 0 |
| 暂停 Music host | 601 | 0 |
| Desktop host | 598 | 0 |
| Topbar host | 712 | 163 |
| Dock host | 598 | 0 |
| Preferences host | 634 | 46 |
| 预热 host | 12 | 0 |
| WM | 132 | 141 |

这些是主线程调度观察，**不是精确唤醒次数**；Topbar 正常更新时钟，Preferences 保留 1s 监控采样，渲染/驱动等待也会产生切换。该结果与移除生产固定轮询一致，但不推导 GPU 工作完成或输入延迟。

四组温度起止约 41.9–43.3°C，ondemand governor，起止均 1.8GHz；mask 0x50000 历史位不变，无新增或当前降频标记。只记录起止频率，不能声称全程固定频率。每条件一次短测，Music 为模拟进度，没有真实媒体解码；PSS 不含完整 GPU 物理内存。鼠标端到端延迟和第 8 节记录的现场输入积压尚未专项归因。

原始证据包括 `ctest-release-gate.log`、`gate-summary.json`、`sdk-submit-final.log`、`sdk-image-final.log`、`wm-gles.json`、`runtime-gates-release.json`、包审计、部署/物理/配色记录、截图、`performance-summary.json`、逐秒报告及两版主线程切换记录。初轮构建/旧断言/真实清理错误和 fixture 问题保留在日志与 `run-notes.json`，不混入最终通过门槛。发布包继续九个平台入口、五应用包，无 tests/probes/fixtures/ImGui；包内 WM 与已审计构建的 strip 后产物一致，无客户端 DSL、Scene、主题编译前端或 Skia 符号。

### 下一阶段

按第 1 节第四步先定义 EGL buffer age、损伤历史、坐标/裁剪和完整重绘回退，验证轮转 buffer、resize、透明/圆角/阴影及资源完成的像素正确性，再安全缩小客户端像素绘制范围。0.1.0-11 每次 Pixels 仍完整绘制该 surface；第四阶段结果见第 12 节。节点增量布局、分块缓存、动画时间接口与零拷贝验证继续后续设计。

## 11. 第四阶段规范与执行顺序

1. 建立通用整数 buffer 损伤契约、有界成功提交历史和 EGL 能力查询。内容损伤表示与上一成功提交相比的变化；修复区域合并当前变化和轮转 buffer 缺少的历史。State/None 不推进历史，失败不记录成功；尺寸/WSI epoch 变化拒绝旧计划。首帧、age 0/未知/超历史范围、无法证明的边界回退全量。
2. 当前完整 DisplayList 与上一成功列表比较，覆盖改变的旧/新实际绘制范围。使用字形真实 ink、同一图标绘制路径和 Skia paint 的阴影扩展；裁剪后向外取整为 buffer 像素。结构/clip/非单位 transform 改变以及资源版本变化保守全量。继续生成完整列表，不在本轮引入节点增量布局或分块缓存。
3. renderer 在整数修复区域的联合 clip 内以 Src 清为透明，再按原序列重放完整列表；区域外保留旧像素。透明、遮挡、移动和删除必须对照同后端全量绘制的整张目标，并满足下述覆盖、保留及数值门槛。不能只重放改变的命令或仅清新位置。
4. 严格按 MakeCurrent/Resize → QueryBufferAge → Plan → SetDamage(repair) → Render → Swap(content) 排序。平台负责 EGL 底左坐标转换；内容与修复不得混用。Swap 成功后通过预分配 ring 和 move 更新历史/列表，不在不可撤回的提交之后分配；失败清理 WSI 并终止。没有能力时使用同一 renderer 的完整修复/普通 Swap 回退。
5. 独立测试先覆盖 1/2/3 buffer 轮转、历史不足、resize、失败、透明/阴影/字形/图标/图片与资源替换；真实 Wayland/V3D 再验证实际能力、age、内容损伤和尺寸恢复。报告修复面积、full/partial/empty、Build/Render/Swap 各自计数。最后打包并执行物理启动、主题/配色、交互及同条件短测，验收后记录结果。

`DamageRegion` 使用左上原点的整数 buffer 像素；`full=true` 权威表示整张目标，rects 为空。`full=false` 且没有 rect 表示内容未变，不能解释成未知 buffer。EGL 的 n_rects=0 则表示全表面；平台用一个零面积矩形表达空内容变化，避免与 full 混淆。

历史默认八次成功像素提交；区域默认最多十六个矩形，覆盖过大或碎片过多升级全量。age N 的修复为当前内容变化加最近 N−1 次成功内容变化；历史中记录 content，不记录已扩大的 repair。完整修复不冒充新的内容变化。所有区域都需保守包含已绘制像素；真实绘制面积计数是修复区域面积，不是 GPU fragment 或实际耗时。

生产当前 scale 为 1，逻辑与 buffer 像素一一对应；其他 scale 暂用完整回退，后续完善坐标映射。窗口重新创建、resize、UI 替换与资源重新注册均不能复用不可靠的像素历史。通用 `partial_rendering` 策略开关保留同一 renderer/损伤契约，可用于完整修复对照与回退，不建立第二套前端。

GLES 生产后端在 repair 联合区域内真正裁剪重放。CPU raster 对照后端有单独的保守策略：Skia 软件 AA 曲线在完整与裁剪路径中可能出现固定点舍入差异，因此 partial Render 在临时全尺寸目标中完整重放，再仅复制 repair 区域；区域外逐字节保留。此回退不用于 GLES Replay，不声称降低 CPU raster 绘制工作，也不属于节点/分块缓存。

像素验证采用三个独立门槛：连续的完整参考图在 **content damage 外逐字节相同**，确认没有漏掉真实内容变化；同一个实际 buffer 在 **repair 外绘制前后逐字节相同**，确认没有越界清除/绘制；整个 GPU 目标与独立完整参考逐像素比较，只有双方 alpha 都非零时允许每个 RGBA 通道最多 1/255 的数值差异，任何零/非零 alpha 转换及透明参考像素必须完全一致。CPU 和无纹理的 80% repair sentinel 始终逐字节比较。每帧记录有差异的像素数、最大 RGBA/alpha 差值，不能用全图平均误差放过漏绘。

该有限 GPU 数值边界源于固定 Skia 的双线性纹理优化：`SurfaceDrawContext::attemptQuadOptimization` 将裁剪域折入 quad，`GrQuadUtils::crop_simple_rect` 重算局部纹理坐标，浮点乘加顺序随修复区域变化。验证已实际遇到 RGB 和 alpha 各一阶差异；这种差异可以随已修复 buffer 保留，故 repair 外的严格检查比较实际旧 buffer，而不要求其与重新采样的参考图字节相同。真实漏绘（例如 alpha 119 变 0）仍会失败。此口径不将数值变化宣称为绘制次数、GPU 时间或零拷贝收益。

本阶段明确普通非 AA `PushClipRect` 的像素语义：canvas 总矩阵为单位矩阵时，逻辑矩形向外取整为整数 buffer 覆盖，再执行硬裁剪；完整/局部 Replay 与损伤比较使用相同规则。它会保守包含原分数边界外最多一像素，避免与细窄 repair 相交时触发 Ganesh 不同取整分支。非单位矩阵继续使用既有变换裁剪且损伤全量回退。AA 圆角裁剪保持浮点几何，损伤以保守 AA 包络求交；图元的 AA/字形 guard 必须在 clip 求交前加入，不能先把处在边缘覆盖中的图元误判为空。

规范依据：[EGL EXT buffer age](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_buffer_age.txt)、[KHR partial update](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_partial_update.txt)、[KHR swap buffers with damage](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_swap_buffers_with_damage.txt)。SetDamage 必须在 age 查询后、绘制前且每帧一次；已声明的修复域之外绘制会使 framebuffer 内容未定义。Swap 损伤是优化提示，成功不等于 GPU/显示完成，也不证明零拷贝。

## 12. 第四阶段结果（0.1.0-12）

2026-09-27，前三阶段已提交为 `13768ec`。本轮实现成功像素快照比较、通用 content/repair 契约、有界成功损伤历史、真实 EGL buffer age 与 damage 提交；0.1.0-12 arm64 已打包安装在同一 Pi 4B。统一 session → launcher → host、业务 ABI v1 和 WM/前端边界保持。默认 GLES 路径启用局部修复；缺乏可靠历史时完整回退。规范、坐标语义、CPU 对照策略与 GPU 数值边界见第 11 节。

### 正确性与部署

- 完整 CTest **38/38**；新增整数区域/历史、无分配成功发布和 Skia 像素门槛。模拟 1/2/3 buffer 轮转各 75 帧，分别 partial/full/empty 为 49/14/12、49/17/9、48/19/8。覆盖透明擦除、移动/删除、重叠、27 种图标、真实字形 ink、内外阴影、图片采样/替换、clip/transform/结构变化、历史不足和 resize。
- **V3D 4.2.14.0** 真实 Wayland EGL 目标验证 74 个场景、75 次整目标读回，partial/full/empty 为 49/16/10；实际未知 age 三次，诊断覆盖另三次，分开计数。连续完整参考的 content 外覆盖检查和实际 buffer 的 repair 外保留检查都逐字节通过。整个 GPU 目标遵守第 11 节透明像素精确、双方非透明 RGBA 每通道最多 1/255 的规则；不能称整图字节完全相同。最终模拟/真实日志的最大 RGBA 与 alpha 差值均为 1。
- Pi 实际能力为 buffer age **1**、swap damage **1**、partial update **0**，SDK 更新时实际 age 为 2。本次验证的是 EXT buffer age 与 swap damage 路径；KHR SetDamage 的排序/能力守卫已实现，不能宣称在该驱动执行了 KHR partial update。
- SDK 无操作/纯状态/等待回归通过，重复值与静置零 Render/Swap；State 不推进像素历史。真实多客户端关闭、BSP resize 和恢复继续提交。图片门槛 `images=1/1`。Host、预热池、失败清理、继承 helper 的四种 session 生命周期及 WM 的 **50 功能场景 + 4 诊断记录**通过。
- 最终包审计为九个平台入口、五业务模块包，无测试/探针/fixtures/ImGui。包内 WM 与已审计 Release 构建的 strip 产物相同，无客户端 DSL、Scene、主题编译前端或 Skia 符号。`dpkg -V` 无差异；重启回收旧进程身份。已安装 DRM 首帧/Ready、激活/新实例/取消、Shell 身份拒绝及实例流通过；八种材质/明暗组合、错误事务和 selector replay 通过，恢复 Glass dark。
- 截图检查确认双窗、图标、文字、玻璃与阴影正常；采样和截图分开进行。最终会话 journal 未见 `[ERROR]`、`commit=FAILED`、SDK 提交失败或输入积压。人工现场点击验收待用户答复；自动检查不代替鼠标端到端延迟测量。

### 真实 SDK 修复范围对照

同一后端、同一完整列表序列，Auto 与强制完整修复各提交 24 次更新。剔除启动两帧，统计如下；完整策略仍比较内容损伤，只有 repair 强制全量。

| 策略 | 成功 Swap / 历史提交 | partial / full | 内容损伤像素 | 修复像素面积累计 |
| --- | ---: | ---: | ---: | ---: |
| Auto | 24 / 24 | 23 / 1 | 130,965 | 1,052,815 |
| 完整修复 | 24 / 24 | 0 / 24 | 130,965 | 22,118,400 |

修复覆盖面积约减少 **95.24%**，包括一次 buffer 不可靠时的完整回退。两组 Build/Render/Swap 次数仍相同；该结果是整数区域面积，不是 GPU fragment 数、GPU 耗时或减少帧数的结论。后续动画能从较小的像素变化区域受益，但当前仍遍历/比较完整列表，并在 repair clip 内按顺序重放。

### 同条件生产会话短测

重新启动安装的 v11 建立基线，再升级 v12 并以干净会话测量。HDMI-A-2 1024×600 / 59.821 Hz；Glass dark、Preferences 外观页、监控开启 1s、Music 暂停或模拟进度更新。每条件一次约 12 秒、13 次采样，完整 session.scope；测量期间无构建、截图、其他探针或鼠标注入。

| 条件 | 版本 | 会话 CPU（单核 100%） | WM CPU（单核 100%） | PSS 峰值 MiB | 输出提交 / 实际 presented | capture / blur / material |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 空闲 | 0.1.0-11 | 0.69% | 0.17% | 218.40 | 12 / 12 | 0 / 0 / 0 |
| 空闲 | 0.1.0-12 | 1.02% | 0.42% | 218.74 | 12 / 12 | 0 / 0 / 0 |
| Music 更新 | 0.1.0-11 | 3.07% | 0.50% | 218.94 | 36 / 36 | 0 / 0 / 0 |
| Music 更新 | 0.1.0-12 | 1.91% | 0.50% | 220.10 | 36 / 36 | 0 / 0 / 0 |

Music 场景会话 CPU 观察值约下降 **38%**，WM 持平；空闲会话 CPU 观察值上升，不能宣称本轮改善空闲性能。PSS 没有下降，Music 峰值增加约 1.16 MiB。两版客户端 buffer/输出提交保持 12/36 次，效果检查/缓存命中保持 48/144 区域；没有新增玻璃 pass、分配、失败或 discarded。本轮单次短测不建立统计显著性，也不能把 CPU 差值全部归因 GPU 修复面积。

四组温度起止约 41.4–42.8°C，ondemand governor；v12 空闲起止频率从 1.8GHz 到 700MHz，其他起止均为 1.8GHz，不能声称全程固定频率。throttled mask 0x50000 历史位不变，没有新增或起止当前降频位。Music 为模拟进度，不包含媒体解码；PSS 不包含完整 GPU 物理分配，输出提交、Swap 和实际 presentation 的计数各自独立。GPU 时间、真实动画预算和鼠标端到端延迟尚未测得。

证据位于 `dist/validation/prism-v12-buffer-damage/`：`ctest-release-gate.log`、模拟/真实像素日志、`sdk-native-gates.json`、`runtime-gates.json`、`wm-gles.json`、最终包审计、部署/现场/配色记录、`physical-glass-dark.png`、journal、`performance-summary.json`、逐秒报告与 `gate-summary.json`。`run-notes.json` 保留 CPU AA 舍入、GPU 硬裁剪漏绘及纹理量化、测试模板和启动等待问题及修复依据。第一次部署脚本在 launcher socket 就绪后过早断言主题；初始失败记录保留，最终脚本等待实际主题 generation 后重启采样。失败证据不计入最终通过门槛。

### 下一步接口顺序

先定义通用活动动画请求、单调帧时间、截止时间和完成/取消接口。Host 只在存在活动动画、有效像素需求或真实事件时等待/请求下一帧；动画结束必须返回静置等待。明确 frame callback 用于节流，presentation 用于呈现反馈，不能合并为同一个时间点；空闲恢复不把休眠间隔作为一次巨大模拟步长。

然后区分 paint、layout 与 compositor 属性的动画失效，并复用本轮损伤/回退契约和工作计数。首轮验证启动、暂停、恢复、结束、取消、窗口关闭及 resize；先建立接口和预算证据，再实现 Dock/横线等视觉动画。节点增量布局、分块缓存及零拷贝专项继续后续安排。

具体的可证伪假设、测量口径和执行优先级见[动画与渲染优化假设清单](ANIMATION_RENDERING_HYPOTHESES.md)。
