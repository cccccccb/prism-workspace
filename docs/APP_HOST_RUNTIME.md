# 统一应用 Host：当前实现与边界

日期：2026-09-26。本文保留 LAUNCH_RUNTIME_RESTORATION_PLAN.md 第二步及后续入口迁移的历史记录，并补充当前主题接口。运行时主题规范见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)；本轮验证结果另行记录。

## 1. 代码归属

`prism-app-host` 是平台提供的客户端可执行文件，静态链接统一 DSL/Scene/Skia GLES/Wayland SDK。`prism_app_host_runtime` 接管前端生命周期，并链接主题编译器用于直接启动模式的主题包加载；生产 worker 接收 launcher 编译完成的纯 ThemeSnapshot。`prism_module_session` 将窄 C ABI 转换为绑定、动作、单次定时回调、BackendReady 和主题请求/结果。

五个应用均以目录包发布。音乐包包含 manifest.json、master.prism、preview.prism、player.so、assets/；其余应用使用同一 host 与模块 ABI。业务模块只依赖 prism_contracts，不链接 Wayland、Skia 或 Scene，不创建窗口或编写 Pump 循环。音乐仍是曲目、播放状态与进度演示，没有音频解码和真实播放引擎；BackendReady 表示这份演示业务完成初始化。

每个实例仍有自己的 host 进程与窗口；业务模块加载到该进程。WM 只处理 Wayland surface、合成与窗口身份，未加入 DSL/应用控件代码。

## 2. 初始化阶段

| 阶段 | 执行动作 | fork 约束 |
| --- | --- | --- |
| AppHost 构造 | 配置与生命周期数据；不构造 ClientApplication | 不主动创建线程/Wayland/EGL；完整 seed/pool 边界见 LAUNCHER_WORKER_POOL.md |
| PrepareFrontend | 构造字体命令渲染器和图片资源工作线程 | 必须在最终 worker 中执行，随后不再 fork 应用 |
| 初始主题安装 | 校验并保存 ThemeSnapshot，向 launcher ACK 当前 generation | 生产 worker 在主题确认前不能 Bind；不创建 Wayland/GPU 对象 |
| Bind | 固定一个应用包/实例；设置窗口身份和资源根；解析 Preview 或 Master；连接 Wayland | 单个 worker 一生只允许绑定一次 |
| 首次 configure/绘制 | 初始化该 surface 的 EGL/GLES/Ganesh，按 configure 尺寸布局 | GPU 和协议对象仅属于此实例 |
| 业务初始化 | dlopen 模块、create、应用绑定及 Ready | 有 Preview 时先等其实际呈现，才加载业务模块 |
| 运行/关闭 | host Pump、动作、定时回调；关闭先 destroy/卸载模块，再销毁前端 | 模块 destroy 必须停止自己的工作线程 |

PrepareFrontend 当前预热字体和资源线程，尚未预热 EGL/GPU。第三步已接入真正待命池，由最终 worker 自身调用此阶段；见 LAUNCHER_WORKER_POOL.md。完整硬件性能与最优池容量仍需后续测量。

## 3. Preview、Master 与就绪

Preview 与 Master 使用同一个 Wayland surface/EGL 上下文。Preview 实际呈现后，host 替换 Scene，再执行业务 create；同步绑定更新延迟到下一次 Pump 合并提交，避免逐个绑定触发半初始化界面。Scene 替换失败保留旧 Scene 并报告失败，不重建窗口。已经解码的共用图片会重新关联到新 Scene。Preview 和 Master 都使用 host 当前快照；主题切换更新保留的 Scene，不走这条 UI 替换路径。

前端在包的 assets 根下解析图片 URI；绝对路径、..、反斜杠、符号链接逃出 assets 和非普通文件均拒绝。host 从已校验包路径读取 UI，有 1 MiB 大小上限；字体默认配置归平台所有，包不使用工作目录或旧资源路径回退。包文件仍要求在实例运行期间保持不变，这不是文件系统沙箱或签名验证。

后端通知仅表示业务就绪；业务 create/action/tick 在 host 事件线程执行，必须快速返回。真正耗时任务应由模块自行异步执行，并通过 host tick 在事件线程取结果；当前 ABI 不允许工作线程直接调用 Host API。Preview 不让同步阻塞业务变得可取消，第三步已接入进程外 watchdog，由 launcher 处理阻塞初始化与崩溃。

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
