# Launcher 待命 Worker 池与服务协议

日期：2026-09-26。恢复计划第三步：已接入常驻服务、同一 host 的待命/分配/补充/回收、公共 SDK Launch API、业务模块事件投递与独立启动 watchdog。第四步已接入 Shell/WM 授权、窗口激活与五应用/会话入口（见 SESSION_LAUNCH_RUNTIME.md）；当前物理演示已部署这一链路（0.1.0-3）；现场画面正常，交互遮挡已在 0.1.0-4 修复并安装，现场复核已通过。

## 1. 生产代码归属

- `prism_launcher_runtime` 依赖 `prism_launch` 和主题编译器，不依赖 WM/Skia/应用 Scene；服务校验包元数据、编译主题快照并管理统一 host，不解析应用 UI DSL。待命 worker 不预解析 Master；下一阶段并行加载见 [MASTER_PARALLEL_LOADING.md](MASTER_PARALLEL_LOADING.md)。
- `prism_launch_client` 为 CLI 与客户端提供同一启动/取消/事件接口；公共请求只有 app_id/模式，不接受命令、模块路径或 Shell 角色。
- `prism-app-host --worker-fd ...` 是同一个平台前端的 worker 模式，准备字体与图片资源线程后报告 WorkerReady；收到绑定后继续在原进程中运行包。
- 业务模块通过 `launch_app` 发起异步请求，通过 `on_launch_event` 接收包含 app_id/实例/PID/里程碑/错误的投影。返回非零仅表示已排队，绝不代表窗口已显示。绑定、Ready、动作与 tick 仍由 host 管理。
- 旧 Zygote、文本 IPC、sh 命令分发和占位 LaunchPipeline 的源码/目标已删除。新的 launcher/invoker 不链接 prism_core 或旧 AOT 工具。

安装后的默认包目录为 `/usr/share/prism/apps`；构建目录默认是 `build-gles/share/prism/apps`。注册目录中 `<app_id>/manifest.json` 必须声明同一 app_id，包目录不能通过符号链接逃出注册根。目录下有可加载文件并不等于签名校验或沙箱实现；包仍要求运行期间不可变。

## 2. Worker 工厂与初始化边界

首版选择提前 posix_spawn 同一个 host、由最终 worker 自行准备前端。它不采用 COW seed 继承方案，不宣称共享 live SDK/GPU 状态。posix_spawn 的 exec 发生在 worker 准备之前；已经 WorkerReady 的进程收到应用绑定后不再 exec，保留已准备的字体/资源线程/运行时映像。此选择符合修订规范允许的提前 spawn 方案。[posix_spawn(3)](https://man7.org/linux/man-pages/man3/posix_spawn.3.html)。

worker 不在待命阶段解析应用 UI、加载业务模块、建立应用 Wayland surface 或创建 EGL/GPU 上下文。实际 configure/尺寸及 GPU 资源仍在绑定应用后建立。每个 worker 一生只绑定一次；业务运行或失败后进程销毁，不作为另一应用的容器重复使用。

默认目标为 1 个未绑定 worker，最多 8 个总 worker；配置上限为 32。Preparing 和 Idle 都计入未绑定数量；准备中的进程不可分配。每次维护最多创建 1 个 worker，创建/准备失败设退避；分配/退出后补充，取消后多余的空闲/准备进程会收缩。总进程数到上限时请求等待，不无限创建进程。最多 64 个尚未分配请求，队列满明确返回 NoWorker。

`--pool-size 0` 使用同一 host/包/服务作为无待命基线：请求到达后创建 worker，等待其就绪再绑定；没有请求时不保留空闲 worker。尚未测定最优池容量，默认小容量避免驻留过多前端。

## 3. 公共会话通道

默认 endpoint：`$XDG_RUNTIME_DIR/prism/launcher.sock`。

- Runtime 与 prism 子目录必须由当前用户拥有、模式 0700；socket 模式 0600，连接双方均检查 SO_PEERCRED 的 UID。
- 服务以私有 lock 文件和 flock 排他占用 endpoint；只清理已拒绝连接的同用户旧 socket，不覆盖普通文件/符号链接或仍有监听者的 endpoint。退出仅删除自己绑定的 socket inode；lock 文件可保留以便重启。
- 公共帧复用 PRL1 大端协议。type 1 为 LaunchRequest、type 2 为 LaunchEvent；本轮在 v1 中补充 type 3 LaunchCancel：沿用被取消的 request ID，instance=0，payload 长度=0。旧客户端无需发送新类型。
- Stream 非阻塞，完整处理分段/粘连帧与短读写，使用 MSG_NOSIGNAL。输入/输出各限制 256 KiB，每轮读/写预算 64 KiB、最多提取 64 帧；头部 payload 上限仍为 64 KiB。非法帧/超量/慢读取者关闭连接；未完整帧等待超过 5 秒关闭连接。
- 最多 64 个外部连接，每 endpoint 最多 256 个已接受请求，服务最多保留 4096 条实例/重放记录。终态记录在所属 endpoint 消失后释放。socket 断开不取消已接受的应用，应用实例可独立继续运行；请求者需要明确取消。

request ID 在请求者连接内唯一；服务使用独立会话内递增 ID 关联真实实例/worker，回传时恢复请求者原 ID。不同连接的 request=1 可以同时存在，不错配实例。

同连接、同 ID、同 app_id/模式的已接受请求重放已知事件，不重新创建应用；同 ID 换 app_id/模式为协议错误并关闭请求连接。包/未知应用等未接受请求会直接失败，不为其建立实例；调用者修正后使用新 ID。服务不会把 Accepted 当作启动完成。

## 4. Launcher↔Worker 私有通道

launcher 使用 CLOEXEC socketpair，将唯一控制端通过 spawn 文件动作传为 worker FD 3；其余服务描述符不继承。worker 检查控制 FD 的 peer UID、创建者 PID 和实际父 PID。私有通道不开放监听路径、不接受公共客户端指定的 PID/包路径。

PRW1 帧头为 12 字节：magic u32、version u16（1）、type u16、body length u32，全部大端。

| type | 正文 | 方向 |
| --- | --- | --- |
| 1 WorkerReady | preparation_ns u64 | worker→launcher |
| 2 WorkerBind | instance u64 + 完整 PRL1 LaunchRequest | launcher→worker |
| 3 自身事件 | 完整 PRL1 LaunchEvent | worker→launcher |
| 4 子应用请求 | 完整 PRL1 LaunchRequest | worker→launcher |
| 5 子应用回复 | 完整 PRL1 LaunchEvent | launcher→worker |
| 6 子应用取消 | 完整 PRL1 LaunchCancel | worker→launcher，协议支持；模块 ABI 尚无取消回调 |

Ready 与 Bind 各只允许一次；worker 发出的自身事件必须匹配其真实 PID、内部 request/instance，且按 InstanceState 推进。Accepted/WorkerAssigned/Exited 只由服务产生，禁止 worker 自报。业务模块不能通过公开 Launch API 设置 Shell 身份；正式会话的 Shell 角色在 WorkerBind 前由可信 WM 通道登记；无 WM 通道的诊断 host 为普通 XDG 窗口。

业务 launch_app 使用同一个私有控制通道，不每次启动 CLI、不额外派生 shell。host 将返回事件投递至模块，借用字符串只在回调期间有效。普通独立 host 的同一 API 使用 prism_launch_client 连接常驻服务，服务不可用则返回 0，无 spawn 回退。

## 5. 生命周期、超时与取消

Accepted 关联实例，WorkerAssigned 使用实际 host PID；RuntimeReady/SurfaceConfigured/FirstPresented/BackendReady 来自 host，其中 FirstPresented 仍只认 presentation-time。BackendReady 与呈现顺序独立。

服务每轮 WNOHANG waitpid 回收自己管理的 worker，并在没有新请求时继续回收/补池。Exited 依据真实 wait 状态：正常退出 0..255，信号退出为负信号值。初始化未完成便退出、非零退出或崩溃先报告 Failed，再报告 Exited；已报告的失败原因不会被后续退出覆盖。

生产 CLI 使用可轮询的 SIGCHLD 通知唤醒回收循环，服务要求独占 worker 的 waitpid 回收，不能在 SIG_IGN/SA_NOCLDWAIT 下运行；检测到外部回收会停止服务，避免向已重用 PID 发送信号。CLI 不虚构退出状态。

从 Accepted 开始的独立进程外 watchdog 默认 15 秒，可调 100..60000 ms，覆盖等待 worker、configure、首次呈现与业务 Ready。超时先返回 Failed/Timeout，再发 SIGTERM，1 秒后仍未退出则 SIGKILL，最后以 waitpid 结果报告 Exited。因此模块同步初始化阻塞也会被服务处理。host 自己的 10 秒前端超时仍保留；两者取先实际触发者。

收到身份和状态均合法的 worker 自报 Failed 后，先撤销窗口授权，将该 worker 标记为 Finishing，给予 250ms 自行清理退出的截止时间；届时仍未退出才发送 TERM，并在随后 1 秒升级 KILL。这样正常错误退出的真实状态可以保留，阻塞清理也不能等待整个启动 watchdog。用户取消、服务 watchdog 和会话停止仍立即进入 TERM 路径，不使用这段宽限。Finishing 与 KILL 截止时间都参与事件等待，不恢复周期轮询；Exited 始终来自 waitpid，不以错误类型推断退出码。

取消只允许原请求 endpoint 操作自己的 request ID：

- 未分配请求报告 Failed/Cancelled，无虚构 Exited；闲置准备进程随后按池目标收缩。
- 已分配实例先 Failed/Cancelled，再终止真实 worker 并报告 Exited。
- 已终态请求仅重放已有结果，不重复杀进程；未知 ID 返回 InvalidRequest。

会话正常停止时拒绝新请求，对活动/排队请求发送 SessionEnded，终止并回收全部实例/待命 worker，发送真实 Exited，清理 socket。worker 设置 PR_SET_PDEATHSIG=SIGKILL 并检查安装信号后的父 PID；launcher 意外死亡时 worker 不继续常驻。[PR_SET_PDEATHSIG](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html)。异常服务死亡时客户端得到连接断开；不能保证收到完整终态事件。

## 6. 当前明确缺口

正式会话使用第四步的可信 launcher↔WM 控制通道完成现有窗口激活，收到 WM 确认才发送 Activated=8。无 WM 通道的诊断 launcher 在不存在活动实例时创建；已存在活动/排队实例时明确返回 InvalidRequest，并说明需要 WM 激活通道。NewInstance 正常创建隔离实例；不会将无法完成的激活报告成成功。重复 ID 的重放与重复启动/激活是两个不同操作。

五个应用已统一为 host 模块/包，Dock 走 host API/真实映射实例订阅，WM 直接 spawn 已移除，session 统一监督 WM/launcher。ShellPermit 的生成/登记/可信 FD/pidfd/首次 surface 关联已接入，不以公共同用户 socket 授予角色；详细协议见 SESSION_LAUNCH_RUNTIME.md。

这一轮没有预热 EGL/GPU、实现 seed COW 共享、做节点增量布局/DisplayList 分块缓存或测零拷贝。CPU seed 审计与 GPU 预热策略仅在有实测收益和资源边界依据时继续设计。

## 7. 运行与验证

在有效 Wayland 会话中：

```sh
build-gles/bin/prism-launcher --pool-size 1
build-gles/bin/prism-invoker demo_player --new
```

invoker 等待实际 FirstPresented 和 BackendReady 后成功退出；`--wait-exit` 可继续跟踪退出，`--socket PATH` 指定私有 endpoint。它复用 SDK API，不自行验包或直接派生应用。服务可用 `--apps-root`、`--host`、`--wayland` 配置固定运行环境；公共请求无法改这些配置。

手动集成：`python3 tests/probes/launcher_pool_probe.py build-gles`。该检查用独立 headless 会话和 tests/ 模块验证真实提前就绪 PID 不被 exec 替换、补池、重放、两连接 ID 冲突隔离、显式激活拒绝、取消、SIGKILL、非法/分段 IPC、未知包、错误 ABI、模块崩溃、阻塞初始化 watchdog、模块 Launch API 回投、请求者退出后子应用继续、会话清理、pool=0 及 endpoint 重启。所有测试模块/探针均不安装。

第一轮 Pi 观察：单个空闲 worker PSS=3189 KiB；一次从开始发送分段请求到收到 Preview FirstPresented/BackendReady 的观察为 419.4 ms，包含分段发送等待和未预热 GPU 初始化。此单次记录不作为性能对比、p50/p95/p99 或速度收益宣称；完整硬件性能门槛仍在第五步。

## 8. 第三步最终验收记录

- GLES 全量构建成功；CTest 21/21 通过，包括注册根/ID/符号链接限制、分段/粘连/短写/缓冲上限/取消与私有帧、真实模块 Launch API 投影。
- 完整 launcher_pool_probe 通过，额外覆盖 invoker/公共 SDK、未分配请求取消、pool=0 收缩、父服务意外退出与残留 socket 重启。
- 最后一轮单次观察：预先就绪 PID 23411 被实际分配，进程身份/启动时间保持不变；空闲 PSS=3172 KiB；分段发送开始到 Preview FirstPresented/BackendReady 的观察为 648.0 ms。两次观察均非统计性能对比，不宣称已取得某个加速倍数。
- 临时安装检查包含新 host/launcher/invoker/音乐包，各 CLI help 可运行；无测试 fixture/probe 安装项。未重打 deb，未变更当前 Pi 物理演示或 systemd/session 配置。

第四步上述控制通道/授权/激活/四应用/Dock/session 已完成，下一步为第五步生产 deb 与物理安装会话验收。

## 2026-09-26 第四步更新

五个应用已统一迁到 host 模块/目录包；可信 WM 控制通道、Shell 一次性登记与 pidfd、真实 Activated 事件、Dock 映射实例订阅和 session supervisor 已接入。此前未切换描述为历史检查点。第五步 0.1.0-3 已部署到物理 Pi，显示确认正常，交互遮挡原因已定位（默认 HUD），0.1.0-4 已修复并安装，现场复核已通过；细则见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。

第五步最新发布检查点：0.1.0-4 已安装到物理 Pi，dpkg 完整性检查通过，默认 HUD 关闭；真实 V3D 首帧及创建/激活/取消/实例流复测通过。本轮 CTest 24/24 通过。现场输入复核已通过，规范和包路径见 PI_DEB_DEPLOYMENT.md。
