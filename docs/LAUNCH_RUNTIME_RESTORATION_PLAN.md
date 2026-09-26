# 统一客户端运行时与预热启动架构修订

日期：2026-09-26。状态：包/ABI/消息/里程碑基础、统一 host、待命池、五应用与可信会话入口已实现并验证，见 APP_LAUNCH_CONTRACT.md、APP_HOST_RUNTIME.md、LAUNCHER_WORKER_POOL.md 和 SESSION_LAUNCH_RUNTIME.md。第六步 BSP、共享 tokens 与图标玻璃主题已随 0.1.0-5 部署，当前 1024×600 现场外观及主要点击确认通过。本文修订前一轮将启动优化留待后续的安排：恢复平台启动链列入当前主线，并先于视觉扩展实施。

## 1. 应当恢复的目标

平台提供统一的 SDK/DSL 前端，应用提供声明式界面、业务逻辑与绑定；统一启动服务负责包识别、实例创建、激活、退出、Shell 授权关联与预热。独立客户端进程用于应用隔离，进程内部的前端由 Prism runtime 提供。

原设计第 3 节已要求客户端自己维护 DSL 与 surface；第 13 节仍保留 WM 镜像 AST/状态的旧拓扑。二者存在冲突。以用户明确要求的 WM 解耦为准：恢复 invoker/launcher 目标，同时保持 DSL、Scene、控件与 Skia 在客户端运行时。

此前直接 spawn 五个客户端是迁移时的过渡实现。它验证了 Wayland/Skia 呈现与 SDK 复用，但统一启动、包入口、预热、Preview/Master 和实例管理没有完成，不能作为最终平台架构。

## 2. 原代码审查

| 位置 | 实际行为与问题 | 修订方向 |
| --- | --- | --- |
| `zygote_server.cpp::Prewarm` | 仅 dlopen libc/wayland-client/libm；日志声称锁定内存，实际没有 mlock；未预热新 SDK | 建立有明确完成状态的 runtime 预热阶段，记录耗时与驻留内存 |
| `HandleClient` | 收到请求才 fork，随后 exec `/bin/sh -c` | 预先准备统一 runtime worker；应用绑定后不再 exec 丢弃预热状态 |
| launcher PID | 回报 shell PID，shell 可能再创建实际应用进程 | 直接关联 worker/实例/PID/实际 Wayland surface，保持身份与生命周期一致 |
| invoker 验证 | 无包时直接报验证成功；Sandbox/Preview 为日志占位；签名没有实际实现 | 定义真实包/ABI/资源校验与错误状态，未实现能力不得报告通过 |
| 调度结果 | 未严谨解析 launcher 回复，fork 成功就报 OK，成功路径未填写 spawned_pid | 分离 Accepted、WorkerAssigned、RuntimeReady、FirstPresented、Failed、Exited |
| IPC | 全局 `/tmp` socket、一次 read/write、256 字节缓冲、阻塞串行 accept | 会话私有 socket，带版本/长度/请求 ID 的结构化消息，完整收发与超时 |
| 子进程回收 | 只在新请求到来后执行 WNOHANG waitpid；失败分支未完整回复 | 将进程退出纳入事件循环，管理池补充、启动失败及会话结束 |
| Shell 身份 | 当前 WM 用自己 spawn 的 PID 授权；原 launcher `/bin/sh` 路径不兼容 | 由会话建立可信 launcher↔WM 通道，按真实实例/worker 授权 |
| SDK 初始化 | ClientApplication 构造持有图片资源系统，Open 解析 Scene 并创建 Wayland/EGL | 明确 fork 前 seed 与 fork 后 runtime/线程/GPU 初始化边界 |
| 安装包 | 五个应用分别静态链接公共 Skia/runtime，包入口未接入新 loader | 通用 host 提供公共前端，业务模块与 DSL/资产按包发布 |

Linux `fork` 的 COW 可以保留父进程内存映像；`execve` 会替换映像，因此旧 fork→exec 路径不能宣称继承 SDK 初始化结果。页缓存可能仍有收益，但必须独立测量。fork 耗时也不能代表可见首帧耗时。[fork(2)](https://man7.org/linux/man-pages/man2/fork.2.html)、[execve(2)](https://man7.org/linux/man-pages/man2/execve.2.html)。

## 3. 建议的生产拓扑

```text
session / systemd
├── prism-wm                         窗口、输入、合成、装饰
└── prism-launcher                   唯一的 runtime 实例与进程管理服务
    ├── CPU seed / worker 工厂       无 Wayland/EGL/业务状态的 fork 源
    ├── 待命 runtime worker          fork 后完成公共前端初始化
    ├── desktop 实例                 统一 runtime + DSL/资源 + 业务模块
    ├── topbar 实例                  统一 runtime + DSL/资源 + 业务模块
    ├── dock 实例                    统一 runtime + DSL/资源 + 业务模块
    └── 普通 Prism 应用实例           同一 SDK/DSL/Skia 前端
```

`prism-invoker` 保留为平台启动/查询命令入口，并复用 SDK Launch API。Dock 直接调用该 API 向常驻服务提交 app_id，不再自行 spawn，也不需要每次先启动一个 CLI 进程。包解析/实例决策在服务端完成，不依赖客户端请求者自报验证结果。

统一 `prism-app-host` / host runtime 是生产客户端入口。应用业务通过版本化模块接口（建议窄 C ABI）接入：创建、动作回调、状态更新、就绪、销毁。模块不负责 Wayland/EGL/绘制主循环；runtime 持有 UI、资源、布局、渲染与平台呈现。实例之间仍是独立进程，未绑定应用的 worker 不运行业务模块。

该 host/module 方式是对原 SDK 应用入口的进一步修订，需要先落定 ABI，再迁移五个自研应用。普通外部 Wayland 客户端可继续由 compositor 接收，其已有可执行文件不能冒充享有无 exec 预热收益的 Prism runtime 包。

## 4. fork 与预热边界

- 不从已有图片工作线程、Wayland 连接、EGL/GL 上下文或业务线程的进程 fork 出可继续执行 SDK 的应用。
- CPU seed 保持单线程；只允许审计过的代码/只读数据预加载，不使用现有完整 ClientApplication 作为 fork 模板。字体文件、Schema 表和不可变 Blueprint 的共享方式分别审查，不能把 live Scene 跨实例共享。
- seed 提前 fork 少量待命 worker。worker 在 fork 后初始化 SDK、字体/资源系统，并可以建立自己的 Wayland/EGL 基础状态；之后不再 fork 派生应用。
- 每个 worker 只绑定一个应用实例。应用退出后销毁 worker，再补充池；首版不回收已经运行过业务代码的进程作为另一应用的容器。
- 实际 wl_surface、xdg configure、窗口尺寸对应的 EGL surface 在正确的实例/角色绑定后建立。GPU 与协议对象从不跨应用共享。
- 第三方库的线程、锁、文件描述符与初始化副作用必须逐项审查。若单线程 seed 仍无法保证可继承状态，使用提前 spawn 同一个 host、由 worker 自身预热的池来满足待命目标；此选择改变工厂内部策略，不恢复五个应用各自随意启动的生产路径。

多线程 fork 后子进程只能安全调用 async-signal-safe 函数直至 exec 的限制，见 [fork(2)](https://man7.org/linux/man-pages/man2/fork.2.html)。预热 GPU 应发生在已派生的最终 worker 中；不将 GPU 上下文当作 COW 模板。

## 5. 统一前端与包契约

先将现有新 DSL 的 Blueprint、绑定、资源及业务入口纳入有版本的包契约；旧 `.prismb` 二进制节点表不直接视为新 Blueprint 格式。新包至少描述 app_id、runtime ABI、主 UI、可选 Preview、业务模块、资源与初始配置，明确资源根目录和依赖。

共享前端统一管理主题 token、控件状态、布局、输入与渲染。应用不再自行组织 SDK Pump 主循环、字体默认路径及图片路径回退。业务通过 SDK API 提供状态和处理动作。这将前端可控具体落实到包入口和运行时，而不只依赖每个应用自觉使用 SDK。

Preview 和 Master 使用同一实例/surface，由客户端 runtime 完成 Scene 更新及过渡；WM 仅观察窗口是否映射/呈现，不解析 Preview DSL。Preview 应先于耗时业务初始化提交，不能把显示 Preview 的条件设为业务加载完成。

Blueprint/字体/资源的缓存需有内容版本与失效规则。首版不为此恢复旧 WM AST 镜像，也不引入尚未设计的节点增量布局和 DisplayList 分块缓存。

## 6. 启动与身份契约

每次启动使用 launch_id / instance_id，将包、worker、PID、角色、surface 和结果关联。WM 与 launcher 的控制通道由会话创建并传递受控 FD 或等效凭证；普通启动请求不能自行指定受信任 Shell 身份。

Shell 的角色关联必须在首次角色 surface 映射前完成。launcher 对指定 worker 的授权与实际连接身份关联，而不是依赖可任意填写的 app_id、标题或全局文本 IPC。可使用限定实例/worker 的一次性启动凭证，必须定义消费、失败、取消和重启后的失效规则。

启动事件至少区分：

```text
Requested → Accepted → WorkerAssigned → RuntimeReady
          → SurfaceConfigured → FirstPresented → BackendReady
          → Failed / Exited
```

BackendReady 与 FirstPresented 是可独立发生的里程碑，上图不规定必须先显示 Master 才报告 BackendReady。请求重复、超时、取消、worker 崩溃、包错误、surface 未呈现、WM 断开均返回确定结果。

实例列表和激活依据实际窗口/进程状态。Dock 的运行标记订阅这些状态，不用 spawn 成功次数推断。session 结束时关闭 launcher 的实例与池，WM 与 launcher 重启策略有明确依赖关系。

## 7. 在 Pi 上应测的优化收益

统一 host 的代码/静态资源复用、少量待命 worker、包与字体准备、先提交 Preview、减少额外 CLI/sh 进程，是本轮应优先验证的方向。池容量先从小容量配置开始，用实测内存/功耗决定数量，避免每个应用维护一整套空闲 GPU worker。

分别记录请求→分配、runtime 初始化、包/模块加载、首次 configure、首帧提交、实际呈现和 BackendReady。对比冷启动、无待命 worker、已预热 worker、重复启动/激活，报告 p50/p95/p99、PSS、空闲 CPU、温度和启动失败率。必须在相同包、DSL、输出模式与驱动下比较；不使用 fork 的微秒计时代替完整启动延迟，也不沿用文档中的“0ms 首帧”完成宣称。

## 8. 修订后的实施顺序

1. **固定契约**：新应用包、业务模块 ABI、Launch API、实例状态、Shell 授权与会话生命周期。修正原文档的冲突拓扑与性能完成标记。
2. **实现 host**：将现有 ClientApplication 拆分为可控初始化阶段，接入一个真实 demo 业务模块、DSL 和资源包；完善 Preview/Ready 语义。
3. **实现 launcher 池**：seed/worker 边界、待命/分配/补充、退出回收、完整收发与失败结果。保留同一 host 的无池启动作为测量基线，生产入口统一走服务。
4. **一次切换五个应用**：session、Shell 身份和 Dock 都接入服务，移除 WM StartShellClients 直接 spawn、Dock posix_spawnp 和旧 sh 调度路径；不恢复 ImGui、旧 WM UI 镜像或占位启动步骤。
5. **打包与实机门槛**：安装包部署 host/业务模块/UI/资产与服务；验证隔离、授权、退出、Preview/首帧/就绪和启动收益。测试仍在 tests/。
6. **继续视觉与平铺**：进入 VISUAL_TILING_REFINEMENT_PLAN.md 的真实 BSP、透明布局、控件/材料、Shell 外观及实机验收。

每一步明确区分已实现与目标设计。在 host 与授权关联没有可运行、可验证的替代之前，当前 Pi 演示继续使用已有工作路径；不能只重开旧 launcher 服务并将其标为架构恢复。

## 9. 实施状态与下一门槛

- 第一步：已建立独立 prism_launch 目标，实现目录包严格校验、纯 C 业务模块 ABI 与加载器、启动消息编码/解码、实例里程碑状态机、一次性 Shell 凭证消费检查。规范以 APP_LAUNCH_CONTRACT.md 为准。
- 第二步：已接入统一 prism-app-host 与音乐 demo 模块/目录包；分离 host 构造、字体/资源线程准备、包/surface 绑定和延迟 EGL 初始化；host 接管事件循环、动作/绑定/tick、Preview 同 surface 切换与 BackendReady；FirstPresented 使用真实 presentation-time 回调。边界及验收见 APP_HOST_RUNTIME.md。
- 第三步：已实现提前 spawn 同一 host 的待命池、就绪/分配/收缩/补充/回收、完整非阻塞分帧 IPC、公共 SDK/模块 Launch API、取消、进程外 watchdog 与会话清理；旧 Zygote/sh 调度源码已移除。GPU 预热仍未实现；WM 激活随后在第四步接入，见 LAUNCHER_WORKER_POOL.md。
- 第四步：已接入可信控制 FD、pidfd 角色登记、真实窗口激活、实例订阅、四个剩余业务模块与统一 session supervisor；验证记录见 SESSION_LAUNCH_RUNTIME.md。
- 第五步：0.1.0-3 已完成打包、安装和 DRM/V3D 运行；创建/激活/取消/实例流与正常停止回收通过。0.1.0-4 修复 HUD 遮挡并补充 Dock 反馈后，用户确认显示与播放切换、Dock 激活反馈均正常；完整启动性能分位数仍待测量。见 PI_DEB_DEPLOYMENT.md。
- 第六步：BSP、共享主题 tokens、图标玻璃五应用界面和 compositor 背景材料已随 0.1.0-5 打包部署；26/26 测试、真实 host 双/三/四窗树与截图、V3D 背景更新检查，以及 1024×600 现场外观/主要点击通过。具体范围及后续项见 VISUAL_TILING_REFINEMENT_PLAN.md。

第一步本机验证：新增启动相关检查与契约 smoke 共 6/6 通过；prism/contracts 独立 C/C++ 构建与测试 2/2 通过。C fixture 真正动态加载并完成状态/Ready 回调。测试模块及验证源码均在 tests/，无安装规则。本轮不重打 deb，不切换当前 Pi 演示的启动链；以上为第一步的验证记录；第二步已接入 host，预热服务仍是下一门槛。

第二步本机验证：GLES 全量构建成功，CTest 19/19 通过；独立 headless host 检查覆盖 Preview 同 surface、实际首帧、无 Preview 的 Ready 顺序、Preview 后 ABI 失败和正常退出；五客户端 V3D 回归与临时安装隔离检查通过。规范及复现见 APP_HOST_RUNTIME.md。该记录为第二步验收；第三步的 launcher 池/传输/生命周期现已接入，下一门槛为第四步的五应用与 Shell/Dock/session/WM 激活切换。

第三步最终验证：GLES 构建成功，CTest 21/21 通过；完整 launcher_pool_probe 验证提前准备 PID 不被 exec 替换、池管理、公共/私有 API、取消/崩溃/外部超时、会话及异常父退出、残留 socket 重启。临时安装内容隔离通过，尚未部署 Pi 实物会话；完整规范与观察见 LAUNCHER_WORKER_POOL.md。下一步进入第四步，不将未接入的 WM 激活或 Shell 身份报告为完成。

第四步会话验证：GLES 构建、CTest 24/24、独立 V3D/headless 统一 session 和 launcher 池回归均通过。覆盖三个 Shell 角色、真实窗口激活、Dock 状态源和正常/WM/launcher/Shell 故障清理，详细记录见 SESSION_LAUNCH_RUNTIME.md。生产 Release 构建与临时安装清单通过；从安装前缀运行统一会话通过，无旧入口或测试内容。重复/死亡/过期登记与自报失败后阻塞回收探针通过。下一步部署新 deb 到物理 Pi。

第五步部署补充：安装包不含旧客户端入口或测试；修复 PAMName=login 启动前已存在的 helper 与 supervisor 相互等待导致的停止超时。带预存 helper 的正常/WM/launcher/Shell 故障回收检查均通过，真实服务停止约 0.347 秒且 Result=success。实机输入反馈与详细记录见 PI_DEB_DEPLOYMENT.md。

### 实机交互问题修订（0.1.0-4）

现场反馈定位到默认开启的 WM HUD：其 x=18/y=44/440×160 覆盖 Music 控件，输入仍到达下面的客户端，导致状态实际变化但看不见。生产默认关闭 HUD，场景节点在首次提交前就禁用，不闪现。诊断开关仅显式启用。Dock 增加 Opening/active/ready/failed 文字反馈：active 只由 WM Activated 驱动，ready 必须同时收到真实 FirstPresented 与 BackendReady，Accepted 不视为完成。该反馈是 Dock 业务绑定，WM 不接收应用 UI。0.1.0-4 的 Settings Theme 当时仅切换 demo 文字，不宣称全局换色。该版已安装，用户确认遮挡消失、播放切换和 Dock 反馈正常。

第五步发布检查点（历史 0.1.0-4）：已安装到物理 Pi，dpkg 完整性检查通过，默认 HUD 关闭；真实 V3D 首帧及创建/激活/取消/实例流复测通过。该阶段 CTest 24/24 通过。现场输入复核已通过，规范和包路径见 PI_DEB_DEPLOYMENT.md。

### 第六步发布检查点（0.1.0-5）

统一 host、预热池、会话授权和业务隔离路径保持单一生产入口。本轮将真实 XDG view 接入同一 BSP 树，增加方向焦点/交换、工作区与显式 fullscreen；共享主题 JSON 生成纯契约，客户端 DSL 与 WM 使用同一几何/颜色参数。SDK 增加透明清屏、通用布局、向量图标、基础控件、描边及阴影；compositor 通过私有 surface-effect 契约实现下层 GLES 背景材料。五应用采用静态图标玻璃布局，没有新增花哨动画。

Settings 外观开关实际改变本窗口 tint、文字颜色和图标，不影响其他实例或 WM；全局主题服务尚未实现。Music 的前后曲、播放状态和进度仍是 demo 业务，无真实音频。

0.1.0-5 已安装物理 Pi；最终 CTest 26/26，Release 安装清单和前缀/预存 PAM helper 正常检查、四种停止/故障回收均通过。真实 host 双/三/四窗截图与树尺寸、V3D 下层模糊与变化传播检查通过。生产包未安装测试、旧客户端或 ImGui。

现场输出 HDMI 1024×600，五应用使用 V3D，背景能力为 1，本轮 journal 无 ERROR，协议门槛通过。用户确认“外观正常，以上点击都正常”，包括播放图标、Preferences 本窗口配色和 Dock 激活；新截图为 `dist/validation/prism-v5-physical.png`。这完成当前单输出静态视觉与主要点击的发布门槛；全部键盘快捷键、多屏/DPI、Slider、实时比例调整、音频、全局主题和性能分位数继续按后续范围处理。详见 VISUAL_TILING_REFINEMENT_PLAN.md 与 PI_DEB_DEPLOYMENT.md。
