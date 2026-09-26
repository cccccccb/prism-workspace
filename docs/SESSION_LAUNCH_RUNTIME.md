# 统一会话、Shell 授权与应用实例

日期：2026-09-26。统一会话的实现规范，包含本轮 DSL 主题生命周期修订。已部署的 0.1.0-5 外观与交互记录保留在第 9 节；其中静态 JSON 与本窗口 Theme 开关属于历史方案，当前主题规范以 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 为准。本轮新增主题链的构建、测试与部署结果另行记录。

## 1. 唯一生产入口

```text
systemd / display manager
└── prism-session（设备和环境准备）
    └── prism-session-runtime（子进程监督与回收）
        ├── prism-wm（窗口、输入、合成）
        └── prism-launcher（包、实例、授权、预热池）
            ├── prism-app-host → desktop.so + DSL + wallpaper
            ├── prism-app-host → topbar.so + DSL
            ├── prism-app-host → dock.so + DSL
            ├── prism-app-host → player.so + Preview/Master DSL
            ├── prism-app-host → settings.so + DSL
            └── 未绑定待命 prism-app-host
```

五个业务模块只链接 `prism_contracts`，不创建 Wayland/EGL/Skia 对象，不组织 SDK Pump，不派生应用进程。所有前端生命周期由同一个 host 实现。独立进程仍用于隔离，统一前端控制与预热由平台负责。

生产构建不再生成或安装 `prism-desktop`、`prism-topbar`、`prism-dock`、`demo_player`、`demo_settings` 五个可执行入口，也不保留音乐 exec 适配器。WM 的 StartShellClients、Shell PID spawn 名单、临时 readiness 文件已移除。原 systemd 单元继续调用 `prism-session`，其 ExecStart 路径不变；脚本改为 exec 会话监督器，无需再启动第二个 launcher 单元。

源码树构建示例：

```sh
XDG_RUNTIME_DIR=/run/user/1000 WLR_BACKENDS=headless WLR_RENDERER=gles2 \
  build-gles/bin/prism-session-runtime
build-gles/bin/prism-invoker demo_player
build-gles/bin/prism-invoker demo_settings
```

`--wm`、`--launcher`、`--apps-root`、`--themes-root` 仅由会话管理者配置。原始 `prism-wm` 与无 WM 通道的 launcher 可用于诊断：原始 WM 不启动 Shell；诊断 launcher 明确拒绝已有窗口的激活，不能作为生产会话入口。

## 2. 可信控制通道与启动次序

监督器在派生任何进程前建立 `SOCK_STREAM | SOCK_CLOEXEC` socketpair；两端分别传入 WM 和 launcher 的 FD 3。继承端验证 SO_PEERCRED 的 UID 和 socket 创建者 PID，要求创建者是当前父监督器。公共用户 socket 与该通道完全不同。launcher 创建 host 时以 worker 通道覆盖 FD 3，其余 FD 都 CLOEXEC；应用不能继承 WM 控制端。

次序必须为：

1. launcher 编译当前主题包；WM 初始化 Wayland socket、输出/backend、presentation-time，生成非零会话标识并发送 Ready。首份主题安装前 WM 使用无预留带、无窗口装饰的中性启动参数。
2. launcher 收到私有 Ready 后，通过 PWC1 发送带 session 和非零 generation 的 InstallTheme；WM 在事件循环中校验、安装几何与装饰参数并回复 ThemeApplied。
3. launcher 确认 WM 成功 ACK 后，才创建固定的三个 Shell 包请求。
4. worker 完成自身公共 CPU 前端准备并发送 WorkerReady；launcher 通过 PRW1 下发当前 ThemeSnapshot，worker 安装到统一前端并 ACK 当前 generation。
5. launcher 分配真实 instance/PID，发送 Grant；WM 打开对应 pidfd 并登记。
6. WM Registered 成功且目标 worker 已 ACK 当前主题后，launcher 才发送 WorkerBind；切换事务期间暂停新应用绑定。
7. host 打开包和真实 Wayland 连接；WM 按该连接的内核进程凭据消费登记，确定场景层。

Shell 包 app_id → 角色只在 launcher 内部 bootstrap 中固定：prism_desktop → Desktop、prism_topbar → TopBar、prism_dock → Dock。公共 Launch API 禁止启动这些保留包，不能声明角色、任意可执行路径或授权凭证。未经登记的外部 Wayland 客户端继续作为普通窗口，即使它填写相同 app_id。

### 授权与 PID 生命周期

Grant 绑定 session/request/instance/真实 PID/role/32 字节随机 token/单调时钟过期时间。首版登记有效期为 5 秒；一次登记只能关联第一个 toplevel。WM 检查 pidfd 未报告退出、连接 UID/PID 匹配、登记未消费且未过期；Shell 还通过 ShellPermitGuard 消费。已登记但过期/身份无效的首次连接明确报 Wayland 错误，不降级为普通窗口掩盖启动失败。相同实例/PID 或占用中的 Shell 角色不允许重复登记。

token 在私有控制端传输，作为一次性登记关联值；当前并不要求业务模块在 Wayland 上发送 token。权限证明来自受控 FD 和内核进程身份，不能把 token 描述成客户端密码或业务进程之间的安全沙箱。pidfd 绑定具体进程寿命，避免仅用数值 PID 的重用问题；无 pidfd 支持时登记失败，不悄悄降级。参见 [pidfd_open(2)](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)、[getrandom(2)](https://man7.org/linux/man-pages/man2/getrandom.2.html)。

取消、失败、回收都发送 Revoke，WM 删除登记并销毁对应 Wayland client；进程终止和 waitpid 由 launcher 管理。重新会话产生新标识；旧 session 或非预期方向的消息导致控制通道失败。

### PWC1 v1 编码

固定 12 字节大端 header：magic `PWC1` u32、version u16=1、type u16、payload size u32。原类型 1..8 保持 70 字节 payload：session u64、transaction/request u64、instance u64、PID u32、role u8、token 32 bytes、expires_ns u64、success u8（0/1）；完整帧仍为 82 bytes。禁止 memcpy native struct。

类型：Ready=1、Grant=2、Registered=3、Revoke=4、Activate=5、Activated=6、Mapped=7、Unmapped=8，追加 InstallTheme=9、ThemeApplied=10。两个主题帧的 payload 为 session u64 + 有界 ThemeSnapshot/ThemeApplied 编码，不能解释为 Shell permit 或沿用固定 70 字节长度。Registered/Activated 必须回显正确 transaction/instance/PID/role；ThemeApplied 回显 generation、success、detail。过期终止请求的回复不再推进状态。WM 不接收 DSL、绑定或绘制命令。

### 当前主题与预热 worker

launcher 是当前主题及 generation 的唯一会话所有者，主题包安装在 `share/prism/themes/<id>/theme.prism`。默认 `glass`、公开选择接口、快照分发和失败恢复见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。WM 只消费纯值几何/装饰契约，SDK 只使用该快照解析材料及主题引用；业务模块通过可选 ABI 请求主题 ID，不能独立维护另一份全局配色。

active 和 idle hosts 都参加主题事务。池中新准备好的 worker 必须安装并 ACK 当前版本，之后才允许 WorkerBind，因此预热期间持有旧版本不会让新窗口沿用旧主题。切换不替换已运行实例的 PID、业务模块或节点 ID；任一参与端拒绝时，用更大的 generation 重发原主题内容。恢复失败或超时属于会话控制失效，沿用 supervisor 的会话清理。

ACK 表示参数已安装，不代表所有 surface 已在同一显示帧提交。客户端新样式及空效果区域随后通过自己的 commit 生效；WM 配置尺寸遵循 Wayland configure/commit。当前会话热切换先要求 ThemeLayout 不变，避免把尚未实现的跨 surface 几何事务当作原子切换。

## 3. 激活必须来自 WM 实际结果

ActivateOrCreate 没有活动实例时正常创建。有活动/排队实例时建立新的请求，关联原 instance/PID；等待原窗口 mapped 后发送 Activate。WM 找到已关联的普通 mapped toplevel，执行 FocusXdgView，然后回复 Activated。

公共 milestone 追加 Activated=8，原 0..7 保持。激活成功事件为 Accepted → WorkerAssigned（原 PID）→ Activated；不伪造 RuntimeReady/FirstPresented/BackendReady，也不启动第二个 worker。NewInstance 明确创建隔离实例。多个实例时选择最早的活动实例；当前尚未提供实例选择 UI。

激活请求不拥有旧 worker：取消尚未完成的激活只终止请求；取消已成功的激活重放结果。原实例的拥有者仍可取消原启动请求。窗口已消失或 WM 激活失败返回 Failed；CLI 收到 Activated 即成功，`--wait-exit` 可继续等待原实例真实退出。

## 4. Dock 订阅窗口事实

PRL1 追加类型 4 InstanceSubscribe、5 InstanceUpdate；头部沿用 request_id/instance_id，订阅 request_id 占用同连接请求命名空间。订阅帧无 payload/instance；更新 payload 是 PID u32、change u8、app_id 长度 u16 + UTF-8。

change：Reset=0、Running=1、Stopped=2、SnapshotDone=3。Reset/Done 使用 instance=PID=0、空 app_id；Running/Stopped 必须有真实实例/PID/合法 app_id。每连接只允许一份订阅；快照按 Reset → 当前 mapped 普通实例 → Done 排序，再接收变化。

生产状态来自 WM Mapped/Unmapped 和真实进程回收，不使用 spawn 或 BackendReady 推断。订阅不包含 Shell、激活请求别名或未注册外部 Wayland 客户端。无 WM 的诊断 launcher 使用真实 FirstPresented 作为近似窗口可见状态，只用于诊断。

PRW1 私有 worker 通道追加类型 7 Subscribe、8 InstanceUpdate，封装对应 PRL1 帧。SDK `SubscribeInstances` / `TakeInstanceUpdates` 与模块 HostApi `subscribe_instances` / Module `on_instance_event` 提供同一状态。Dock 按 instance_id 集合维护运行数量；重复 Running 不重复计数，取消/退出会减计数，连接断开 Reset。

C ABI v1 在结构尾部追加可选函数/事件。加载器逐字段按 struct_size 读取，缺失尾部回调置空，旧布局模块可以加载。新 Dock 要求 host 支持订阅；模块不得读取未声明的尾部字段。Launch API 和其异步事件语义保持原有前缀。

## 5. 业务迁移边界

Desktop 只提供标题绑定，图片从包内 assets/wallpapers 获取。Topbar 显示系统时钟、真实 up 网络接口名、电池容量；没有电池的 Pi 显示 AC。尚未实现通知中心与应用列表按钮，因此暂不显示无业务动作的按钮。

Dock 当前仅提供已注册的 Music/Pref 两个入口，直接通过 host Launch API 请求服务；未实现的 Files/Term/Web/Code 不提供虚假启动入口。Settings 从 /proc/stat 差值和 /proc/meminfo 获取 CPU 与内存，首个采样显示 sampling，刷新图标只刷新数据。当前主题选择通过 host 的 select_theme 请求 launcher，以 ThemeEvent 报告当前主题及结果；0.1.0-5 的本窗口 tint/文字配色 Toggle 已被该会话接口替代。音乐仍是交互业务 demo，尚无音频解码/输出后端。

第四步迁移当时不改变窗口尺寸安排、不实现毛玻璃或 BSP 树；后续 0.1.0-5 的视觉发布记录见第 9 节，本轮主题接口不据此新增效果或性能完成宣称。

## 6. 会话停止与故障

监督器在派生前记录已经存在的直接子进程（例如 systemd PAM helper），这些进程归原启动环境管理，不发信号、不等待其退出。正常监视只 waitpid 已知 WM/launcher；停止时遍历并回收属于本会话的直接及后来接管的后代，重复检查以覆盖主进程退出时新接管的 host。不能使用 waitpid(-1) 等到 ECHILD，否则 PAM helper 等父进程退出会造成循环等待。监督器拥有 WM/launcher waitpid，启用 CHILD_SUBREAPER 接管 launcher 意外退出后的 host 回收；任一主进程退出则停止整个会话。TERM 正常停止，3 秒后未退出的主进程 KILL。launcher 内部按既有 1 秒 TERM→KILL 回收所有 workers。worker 的父死亡 SIGKILL 与父 PID 二次检查防止游离运行。

Shell 任一关键实例退出/失败导致会话停止；普通业务崩溃只影响自己的实例。worker 自报 Failed 后同样进入 TERM→KILL 回收，避免业务 destroy() 阻塞时因请求已失败而跳过 watchdog。WM 控制断开终止 WM 主循环并完整清理，launcher 返回 SessionEnded 并结束；不单独重启一个 WM 留下旧授权和 workers。用户 systemd 单元的 Restart=on-failure 对整个 prism-session 生效；手动 demo 单元保持不自动重启，保留 KillMode=control-group/TimeoutStopSec=10。

正常退出删除私有 socket；launcher SIGKILL 可留下陈旧 socket，下一会话仍通过锁和 ECONNREFUSED 检查安全清理，不能宣称异常死亡能够发送完整终态事件。

## 7. 验证与下一步

验证源码/ABI fixtures/手动 Pi probes 仅在 tests/；BUILD_TESTING=OFF 不生成或安装它们。

```sh
cmake --build build-gles -j3
ctest --test-dir build-gles --output-on-failure
python3 tests/probes/session_launch_probe.py build-gles
python3 tests/probes/launcher_pool_probe.py build-gles
python3 tests/probes/app_host_probe.py build-gles
python3 tests/probes/shell_authorization_probe.py build-gles
python3 tests/probes/failed_worker_probe.py build-gles
```

第四步 Pi 验证：CTest 24/24 通过（包括控制编码/会话凭据、C ABI 旧尾部布局兼容、四个业务模块与 Dock 去重/断开）。独立 V3D/headless 会话验证三个 Shell 的真实角色、公共 Shell 启动拒绝、外部 app_id 冒充仍是普通窗口、同 PID/instance 激活（0/1/8）、NewInstance、取消激活不终止原窗口、实例快照/退出变化，以及正常停止、WM/launcher/Shell 崩溃后的完整子进程回收；确认全部 workers 使用同一 host 且未继承 WM 控制 FD。旧 launcher pool probe 的待命身份、补池/收缩、取消/崩溃/watchdog、模块子启动与陈旧 socket 重启也通过。额外授权探针验证重复/过期/已退出 PID 登记拒绝，以及登记后延迟的 Shell 不降级映射；WM 保持运行。失败 worker fixture 验证自报 Failed 后阻塞清理仍在 5 秒内被 KILL/waitpid 回收，早于设定的 60 秒启动 watchdog。生产 Release（BUILD_TESTING=OFF）构建成功，临时安装只有九个平台入口、五个 module/DSL 包与 desktop 图片，无五个旧客户端入口或测试 fixture/probe；从临时前缀、非源码目录启动统一 V3D 会话并完成创建/激活/实例流/停止检查。host 原有 Preview 同 surface、Ready 顺序和错误 ABI 回归通过。下一门槛是第五步：新的生产 deb、安装清单/服务检查、部署 Pi 实物会话与真实输入/显示确认。性能分位数、池容量与零拷贝不在本步宣称已验证。

## 8. 第五步物理部署

0.1.0-3 已部署；真实 HDMI-A-1 1024×600 / 59.821 Hz、五个 host V3D 首帧、应用创建/激活/取消与实例流通过。实机 service 正常停止约 0.347 秒，进程和 socket 清理完成。FirstPresented 的 detail 记录实际客户端 GL renderer，launcher 日志保留有效 worker 里程碑以便观测。PAM helper 边界已加入 tests/fixtures/session_parent.py 和 session_launch_probe.py --inherited-helper 回归；正常/WM/launcher/Shell 故障均通过。该版本现场反馈显示正常但部分点击无效，随后已在 0.1.0-4 修复并完成输入复核；详见 PI_DEB_DEPLOYMENT.md。

### 实机交互问题修订（0.1.0-4）

现场反馈定位到默认开启的 WM HUD：其 x=18/y=44/440×160 覆盖 Music 控件，输入仍到达下面的客户端，导致状态实际变化但看不见。生产默认关闭 HUD，场景节点在首次提交前就禁用，不闪现。诊断开关仅显式启用。Dock 增加 Opening/active/ready/failed 文字反馈：active 只由 WM Activated 驱动，ready 必须同时收到真实 FirstPresented 与 BackendReady，Accepted 不视为完成。该反馈是 Dock 业务绑定，WM 不接收应用 UI。0.1.0-4 的 Settings Theme 当时仅切换 demo 文字，不宣称全局换色。该版已安装，用户确认遮挡消失、播放切换和 Dock 反馈正常。

第五步发布检查点（历史 0.1.0-4）：已安装到物理 Pi，dpkg 完整性检查通过，默认 HUD 关闭；真实 V3D 首帧及创建/激活/取消/实例流复测通过。该阶段 CTest 24/24 通过。现场输入复核已通过，规范和包路径见 PI_DEB_DEPLOYMENT.md。

## 9. 历史 BSP 与图标玻璃主题发布检查点（0.1.0-5）

本节记录已发布版本的验收范围；静态 JSON/生成常量和本窗口主题开关已由 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 的 launcher 主题快照方案替代，不代表当前主题架构。

会话 supervisor、可信 Shell 授权、实例订阅及统一 host 的职责不变。真实普通 XDG view 已映射到 managed Window/TreeEngine；横纵嵌套、方向焦点/交换、工作区、关闭与 fullscreen 使用同一记录，Shell 仍在树外。该历史版本的主题由单个 JSON 生成纯契约，WM 不引入 DSL/parser/Skia；当前改为 launcher 编译主题 DSL 并传输纯值快照。

五应用已切换为图标主导的静态玻璃布局，保持当前壁纸；Dock 运行标记使用真实 app ID/instance 分组。SDK 负责透明 tint、控件、图标、布局及局部边框/阴影，Wayland 平台提交已布局的效果区域；WM 的 GLES pass 采样下层场景并生成背景材料与普通窗口外部装饰。不新增花哨动画。

该历史版本的 Settings 开关仅切换自身窗口配色，当时尚无全局运行时主题切换。其现场确认不构成本轮全局主题接口的验收。Music 仍是模拟播放进度的业务 demo，没有真实音频输出。

0.1.0-5 已构建并安装到 Pi，最终 CTest 26/26、Release 前缀/预存 helper 正常会话检查和四种停止/故障清理均通过；真实 launcher/host 双窗、三窗、四窗截图及 tree 尺寸验证通过。生产包无测试、旧客户端入口或 ImGui。

物理 HDMI 1024×600 会话的五应用使用 V3D，backdrop capability=1，本轮 journal 无 ERROR，协议启动/激活/取消检查通过。用户确认“外观正常，以上点击都正常”，覆盖播放图标、Preferences 本窗口配色 Toggle 和 Dock 激活；截图位于 `dist/validation/prism-v5-physical.png`。全部键盘快捷键、多屏/DPI、Slider、实时比例调整及性能分位数没有据此宣称完成。完整记录见 [VISUAL_TILING_REFINEMENT_PLAN.md](VISUAL_TILING_REFINEMENT_PLAN.md) 和 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)。

### Preferences 监控与配色更新

设置 demo 使用真实 CPU 差值、内存、可选 GPU 忙碌率/频率及 SoC 温度；Pi 的固定只读 firmware 查询通过受限设备 ioctl 完成，不每 tick 启动命令。监控暂停、采样周期和手动刷新由业务层管理。主题 ID 与明暗配色是同一 owner 事务中的独立 selector；重放身份包括两者，不能用同一 request ID 改换配色。协议默认请求/深色事件保持旧编码，非默认尾扩展以 version=1 与有界字符串编码；schema 2 快照公布实际配色，schema 1 保持 dark 兼容。八种组合必须经物理会话确认后记录验收。
