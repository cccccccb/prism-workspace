# 统一应用 Host：当前实现与边界

日期：2026-09-26。属于 LAUNCH_RUNTIME_RESTORATION_PLAN.md 的第二步。

## 1. 代码归属

`prism-app-host` 是平台提供的客户端可执行文件，静态链接统一 DSL/Scene/Skia GLES/Wayland SDK。`prism_app_host_runtime` 接管前端生命周期，`prism_module_session` 将窄 C ABI 转换为绑定、动作、单次定时回调与 BackendReady。

音乐应用以目录包发布：manifest.json、master.prism、preview.prism、player.so、assets/。业务模块只依赖 prism_contracts；不链接 Wayland、Skia 或 Scene，不创建窗口或编写 Pump 循环。迁移的是已有曲目切换、播放状态与进度演示逻辑；当前没有音频解码和真实播放引擎，BackendReady 表示这份演示业务完成初始化。

每个实例仍有自己的 host 进程与窗口；业务模块加载到该进程。WM 只处理 Wayland surface、合成与窗口身份，未加入 DSL/应用控件代码。

## 2. 初始化阶段

| 阶段 | 执行动作 | fork 约束 |
| --- | --- | --- |
| AppHost 构造 | 配置与生命周期数据；不构造 ClientApplication | 没有主动创建线程/Wayland/EGL；CPU seed 整体仍需下一步审计 |
| PrepareFrontend | 构造字体命令渲染器和图片资源工作线程 | 必须在最终 worker 中执行，随后不再 fork 应用 |
| Bind | 固定一个应用包/实例；设置窗口身份和资源根；解析 Preview 或 Master；连接 Wayland | 单个 worker 一生只允许绑定一次 |
| 首次 configure/绘制 | 初始化该 surface 的 EGL/GLES/Ganesh，按 configure 尺寸布局 | GPU 和协议对象仅属于此实例 |
| 业务初始化 | dlopen 模块、create、应用绑定及 Ready | 有 Preview 时先等其实际呈现，才加载业务模块 |
| 运行/关闭 | host Pump、动作、定时回调；关闭先 destroy/卸载模块，再销毁前端 | 模块 destroy 必须停止自己的工作线程 |

PrepareFrontend 当前预热字体和资源线程，尚未预热 EGL/GPU。第三步已接入真正待命池，由最终 worker 自身调用此阶段；见 LAUNCHER_WORKER_POOL.md。完整硬件性能与最优池容量仍需后续测量。

## 3. Preview、Master 与就绪

Preview 与 Master 使用同一个 Wayland surface/EGL 上下文。Preview 实际呈现后，host 替换 Scene，再执行业务 create；同步绑定更新延迟到下一次 Pump 合并提交，避免逐个绑定触发半初始化界面。Scene 替换失败保留旧 Scene 并报告失败，不重建窗口。已经解码的共用图片会重新关联到新 Scene。

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

host 的 CLI 直接启动模式是后续冷启动基线。request/instance 参数仅作基线与未来集成关联，不能授予 Shell 身份。独立 CLI 基线输出诊断日志；worker 模式已接入 launcher 的私有控制 FD，前端事件经结构化消息传回服务。Accepted/WorkerAssigned/Exited 应由真实进程管理服务报告，host 不代替服务虚构这些状态。

Host API 的 launch_app 与模块 on_launch_event 已接入服务，服务不可用时返回 0，无 spawn 回退；音乐 demo 本身没有跨应用启动需求，链路由 tests/ fixture 验证。第四步已切换其余四应用、Dock/session/Shell 授权与 WM 激活；详细规则见 SESSION_LAUNCH_RUNTIME.md。

本段记录第二步检查点，当时 Pi `/usr` 尚未部署修改。第五步现已安装 0.1.0-3 并启动物理会话；测试、fixture 和 probe 留在 tests/，不安装到生产包。

## 6. 第二步验收记录

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

第五步最新发布检查点：0.1.0-4 已安装到物理 Pi，dpkg 完整性检查通过，默认 HUD 关闭；真实 V3D 首帧及创建/激活/取消/实例流复测通过。本轮 CTest 24/24 通过。现场输入复核已通过，规范和包路径见 PI_DEB_DEPLOYMENT.md。
