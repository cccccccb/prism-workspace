# 统一客户端运行时与预热启动架构修订

日期：2026-09-26。状态：原实现审查、目标架构与实施顺序；统一 host、预热 worker 池和新启动协议尚未实现。本文修订前一轮将启动优化留待后续的安排：恢复平台启动链列入当前主线，并先于视觉扩展实施。

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
