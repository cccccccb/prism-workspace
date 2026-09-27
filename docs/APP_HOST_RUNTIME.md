# 统一应用 Host：当前实现与边界

日期：2026-09-26。本文保留 LAUNCH_RUNTIME_RESTORATION_PLAN.md 第二步及后续入口迁移的历史记录，并补充当前主题接口。运行时主题规范见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)；本轮验证结果另行记录。

## 1. 代码归属

`prism-app-host` 是平台提供的客户端可执行文件，静态链接统一 DSL/Scene/Skia GLES/Wayland SDK。`prism_app_host_runtime` 接管前端生命周期，并链接主题编译器用于直接启动模式的主题包加载；生产 worker 接收 launcher 编译完成的纯 ThemeSnapshot。`prism_module_session` 将窄 C ABI 转换为绑定、动作、单次定时回调、BackendReady 和主题请求/结果。

五个应用均以目录包发布。音乐包包含 manifest.json、master.prism、preview.prism、
layout.prism、ui/、player.so 和 assets/catalog.json；其余应用使用同一 host 与模块 ABI。
业务模块依赖纯 prism_contracts；Music 另使用 nlohmann/json 处理 typed 曲库 DTO，
不链接 Wayland、Skia 或 Scene，不创建窗口或编写 Pump 循环。音乐仍是曲目、播放状态
与进度演示，没有音频解码和真实播放引擎；BackendReady 表示这份演示业务完成初始化。

每个实例仍有自己的 host 进程与窗口；业务模块加载到该进程。WM 只处理 Wayland surface、合成与窗口身份，未加入 DSL/应用控件代码。

## 2. 初始化阶段

| 阶段 | 执行动作 | fork 约束 |
| --- | --- | --- |
| AppHost 构造 | 配置与生命周期数据；不构造 ClientApplication | 不主动创建线程/Wayland/EGL；完整 seed/pool 边界见 LAUNCHER_WORKER_POOL.md |
| PrepareFrontend | 构造字体命令渲染器和懒启动资源调度句柄 | 必须在最终 worker 中执行，随后不再 fork 应用 |
| 初始主题安装 | 校验并保存 ThemeSnapshot，向 launcher ACK 当前 generation | 生产 worker 在主题确认前不能 Bind；不创建 Wayland/GPU 对象 |
| Bind | 固定包/实例与资源根；解析轻量 Preview，或排队无 Preview 的 Master | 单个 worker 一生只允许绑定一次 |
| 首次 configure/绘制 | 初始化该 surface 的 EGL/GLES/Ganesh，按 configure 尺寸布局 | GPU 和协议对象仅属于此实例 |
| 业务初始化 | 快速 dlopen/create；通过 work 准备，owner 完成回调更新绑定及 Ready | 有 Preview 时先等其实际呈现，才加载业务模块 |
| 运行/关闭 | host Pump、动作、定时回调；先取消/join 业务工作，再 destroy/卸载模块与销毁前端 | 模块另起的私有线程也必须在 destroy 中停止 |

PrepareFrontend 当前预热字体、资源调度句柄与主题，工作线程按需启动，尚未预热 EGL/GPU。第三步已接入真正待命池，由最终 worker 自身调用此阶段；见 LAUNCHER_WORKER_POOL.md。完整硬件性能与最优池容量仍需后续测量。

## 3. Preview、Master 与就绪

Preview 与 Master 使用同一个 Wayland surface/EGL 上下文。Preview 实际呈现后，host 替换 Scene，再执行业务 create；同步绑定更新延迟到下一次 Pump 合并提交，避免逐个绑定触发半初始化界面。Scene 替换失败保留旧 Scene 并报告失败，不重建窗口。已经解码的共用图片会重新关联到新 Scene。Preview 和 Master 都使用 host 当前快照；主题切换更新保留的 Scene，不走这条 UI 替换路径。

前端在包的 assets 根下解析图片 URI；绝对路径、..、反斜杠、符号链接逃出 assets 和非普通文件均拒绝。host 从已校验包路径读取 UI，有 1 MiB 大小上限；字体默认配置归平台所有，包不使用工作目录或旧资源路径回退。包文件仍要求在实例运行期间保持不变，这不是文件系统沙箱或签名验证。

后端通知仅表示业务就绪；业务 create/action/tick 在 host 事件线程执行，必须快速返回。
第五步接入 Host submit_work 与共享调度器，完成 FD 唤醒后在事件线程调用
on_work_completed；无需用定时 tick 取结果。工作线程直接调用 Host API 会被拒绝。
Preview 和新工作接口不使任意同步阻塞入口可抢占，launcher 的进程外 watchdog
继续处理不返回的初始化与崩溃，具体边界见第 14 节。

没有 Preview 的应用可先完成业务绑定，再提交主界面；BackendReady 可以早于 FirstPresented。有 Preview 的音乐 demo 则先实际呈现 Preview，再初始化业务。首次呈现、配置与 Ready 都只报告一次；运行时 resize 不重报启动里程碑。Startup 的呈现与 Ready 等待上限为 10 秒，受事件线程持续运行这一前提约束。

## 4. 真正的呈现反馈

WM 注册 `wlr_presentation_create`，wlroots scene 的 surface/output 路径负责呈现反馈；客户端绑定 wp_presentation v1，在 buffer commit/eglSwap 前请求 feedback。只有 presented 回调递增 PresentationCount，并触发 FirstPresented。discarded 不算呈现，启动帧被丢弃时请求重绘。

FrameDoneCount 是帧调度回调；旧 PresentedCount 为兼容已有诊断仍保留，实际含义是 EGL swap 提交次数；这两者都不用于 FirstPresented。Host 要求 compositor 提供 presentation-time，否则明确失败。headless 的 presented 证明协议与合成链可达，物理可见时间仍需 DRM 实机验证；本步骤不宣称零拷贝、硬件呈现时钟或性能收益。

协议依据来自系统 wayland-protocols 的 stable/presentation-time/presentation-time.xml：[上游协议](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/presentation-time/presentation-time.xml)。

## 5. 当前入口及下一步

构建后可运行：

```sh
build-gles/bin/prism-app-host --package "$PWD/build-gles/share/prism/apps/demo_player"
```

安装规则部署 bin/prism-app-host 和五个 share/prism/apps/ 目录包。第四步已删除 demo_player exec 适配器及其余四个独立可执行入口，正式会话统一走 launcher 的待命 host；详情见 SESSION_LAUNCH_RUNTIME.md。

host 的 CLI 直接启动模式用于诊断或冷启动基线；没有私有初始快照时从程序前缀下的 `share/prism/themes/glass/theme.prism` 加载默认主题。request/instance 参数仅用于实例关联，不能授予 Shell 身份。独立 CLI 输出诊断日志；worker 模式通过 launcher 的私有控制 FD 接收主题与启动命令，前端事件经结构化消息传回服务。Accepted/WorkerAssigned/Exited 由真实进程管理服务报告，host 不代替服务虚构这些状态。

Host API 的 launch_app 与模块 on_launch_event 已接入服务，服务不可用时返回 0，无 spawn 回退；音乐 demo 本身没有跨应用启动需求，链路由 tests/ fixture 验证。第四步已切换其余四应用、Dock/session/Shell 授权与 WM 激活；详细规则见 SESSION_LAUNCH_RUNTIME.md。

本段记录第二步检查点，当时 Pi `/usr` 尚未部署修改。第五步现已安装 0.1.0-3 并启动物理会话；测试、fixture 和 probe 留在 tests/，不安装到生产包。

## 6. 第二步验收记录（历史）

- Pi AArch64 GLES 全量构建成功；包含音乐模块业务行为/定时/失败初始化测试的 CTest 19/19 通过。
- 独立 headless 会话：音乐 host 请求 71/实例 81 依次收到 RuntimeReady、SurfaceConfigured、FirstPresented、BackendReady；Preview/Master 只有一次窗口映射；定时回调运行后 SIGTERM 正常退出。
- 无 Preview 包在首次呈现前报告 BackendReady；错误 ABI 包在 Preview 实际呈现后报告 UnsupportedAbi/Failed，退出为 1，未报告 BackendReady。
- 原五客户端 headless 集成通过；GL 探针为 V3D 4.2.14.0，configure=3/frame=2/swap 提交=4/images=1/1。计数可能随时序变化，不作为固定性能指标。伪造 prism_topbar app_id 仍映射为普通角色。
- 临时安装目录包含 host、适配器、音乐模块/manifest/UI/assets；无测试 fixture/probe 安装项。没有重打/安装 deb，也没有切换当前物理会话。

复现：

```sh
cmake --build build-gles -j3
ctest --test-dir build-gles --output-on-failure
python3 tests/probes/app_host_probe.py build-gles
python3 tests/probes/client_suite_probe.py build-gles
```

## 7. 第三步更新

待命池已接入：worker 在 PrepareFrontend 完成后通过私有控制 FD 报告就绪，Assign 将真实请求/实例绑定到已准备 host；Bind 不再执行新程序。模块 launch_app/on_launch_event 已接入服务，独立 host 使用同一 SDK 连接；不可用时返回 0，无 spawn 回退。进程外 watchdog/退出回收补充了第二步的 host 自身超时。第二步章节保留当时的验收记录；池机制见 [LAUNCHER_WORKER_POOL.md](LAUNCHER_WORKER_POOL.md)，第四步入口/WM 激活/角色切换现已完成，见 SESSION_LAUNCH_RUNTIME.md；GPU 待命预热尚未实现。

## 2026-09-26 第四步更新

五个应用已统一迁到 host 模块/目录包；可信 WM 控制通道、Shell 一次性登记与 pidfd、真实 Activated 事件、Dock 映射实例订阅和 session supervisor 已接入。此前未切换描述为历史检查点。第五步 0.1.0-3 已部署到物理 Pi，显示确认正常，交互遮挡原因已定位（默认 HUD），0.1.0-4 已修复并安装，现场复核已通过；细则见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。

历史 0.1.0-4 发布检查点：已安装到物理 Pi，dpkg 完整性检查通过，默认 HUD 关闭；真实 V3D 首帧及创建/激活/取消/实例流复测通过，当时 CTest 24/24 通过。其后的 0.1.0-5 静态视觉/BSP 发布记录见 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)，这些结果不代替本轮运行时主题验证。

## 8. ThemeSnapshot 与已有实例更新

`HostConfig::initial_theme` 可携带初始快照，`select_theme` 回调负责异步主题请求。`AppHost::ApplyTheme(snapshot, diagnostic)` 将快照应用到已准备的 ClientApplication，并保存到 host 配置；Open 和 Preview/Master 的 ReplaceUi 使用这份最新快照。`ThemeGeneration()` 返回当前已接受版本。无变化的合法快照同样返回 true；拒绝时返回 false 和诊断，保留旧快照与正常业务实例。

生产 worker 在 PrepareFrontend 后发送 Ready，收到快照并成功安装后发送 `ThemeApplied{generation, success, detail}`。launcher 确认当前版本才可分配/绑定应用。运行时更新使用同一通道；SDK 在候选 Scene 中验证 token、材料、布局、效果和输入轮廓，成功后只替换现有节点的样式数据，保留 NodeId、业务 binding、模块实例、资源、surface 和 EGL。无法表示的 backdrop clip 等问题产生失败 ACK，launcher 按新 generation 恢复原主题。全局事务与限制见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。

`DeliverThemeEvent` 将平台结果交给 ModuleSession。业务 create 完成后，host 发送一份初始 Current 事件；此后的 Current/Applied/Rejected 来自会话服务。ACK 表示参数安装，不能据此宣称多个进程在同一显示帧原子换肤。测试只放 tests，本节不宣称本轮构建、实机或性能验证已完成。

## 9. 模块 ABI 可选主题尾部

ABI v1 的原有字段和布局前缀保留：HostApi 尾部增加 `select_theme(context, PrismStringViewV1 id) -> uint64_t`，Module 尾部增加 `on_theme_event(instance, const PrismThemeEventV1*)`。加载器只复制 struct_size 覆盖的完整字段，旧模块缺失回调时置空；模块调用新 Host API 前同样检查字段是否存在。`prism::app::SelectTheme` 已提供这一检查。

主题接口的参数包含已安装的包 ID，空 ID 查询当前主题；配色接口独立请求 light/dark；返回非零表示已排队，零表示接口不可用或请求被拒绝，不能提前报告已切换。ModuleSession 构造函数保留可选 `ThemeSink`，其后追加可选 `ColorSchemeSink`，并通过 `Deliver(const ThemeEvent&)` 投影结果。

`PrismThemeEventV1` 包含 struct_size、request_id、generation、status 和借用的 id/name/detail 字符串。status 为 Current=0、Applied=1、Rejected=2；request_id=0 可表示当前主题广播。模块按真实结果更新显示，不自行写另一份窗口配色。字符串只在回调期间有效。旧 host 无此尾部时，应用应明确显示接口不可用；不可回退为伪装成全局主题的本窗口颜色切换。

### 配色与 ABI 尾字段

`HostConfig::select_color_scheme` 与 module 的可选 `select_color_scheme` 接口沿用主题请求通道。初始快照、Preview/Master 切换、预热 host、应用中快照更新及 `on_theme_event` 均携带同一 `color_scheme`。V1 的原前缀不变；新 module 处理旧 `PrismThemeEventV1` 时按旧前缀长度校验，完整尾字段存在才读取配色，否则 dark。独立 host 的事件回读同样用 ID 和实际配色加载资源，不能重置为默认 dark。


## 10. 事件等待与 one-shot tick

Host 的 `Pump(timeout_ms, wake_fds)` 合并前端 Wayland/资源、内部 launcher、调用者控制与退出通知；负 timeout 允许无限等待。业务 ABI v1 已有 `schedule_tick`，本轮不扩展模块结构。等待上限取业务下一次 tick、启动 10 秒 deadline 与调用者上限的最早值；没有 tick 时不设置固定轮询。业务回调仍在 Host 主线程执行。

生产 worker、launcher 与 session supervisor 使用可轮询信号通知，SIGCHLD 驱动回收，watchdog/主题/片段/升级清理按真实截止时间处理。控制事件和完整用户态缓存消息均能唤醒；仅片段不选零 timeout。Music 暂停停止重排定时器，播放恢复 500ms 更新。Master 纯准备在 Preview 像素提交后派发，安装和业务加载等待其实际呈现；同 surface 替换、Ready 和取消协议沿用既有链路。渲染规范见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md)，异步加载进度见本文第 11 节。

worker 自报合法 Failed 后，launcher 保留失败事件并给予 250ms 自行退出窗口，仍未退出则 TERM→1s KILL；正常退出和信号退出都由 waitpid 如实报告。取消、watchdog、会话停止立即 TERM，未增加等待宽限。该规则覆盖快速错误退出与失败后阻塞清理，详见 [待命池生命周期](LAUNCHER_WORKER_POOL.md#5-生命周期超时与取消)。

## 11. 2026-09-27 加载现状核对与下一阶段

有 Preview 的包在成功像素提交后派发单单元 Master 的读取/纯准备，并继续处理事件；
候选完成且该 Preview 实际呈现后，才在所有者线程替换 Scene、dlopen/create 业务。
无 Preview 的包在 Bind 中排队准备，Pump 在窗口尚未打开时合并完成 FD 与调用者控制 FD。
HostUiState 本地记录 MasterPrepared/Installed/Submitted/Presented，呈现绑定具体加载代数
与像素提交身份；不扩展 worker/public wire 或 Dock 状态。

统一 Host 与业务模块通过窄接口解耦，业务 `.so` 在本实例的 Host 进程内，并非另一个
后台进程。下一阶段的准备/安装边界、DSL 组件依赖图、critical/deferred、资源预算、
呈现代数、业务异步约束及六步顺序见 [MASTER_PARALLEL_LOADING.md](MASTER_PARALLEL_LOADING.md)。
第一步已提交 `99101cf`。第二步接入 LoadSession 的有界单线程准备与 eventfd 唤醒，
并通过验证。第三步组件图、统一资源调度与全会话预算已提交 `b4fdf5b`；第四步
已提交 `56704a5`，在事件线程分轮构造和安装候选，执行真实图片 GPU 上传预算。字体、布局、GPU/
Wayland 和业务回调仍归事件线程，单次库/驱动调用及慢业务初始化不能被预算抢占。
第五步已提交 `527df14`，规范见第 14 节。第六步真实 Music 已拆分 critical/deferred
组件并迁入曲库 work，正在统一构建与实机测量。进度与验证以加载规范第 13 节为准。


## 12. 组件加载调度（2026-09-27）

第二步提交 `f82ef5d` 后，Host 统一使用 MasterLoadSession 编排旧单文件和 Interface v2。
纯读取/校验/组件组合和图片检查/解码共享同一 TaskScheduler；PrepareFrontend 准备
字体、调度句柄和主题，工作线程仅在有任务时创建，不再提前启动独立图片线程。
launcher 的共享 SessionTaskBudget 通过 FD4 传入并经认证，私有控制 FD3 和启动消息
保持原契约。第三步已提交 `b4fdf5b`。Master 的必需图片就绪后安装，真实呈现后才
准备 deferred；当前第四步已接入下述稳定区域安装事务。第三步边界、内存计账和
历史验证见
[加载规范第 10 节](MASTER_PARALLEL_LOADING.md#10-第三步组件图共享工作池与会话预算)。

## 13. 第四步：安装事务与所有者线程预算

`StartPreparedInstall` / `StartRegionInstall` 只建立待安装候选；`AdvanceUiInstall`
分轮申请图片、等待解码、注册资源、上传图片并推进 `SceneConstruction`。候选使用
当前主题、视口和绑定值完成整体布局、绘制指令、效果与输入预检，成功后才发布；取消、旧代数、
资源或样式失败不发布半成品，保留原 Scene。主题变化或区域事务版本变化会丢弃旧
候选并重新准备。预检、单次 shaping/布局与驱动操作仍是不可抢占的同步工作。

Master 实际呈现后才派发 deferred 准备。Host 按依赖顺序选择区域小批次， SDK 在
稳定 Region 边界提交新子树，保留未替换节点、绑定、窗口与 EGL；移除节点的 focus/
hover 被清理。未挂载目标的已声明 typed binding 仍保留最新业务值，挂载前使用当前
值。局部失败保留已有界面、报告对应组件诊断并拒绝依赖该失败区域的后续区域；不能
因候选失败将整份 Master 标记为新呈现。

Host 用 `BeginUiWorkTurn` / `EndUiWorkTurn` 包围整次 Pump，安装前后半段和前端
上传共用一个延迟启动的预算，不能通过再次调用 Advance/Pump 重置额度。默认每轮
128 个构造节点、分阶段安装的每个图片处理阶段 2 个资源、实际上传最多 2 张/4 MiB
和 2 ms 协作式 CPU 截止时间，均可配置。单张大图片允许一次原子超额上传并单独
计数；时间在工作单位
之间检查，不承诺严格 2 ms 返回，也不把它描述为节点增量布局或分块缓存。
同步便捷入口及既有 Scene 的异步图片完成 CPU 注册仍可批量执行；对应 GPU 上传
使用有界队列。完整额度范围与不可抢占工作见加载规范第 11 节。

GLES 的 `UploadImage` 实际创建并保留 texture-backed image，在所属 EGL 上下文
实例化纹理并 flush/submit；解码完成、CPU 注册和 GPU 上传是不同阶段。生产 SDK
预算内预上传后才 Render，绘制复用该纹理。无 Preview 的 Master 先安装并打开窗口，
首次 configure/EGL 初始化后继续有界上传；`Deferred` 保留像素请求且不 commit、
不创建 frame/presentation feedback，每轮返回事件所有者。Installed/Submitted/
Presented 各自独立，只有对应代数的真实 presentation feedback 证明已呈现。

本轮 Pi GLES 构建、完整 CTest 49/49、Host/V3D 加载 18/18、SDK 呈现及 Host/待命池
回归通过。第五步业务异步准备见第 14 节，第六步真实 demo/实机性能对照尚未实现，不将安装
预算当作已证明的启动提速。完整规范与验证记录见
[MASTER_PARALLEL_LOADING.md](MASTER_PARALLEL_LOADING.md)。

## 14. 第五步：Host 管理的业务工作（已实现并验证）

第四步已提交 `56704a5`。本轮新增 v1 的 optional C ABI 尾字段：Host 的
submit_work/cancel_work，以及模块的 on_work_completed。模块在快速 create 中提交
复制的值输入，具名 work 在共享 TaskScheduler 中执行；runtime 保存有界不可变结果，
完成 FD 加入 AppHost 原来的等待集合，再在 Host 线程交给业务实例消费。业务任务
不持有 Scene、instance 或 Host API；调用所有者 API 的工作线程会在入口处被拒绝。

默认任务/输入/结果和完成派发额度，以及取消 FD、busy 重试、租约与 ID 复用规则见
[加载规范第 12 节](MASTER_PARALLEL_LOADING.md#12-第五步业务工作完成通知与模块生命周期)。
新工作接口无需定时 tick。每个 Host Pump 至多派发一个有界完成批次，caller 控制
FD 在前后两段均先于完成回调；消费结果而无像素更新也返回外层，及时发布 Ready。
本地 business_work_pending/completion_ready 只提供观察，不增加外部启动里程碑。

退出先禁用新动作/发布，再取消并 join 工作，随后 destroy/dlclose；工作函数的代码
生命周期不能短于任务。loader 仅复制完整尾字段，旧模块缺少新 callback 时仍可
使用原接口；新模块须检查 Host capability。dlopen/入口查询及 create 默认各自 20ms
返回后检查，不能抢占不返回的静态构造或 create；违反契约的模块仍由外部 watchdog
回收，不宣称任意旧模块被自动改成异步。

本轮 Pi GLES 构建、完整 CTest **50/50**、原生异步业务 **3/3**、Master 加载
**18/18**、Prepared UI/SDK 提交 **2/2**、Host/待命池回归及依赖/风格检查通过。
完整证据与验证范围见加载规范第 12 节。第六步迁移与对照见下节。

## 15. 真实 Music 与加载观测

Music 的 Interface v2 声明三个 critical 组件和一个 deferred 曲库，Host 保留稳定
Slot 并在首屏实际呈现后安装曲库。create 复制 init.assets_root，通过 Host work
读取 catalog.json；owner 完成回调更新声明绑定，成功才报告 Ready。资源根来自包
校验，与视觉 DSL/绘制器无关；原生业务模块仍不是沙箱。新 Music 随匹配平台部署，
缺完整 init 或 work capability 时不采用同步后备入口。

HostConfig.task_workers 为 1/2，默认 2；同一管线可作串行/并行准备对照。
GetStartupStats 返回 monotonic 所有者观察，不增加启动 wire 里程碑；deferred_complete
指安装完成。pump_processing 仅扣除直接计量的 poll 等待，保留内部 roundtrip/IO/
驱动阻塞；实际 CPU、内存及主题反馈由隔离 probe 单独测量。规范、矩阵、验证及
发布状态见 [加载规范第 13 节](MASTER_PARALLEL_LOADING.md#13-第六步真实-music多区域与同管线实机对照)。

第六步最终 CTest 51/51、直接 Host 40 组、生产 launcher 24 组及原生回归通过。
0.1.0-13 已审计/安装并通过 DRM 启动和实例门槛，曲库/收藏及原有操作现场确认通过。小 demo
未呈现稳定的并行提速；下一阶段优先细分 EGL 初始化与首次 Render 的同步成本。
