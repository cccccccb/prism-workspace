# WM 权威布局、组沉浸与分隔线控制实施计划

日期：2026-10-04。状态：**第一至第四步已完成代码实现；第三步验收见第 9 节，第四步细则和验证见第 10、11 节。**

本计划承接 [通用交互规范](INTERACTION_AND_PRESENTATION_SPEC.md)第 2、8、9、11 节。
实施顺序为：稳定树与边界身份、WM 权威快照 → typed 控制会话与连续手势桥接 →
组沉浸与可靠恢复 → 分隔线调节与尺寸约束。每步通过验收后再接下一步。
Topbar 的 action 只分派语义动作，系统操作必须经过权限与目标验证。

2026-10-01 范围修订：按用户确认，后续主体功能以**鼠标优先**推进。第三步和第四步
暂不设计触屏操作或安排实体触屏验收；已有通用 touch 输入兼容性保留，后续在主体功能
完成后另行设计触屏交互。键盘恢复继续作为独立兜底路径。

## 1. 第一步结束时的基础与边界

下表保留第一步交付时的基线；第二步更新见第 8 节。

| 范围 | 当前源码事实 | 后续缺口 |
| --- | --- | --- |
| 客户端输入 | 已接入 source/contact、取消、成功提交输入快照与 touch；验收状态见通用交互规范第 14 节 | 连续手势识别、阈值和系统控制桥接 |
| 原生布局 | `ArrangeXdgViews` 根据活动 workspace 安排窗口、Shell 和 XDG configure；布局快照区分目标和客户端提交 | 独立组模式、控制会话及呈现反馈 |
| 平铺树 | 支持多子节点与嵌套容器；稳定节点/邻接边界身份与不可变树快照 | 递归最小尺寸与受约束的比例操作 |
| 节点身份 | WM session 内不复用的节点、workspace、输出和边界 ID；客户端 Scene 身份保持独立 | 第二步用这些身份校验控制目标 |
| 权限 | launcher/WM 私有通道核对 WM session、PID、实例与一次性 ShellPermit | 持续控制能力、操作范围及会话撤销 |
| 实例订阅 | launcher 的 Running/Stopped 事件保留；私有 WM 通道新增权威布局与焦点快照订阅 | worker/Host/Shell 的 typed 状态桥接 |
| 模块接口 | 通用 Host 提供状态、释放确认后的 action、启动及实例事件 | 连续 Begin/Update/End/Cancel 与控制结果 |
| 全屏 | XDG view 的单窗 fullscreen 隐藏其他普通窗口及 Topbar/Dock | 保留整个平铺组的沉浸模式和恢复入口 |

`PointerButtonEvent` 与 `TouchDownEvent` 已保留 `protocol_serial`。它是 opaque 平台
凭据，默认零表示不可用；仍需 WM 结合真实 seat、surface、输入序列和已授能力验证。
客户端 `InputSource` 与 receipt time 不能自行构成系统授权。

旧 `Compositor::OnPointerMotion/OnPointerButton` 的分隔线逻辑在存在原生窗口时返回，
并使用固定屏宽及比例范围。原生分隔线应接入 TreeEngine。当前 split 布局只保证极小的
可用尺寸，尚未把 XDG `min_width/min_height` 汇总为子树约束。

本计划首版只处理当前输出上的活动 workspace 平铺根。协议仍显式携带 output、workspace
与 group 身份；尚未支持的多输出工作区请求明确拒绝，不能隐式改用第一个输出。

## 2. 第一步：稳定身份与 WM 权威快照

### 交付范围

新增 WM 专用 typed 布局契约，客户端 Scene 的 `contracts::NodeId` 继续保持本地用途。

| 建议类型 | 身份与生命周期 |
| --- | --- |
| WmSessionId | 使用 WM 会话代数；WM 重启后旧请求、订阅和控制全部失效 |
| OutputId | WM 分配 ID/代数；拔除后重建得到新身份，名称仅用于显示 |
| WorkspaceId / GroupRootId | workspace 与其平铺根的稳定身份，不能由名字或焦点临时推导 |
| LayoutNodeId | WM 分配 ID/代数，覆盖 view 和 container；删除、复用不能产生旧句柄别名 |
| BoundaryId | WM 分配，绑定父容器、相邻两个子节点及分割方向；邻接关系消失即撤销 |

一个有 N 个子节点的 split 容器有 N−1 个边界。BoundaryId 不使用数组下标，也不只使用
父容器 ID。重排后仍存在的同一邻接关系可保留身份，消失后重新形成的关系获得新身份。

WM 发布完整初始快照及有序更新，包含输出逻辑坐标、workspace/group、目标与已提交窗口
几何、实际模式、活动实例和边界描述。第一版只表示现有 Normal 组模式，边界
`resizable=false`；可用控制操作在第二步加入，不能把已有边界误认为已支持拖动。快照由 WM 的真实焦点与布局变化触发，
Dock 不通过自身 launch 请求的 Activated 回复推测全局焦点。

- `topology_revision` 随插入、删除、重排、重新挂接与布局方向变化推进。
- `layout_revision` 随目标几何、比例、模式、主题及输出尺寸变化推进。
- 显式组模式操作记录独立 `mode_revision`，供后续覆盖模式恢复使用。
- 控制会话自身的比例更新不改变拓扑修订；快照明确区分 configure 目标与客户端已提交几何。
- 初始化、更新、断线及重订阅均有边界和数量上限；队列超限采用明确的关闭或重新同步策略。

树结构与比例修改通过能更新修订的具名操作完成。已有插入、剪枝、交换与重新挂接路径
纳入修订；调用者不能直接改写子节点向量和比例，避免有布局变化却没有修订更新。

### 验收门槛

验证三子节点和嵌套容器的边界枚举、重排与中间节点删除；旧 ID/代数不得指向新目标。
输出重建、workspace 切换和 WM 重启后旧请求失败。键盘、鼠标、触摸、实例激活、映射与
卸载导致的焦点变化都进入权威快照；首次订阅和断线重订阅得到一致状态。

## 3. 第二步：typed 控制会话与连续手势桥接

### 交付范围

沿已有继承的 worker → launcher → WM 私有通道增加 typed 请求和回复。launcher 根据
真实 worker/job 身份附加授权信息，WM 再核对会话、实例、PID、角色及允许操作。
普通 Launch API 不接受调用者自报的 Shell 角色或系统权限。

一次性 ShellPermit 用于启动注册；持续能力绑定已注册实例与端点的生命期，并在撤权、
实例退出或端点失效时收回。后续控制不能反复消费启动凭据，也不能沿用已退出实例的能力。

| 阶段 | 建议信息与行为 |
| --- | --- |
| BeginControl | ControlTarget、输入凭据、预期修订、操作类型；接受后由 WM 分配 ControlSessionId |
| UpdateControl | 会话 ID、严格递增序号、有限且有范围限制的位置或进度 |
| EndControl | 最终意图；完成一次，重复请求返回一致结果 |
| CancelControl | 明确取消；释放捕获、预览和会话资源，迟到 Update/End 不得复活操作 |
| ControlResult | request/session ID、接受/拒绝/取消原因、当前修订与权威状态 |

首版可限制每个 workspace 同时一个修改布局的会话，其余输入得到 Busy。权限属于端点
及已授能力；action 字符串、目标 ID 或非零 serial 本身均不能授予权限。

通用 Scene/SDK 增加连续手势生命周期，区分 captured 与 dragging；Host 将已确认的
手势转成 typed 请求，再向模块和 DSL 反映 WM 结果。业务模块不逐帧重新实现命中或输入
仲裁。既有模块 C ABI 采用有 `struct_size` 检查的可选尾部扩展，普通模块不能获得 Shell
控制能力。原生 serial 的验证需要匹配实际输入序列；WM 必要时维护一次性凭据记录。

会话锁定目标和起始输入。目标或边界删除、外部拓扑变化、workspace 切换、输出/约束
变化、设备撤回、UI 替换、Shell 撤权及端点断开都必须取消或明确拒绝继续。
建议会话持有临时比例预览，End 再提交持久比例；Cancel 移除预览并按当前树重新安排，
避免用旧矩形或比例覆盖期间发生的新操作。

连续 Update 只合并同一会话中尚未处理的位置，不能跨 Begin/End/Cancel。限制在途请求、
会话数与队列字节；超限和断线要清理会话。协议保持 typed 二进制编解码，明确版本、
类型编号、载荷长度与字段范围，不能直接复用调试 `prism-msg` 文本命令。

### 验收门槛

验证普通应用、伪造角色、过期 WM session、错误实例/输入凭据均被拒绝；多触点与鼠标
争用不会转移会话所有权。重复 Begin/End、乱序序号、取消后的迟到更新、队列超限及
断线均有确定结果。外部操作之后取消旧会话不得恢复陈旧布局。

## 4. 第三步：组沉浸与可靠恢复

### 交付范围

WM 按 output/workspace 保存 `Normal/Immersive`，与现有单窗 XDG fullscreen 分开。
沉浸时使用输出可用范围安排整个组，隐藏 Topbar/Dock，保留成员、拓扑及分割比例。
退出按当前输出、主题和存活窗口重新安排。外边距策略单独定义，不能通过逐帧修改主题
或 Shell 保留带实现收起动画，也不把组成员逐个设置为 XDG fullscreen。

组模式和单窗模式分别保存。进入、退出单窗模式遵从最新合法组模式修订；明确的
“恢复桌面组”操作可以清除覆盖模式。组操作只在 WM 确认后更新 Shell 的实际模式状态。

隐藏 Topbar 前同时完成三条恢复路径：

1. WM 独立键盘命令直接恢复当前组；现有 Super+F 仍是单窗 fullscreen。
2. WM 保留窄范围边缘入口，在 Down 时确定系统或应用归属，唤出受信任恢复控件。
   已交给应用的鼠标按键序列保持原归属；不在中途重放点击，也不让全屏透明输入区域长期
   覆盖应用。边缘尺寸与阈值留在布局控制策略中，后续用实机校准。
3. Shell 退出/崩溃后，WM 取消该端点会话并保持可恢复状态；输出和 workspace 改变时
   清理边缘手势与临时唤出状态。

第三步开始前 launcher 在 Shell worker 失败或退出后设置 `control_failed`，最终
session supervisor 会终止 WM。本步一起调整故障策略：撤权和取消控制、WM 恢复安全
组状态，并保持会话支持恢复。实际采用的无自动重启策略见第 9 节。

Topbar 继续使用统一 Host/DSL；语义动作调用第二步的 typed 能力。边缘唤出可临时显示
受信任控制视图，WM 限定其位置和输入范围。Shell 可见性动画不改变应用输入所有权。

### 验收门槛

至少两个原生应用进入沉浸后均可见，拓扑与比例保持，客户端 XDG fullscreen 标志不因
组操作改变。验证退出、主题/输出改变、workspace 切换、单窗 fullscreen 交错和较新的
组操作。本轮验证鼠标和键盘恢复，触屏延期；边缘输入不泄漏为应用点击；Shell 崩溃后仍能
恢复。一次模式变更产生明确布局目标，动画采样不反复 configure 全部窗口。

### 第三步鼠标操作约定（2026-10-01）

- Topbar 横线保持固定命中区；鼠标主键拖动识别阈值为 6 逻辑像素，向下位移至少
  24 逻辑像素且纵向占优时，释放提交 EnterImmersive/ExitImmersive。短拖和反向拖动
  只结束跟踪，取消不更改组模式。实际选中态来自 WM 快照。
- 沉浸时平铺组使用完整输出，外边距为 0，保留主题内间距、成员和分割比例。
  单窗 XDG fullscreen 独立保存；显式恢复组清除当前组的单窗覆盖。
- 组沉浸或单窗 fullscreen 隐藏控制栏时，鼠标主键按下于输出顶部中央 160 × 6 逻辑像素
  入口，WM 消费该次完整按钮序列并临时唤出原 Topbar。下一次横线拖动可以恢复。离开
  Topbar 范围且没有按键或待完成输入时收起；已有应用拖动不因进入边缘而被接管。
  位置与输入范围由 WM 限定；释放后的在途输入沿用第二步最多 2 秒的宽限。
- `Super+Shift+F` 恢复当前组并清除单窗覆盖；`Super+F` 保持原单窗 fullscreen。
- Shell worker 失败时撤权、取消控制并恢复安全组状态，WM/launcher 与普通应用保持
  运行。本阶段不自动重启 Shell；WM/launcher 本身或私有控制通道失效仍停止整个会话。
- workspace、输出身份/几何改变清理临时唤出状态；组模式按当前支持的输出/workspace
  保存，输出丢失后回到 Normal，修订不可倒退。触屏不启用新的组控制意图。

## 5. 第四步：分隔线调节与尺寸约束

### 交付范围

从已提交的 XDG `min_width/min_height` 镜像窗口约束，在 TreeEngine 中递归汇总子树
最小尺寸和 gap。水平分割沿宽度求和、垂直分割沿高度求和，交叉方向取最大值；tabbed/
stacked 子树纳入其头部占用。父容器空间不足时明确禁用该边界操作或报告受限范围。

拖动只调整选中边界两侧相邻子树，保持二者比例之和及其余兄弟比例。位置在父容器
逻辑坐标中解释，根据约束夹取；拒绝 NaN、无穷及失效目标。持续更新导致约束变化时，
按第二步的会话规则取消或重新验证，不能静默改控另一条边界。

分隔线 UI 由可信 LayoutControls Host 承载。WM 发布边界几何与允许输入范围，Host 用
通用 DSL 呈现。若增加专用角色，必须同时更新 WindowRole、ShellPermitGuard、control
编解码角色范围、Shell bootstrap、ShellRect 与 scene 层级映射。WM 限制实际输入区域，
不能完全相信客户端自报的全屏 input region。触摸目标大小需要实机校准，较窄分割缝
可使用明确唤出的受限控件。

按提交能力合并拖动中间目标，跟踪 configure 序号及对应的新 buffer。控制已接受、
布局目标已更新、客户端已提交新尺寸、输出实际呈现分别记录。旧 buffer 的视觉缩放
预览不能作为新布局已可交互的依据。

### 验收门槛

覆盖水平/垂直、三个以上兄弟、嵌套子树、tabbed/stacked 最小尺寸、极小输出、大 gap、
客户端动态改变约束及无法满足的约束。验证拖出边界、取消恢复、多设备争用、窗口关闭
与拓扑变化。慢客户端、configure 积压及丢弃帧情况下队列仍有界，输入坐标与实际采用
的几何一致。本轮以鼠标验证为准；触屏交互和实体触屏验收留待后续专项。

## 6. 文件职责与交付规则

下表保留整条主线的职责划分。第一步的实际文件见第 7 节；其余文件仍为建议。

| 职责 | 新增建议 / 既有接入点 |
| --- | --- |
| 布局契约与编解码 | 新增 `contracts/layout_control.hpp` 及独立 typed 编解码；接入 `launch/control_protocol.*`、`worker_protocol.*` |
| 树身份、边界与约束 | `tree_node.*`、`tree_engine.*`、`tree_container.*`；按职责新增 `tree_boundary.cpp`、`tree_constraints.cpp` |
| WM 模式与会话 | 新增 `wm/layout_control.cpp`、`wm/group_mode.cpp`；接入 `wlr_server_views/control/input/touch/frames` |
| 授权、订阅与故障恢复 | `launcher_runtime/service_control/requests/workers/loop`、`launch/shell_permit.*` 与 session 生命周期 |
| 通用手势和 Host 桥接 | Scene/SDK、`host/worker.cpp`、`module_session.*`、`contracts/app_module.h` |
| Shell 呈现 | Topbar、Dock 的权威状态接入及后续 LayoutControls 包；按项目 [prism-app-ui Skill](skills/prism-app-ui/SKILL.md) 开发 |

每步独立补充协议、树算法和原生 Wayland fixture；测试与 probe 留在 `tests/`。
遵循 [代码规范](CODING_STYLE.md)，按职责拆分新增编译单元，保持 800 行上限。
阶段报告分别列出源码能力、自动化验收、实体设备校准、实际安装与运行会话更新情况。
未完成的能力继续标记后续，不以 mock、调试 IPC 或动画预览代替生产控制链路验收。


## 7. 第一步源码交付：权威状态与订阅

### 7.1 生产链路与身份

`TreeEngine::CaptureSnapshot()` 冻结树拓扑、比例、节点及相邻 split 边界；
`WlrServer::GetLayoutSnapshot()` 组合 WM 实际焦点、原生窗口可见性、fullscreen、客户端
提交几何和输出身份。只在内容变化时发布新的不可变快照，读者持有的旧快照不会被改写。

节点、workspace 根及边界使用会话内不复用身份，不能从地址、数组下标或应用名称推导。
同一 split 邻接关系保留边界 ID；邻接消失后再次形成会得到新 ID。输出拔除后再创建使用
新 ID，即使显示名称相同。每份传输快照都携带非零 WM session，WM 重启使旧会话失效。

客户端 Scene 的输入快照与这里的布局快照职责不同：输入快照保存一次实际提交对应的
客户端命中几何；WM 布局快照描述权威树、输出和窗口状态。二者不能互相替代，也不把
WM 节点 ID 解释成客户端 Scene NodeId。

### 7.2 修订与几何语义

`revision` 是发布序号，`topology_revision`、`layout_revision`、`focus_revision`
分别描述拓扑、布局目标/模式/主题/输出和焦点的变化。客户端新 buffer 的提交可能只
更新已提交几何和发布序号；读者不能只看布局目标修订判断画面是否追上配置。

每个窗口同时保留 `tile_bounds`、`target_bounds`、`committed_bounds` 和
`has_committed`。目标可能已经改变，客户端仍显示上一尺寸的 buffer；纯位置调整则会
更新已显示 buffer 的位置。`committed_bounds` 是 WM 观察到的客户端提交结果，**不是
输出实际呈现确认**，也没有承诺等待了 compositor/GPU fence。

权威焦点包括键盘导航、鼠标点击、触摸 Down、实例 Activate 及窗口 map/unmap 所产生的
变化。`active_instance=0` 可表示没有活动实例或焦点窗口来自未注册的外部 Wayland
客户端，不能据此丢弃实际 focused 节点。Shell 不能通过自己上次的 Activate 回复推测
当前全局焦点。

版本 1 只支持单输出配置中的主输出活动 workspace；接入多个输出时全部明确
`supported=false`，未分配的 workspace 使用 `output=0`。此字段不授予多输出布局能力。组模式仍为 `Normal`；单窗
fullscreen 仅写在对应窗口上，不能伪装成组沉浸。边界始终 `resizable=false`。

### 7.3 私有协议和有界缓存

继承的 launcher/WM 私有通道新增 `LayoutSubscribe` 与 `LayoutSnapshot`。launcher 在
Ready 后用已确认的 session 订阅，WM 回送完整当前快照，再发送有序更新。取消订阅后
停止更新，重新订阅重新获取完整快照。会话错误关闭通道，不能回退到调试 IPC 或接受
请求中自报的身份。

快照采用 version 1 typed 二进制，整数为网络字节序，浮点为 IEEE754 binary64。
WM 先把输出/workspace 的显示名称规范成有界 UTF-8 标签，身份仍由 ID 决定；原始名称
不作为控制目标。编码和解码都校验有限数值、名称 UTF-8、唯一身份、根/父子关系、
同 workspace 引用、
无环树以及完整的相邻 split 边界。载荷上限 192 KiB，最多 16 个输出、64 个 workspace、
512 个节点、512 个边界和 512 个子节点引用；名称最多 128 字节。Stream 总队列维持
256 KiB 上限，传输队列超限关闭端点；不存在无界快照历史或静默漏掉部分节点的降级路径。

生产修改入口先检查容量。创建 workspace、插入/拆分/分组等操作在超过节点或 workspace
上限时拒绝，保留既有树；原生新窗口无法纳入树时终止该客户端连接并给出容量错误，
不破坏 launcher 的有效订阅。超过输出上限的新输出不纳入布局管理与快照。序列化校验仍是
第二道边界，不通过事后截断快照掩盖接入数量。

launcher 的 `LayoutSnapshotCache` 仅接受同会话、更大的发布序号和不倒退的子修订，
以 `shared_ptr<const LayoutSnapshot>` 替换当前值；旧读者按引用生命期释放。断线清空
缓存，重启重新握手。未来主动重订阅的调用方须先 `Reset(session)`，把完整回复作为
新订阅的基线，不能把它当作旧订阅中严格递增的更新。**这一步的生产订阅止于
WM → launcher。** worker、Host、模块及
Topbar/Dock 的状态接口将在第二步接入；当前桌面 UI 尚未消费这份新快照。

### 7.4 文件与验证

- `contracts/layout_snapshot.hpp` 与独立编解码实现负责可传输数据和边界校验。
- `tree/tree_snapshot.*` 负责不可变树状态，现有具名修改入口维护稳定身份和修订。
- WM 独立快照实现负责原生状态组合、输出身份和发布；control 通道承载订阅。
- launcher 的独立缓存校验会话、发布顺序与修订，失联后清空。
- `tests/native_layout_snapshot_test.cpp` 用真实 XDG SHM 客户端和继承的私有端点验证
  订阅、焦点、窗口生命周期、目标与提交几何、输出重建与会话拒绝；测试设备通过
  wlroots 信号注入，不表示实体触摸屏验收。

本轮 Pi 完整构建通过，CTest **64/64** 通过，包括树身份/邻接边界、协议非法载荷与
缓存修订、容量拒绝以及原生布局快照。原生 fixture 验证鼠标、真实 wlroots touch 信号、
键盘及实例 Activate 的权威焦点，目标/提交几何、输出重建和 2× 输出缩放；客户端补交
buffer 只推进发布序号，空闲读取复用快照。输出平移与实际整数目标几何修正后的最终
完整构建通过，五项定向回归 **5/5** 通过；暂停客户端时平移输出，目标及已提交窗口
几何立即同步移动且尺寸不变，输出/窗口 ID 与拓扑修订保持，恢复原位同样无需新 buffer。

隔离 V3D 会话的 `session-launch` normal 场景通过：真实 Shell 授权、实例激活、状态
订阅和进程清理正常。首次与完整 CTest 同时运行时，Desktop 模块入口超过现有执行预算；
停止并行负载后的串行复测通过，生产预算未放宽。首轮定向测试中的旧协议测试目标未
重建，以及 fixture 在 EOF 剩余完整帧尚未排空时过早断言 Closed，均已纠正并复测通过。
初始失败日志保留，未用它们代替最终结果。

证据位于 `dist/validation/wm-layout-snapshot-v1-20260929/`，包括 `build-full.log`、
`ctest-full.log`、`build-final.log`、`target-tests-final.log`、`session-launch.log` 和
`style-final.log`；首轮记录为
`target-tests-initial.log`、`session-launch-initial-budget-failure.log`。
代码格式、goto/800 行门槛通过，测试与 fixture 留在独立测试目录。

组沉浸、恢复入口、控制会话、分隔线交互、递归最小尺寸、连续手势、交互节点变换和
新的动画效果均未在本步启用。本轮未打包安装或替换现有桌面/VNC 会话；实体触屏未验收。

## 8. 第二步源码交付：连续手势与 typed 控制会话

### 8.1 输入声明与业务边界

`InteractionTarget` 新增静态声明 `.gesture(action: "group.track", threshold: 6)`。
阈值为有限的逻辑像素距离，范围 `[0, 1024]`，默认 6；action 非空且最多 128 字节。
声明只安装通用拖动识别器，不携带 WM 操作名称、窗口身份或 Shell 权限。
普通应用同样可以用于自己的滑块、拖动预览等交互。

```prism
InteractionTarget(action: "group.activate", width: 104, height: 28) {
    Visual { Card(width: 80, height: 5, background: #DCE3EEFF) }
}
.gesture(action: "group.track", threshold: 6)
```

上例仅说明输入声明，实际尺寸与样式仍由主题和布局决定；不能把 Visual 的动画外观
误认为已支持变换后的交互命中。交互节点变换仍属于后续步骤。

Down 保留实际已提交画面的输入快照身份、原始 serial、source、contact 和起点。
移动达到欧氏距离阈值才发布 Begin，之后发布 Update，释放发布 End；未达到阈值仍走
已有点击逻辑。拖动被认领后不再触发同一次点击，移出控件仍可完成捕获序列。
`dragging` 加入通用 interaction state，可以驱动 Visual 的状态规则与时间动画。

Gesture ID 在进程内单调递增，不因 UI 替换而复用。隐藏、禁用、移除、修改动作或识别器、
失去有效输入、设备取消、UI 替换与关闭都会终止相应序列。每个触摸 contact 独立识别。
Scene 提供拥有数据的事件批次，SDK 在 UI owner 线程调用 `OnGesture`；回调可替换 UI，
旧 UI 的后续 Update 不会继续交付，已交付 Begin 的终止事件只交付一次。

### 8.2 Host 与模块接口

业务模块 ABI v1 追加可选尾部，loader 继续按 `struct_size` 读取，旧模块无需重新实现
新回调。`contracts/app_control.h` 定义 C 结构体视图：

- `on_gesture` 接收本地连续手势，普通客户端可使用。
- `subscribe_layout` / `on_layout_state` 接收 WM 布局快照；数组与字符串只在回调期间有效，
  保存信息须复制值，不能保留借用指针。
- `control_gesture` / `on_layout_control_result` 提交并接收 WM 控制会话。

模块在真实 Begin 的 `on_gesture` 回调中选择操作类型及来自当前布局快照的 target。
Host 绑定当前 Gesture ID，从 SDK 事件填入 serial/contact/位置，管理 request/session/sequence。
模块不能在这个 API 中自行提供这些输入凭证。后续 Update、End、Cancel 由 Host 自动转发；
模块可在 End 回调中设置最终 intent，也可提前取消自己已建立的控制序列。

```c
/* self->target 必须由 on_layout_state 复制当前活动 workspace 和修订。 */
static void on_gesture(void *context, const PrismGestureEventV1 *event)
{
    struct App *self = context;
    if (event->phase != PRISM_GESTURE_BEGIN_V1) {
        return;
    }

    PrismLayoutCommandV1 command = {0};
    command.struct_size = sizeof(command);
    command.gesture_id = event->gesture_id;
    command.phase = PRISM_GESTURE_BEGIN_V1;
    command.operation = PRISM_LAYOUT_GROUP_GESTURE_V1;
    command.target = self->target;
    self->host->control_gesture(self->host->context, &command);
}
```

调用前须像其他 ABI 尾部一样检查 Host `struct_size` 和函数指针；模块声明
`on_layout_state`、`on_layout_control_result` 以消费真实结果。完整可编译的 C 模块用例见
`tests/fixtures/layout_control_module.c`，它是测试 fixture，不随产品安装。

每个 Host 最多保留 16 个控制序列，每个序列只有一个在途请求。等待 Begin/Update ACK
期间，连续移动合并成最新一个 Update，End/Cancel 单独保留。Cancel 丢弃尚未发送的移动，
End 在最后一个已保留 Update 之后发送；始终拿到非零 WM session 后才发送后续阶段。
同步函数返回成功只代表请求已接收，最终能力与结果以 WM 回复为准。

### 8.3 生产传输与 WM 权威

生产链路为 `Scene → SDK → Host/module → worker → launcher → WM`，结果原路返回。
worker 与 WM 私有协议追加 typed 二进制消息，已有消息编号保持；公共 launch socket
不增加系统控制入口。layout state 经 launcher 当前缓存进入 worker/Host/module。

launcher 从实际绑定 job 填入 PID、instance、启动 request、role 和 WM session；不接受
worker 自报身份。已注册并绑定的 Topbar/Dock 可订阅状态，以便 map 前准备 UI；只有
已 map 的 Topbar 可请求系统控制。普通应用和 Dock 的控制请求返回 Unauthorized。
WM 再检查 registration、pidfd 存活及实际 mapped/visible 原生 surface。

WM 记录真正发给该客户端的 pointer/touch Down serial，并绑定 kind/contact 与上述身份。
serial 为零、猜测 serial、另一实例或其他 contact 均不能开始会话。同一个输入凭证只能
消费一次；busy 拒绝也消费该次输入。普通释放保留 2 秒宽限，以容纳 Wayland Up 先到、
异步 Host Begin 后到的快速操作；native Cancel、设备拔除、surface 隐藏或销毁立即作废。
Down 凭证与会话最长 120 秒，控制会话 30 秒没有有效请求则过期，释放宽限届满也会取消。
这些是输入控制的有界生命周期，不是动画时钟或动画时长限制。

Begin 锁定非零 WM session、output/workspace/root/boundary 身份和 topology/layout 修订。
仅支持当前单输出活动 workspace，每个 workspace 同时最多一个会话。目标/输出消失、
workspace 切换、拓扑或布局修订变化取消会话；纯焦点变化和客户端 buffer 追上目标所产生
的发布修订不会无故取消。会话内目标、操作与输入凭证不可切换，sequence 严格递增。
已认证会话提交错误目标或错误序号会取消该会话，避免 Host 结束而 WM 仍占用控制权。

WM 使用最多 64 个输入记录与 256 项重放记录。相同身份和 request 的完全相同请求取得
缓存结果，篡改同 request 的内容被拒绝，并取消该重放记录关联的活动会话；取消后旧请求
不能恢复会话。Host 接受 WM 异步 Cancelled，即使它引用的最后请求已经 ACK。
断线、Revoke、worker 退出与 surface unmap
都会清理状态。传输有明确载荷与队列上限，布局快照仍最多 192 KiB，不保存无界历史。

### 8.4 当前交付能力与后续步骤

第二步交付时实现 `GroupGesture` / `BoundaryGesture` 的跟踪会话。End intent 为 None 时返回
Ended，`applied=false`。EnterImmersive、ExitImmersive、ApplyBoundary 均明确返回 Unsupported
并终止会话；没有假装已经应用布局。传输中的位置仍是客户端局部逻辑坐标，不直接作为
受信任的窗口几何输入。

第二步交付时主题、Topbar/Dock 及应用 UI 保持现有效果；新接口没有自动把所有本地手势变成系统
操作。第三步接入组沉浸状态机、恢复入口与 Shell 行为（见第 9 节）；随后在递归尺寸约束和 WM 几何
计算具备后接入分隔线调整。边界 `resizable` 在本阶段继续为 false。

### 8.5 验证记录

Pi 完整构建通过，最终 CTest **70/70** 通过。新增六项测试覆盖 typed 协议、Scene 手势、
WM 权威校验、Host 桥接、C 模块 ABI 和原生控制链路：

- 协议拒绝非法枚举、载荷和目标；Host 保留终止顺序、合并未发送的 Update，并处理异步取消。
- Scene 验证阈值、点击与拖动互斥、捕获、独立触点、隐藏/移除和 UI 生命周期取消。
- WM 验证原始输入凭证、身份和修订、单次消费、Busy、重放、过期和错误续传后的清理。
  同 request 改写载荷会终止关联的活动会话，完全相同的重放仍返回一致结果。
- 原生 Wayland fixture 使用实际映射的 Shell 客户端和 wlroots 输入事件，验证真实 serial、
  释放宽限、主题/布局失效、unmap，以及 pointer/touch 设备销毁后的取消。

隔离 V3D 会话的六项 SDK 门槛全部通过：Prepared UI、提交、损伤、动画、输入快照、
输入与动画。输入快照 probe 验证旧画面命中、连续手势，以及回调内替换 UI、隐藏目标和
Close 后的终止事件唯一性。它先有界等待成功像素提交，再检查稳定状态；计数短暂不变
不能单独证明新帧已经提交，也不等于显示端已呈现。

launcher 路由 probe 用真实 launcher、测试 worker 和模拟 WM 端点，验证普通应用与 Dock
的权限拒绝、Topbar 通道及不可变 owner 匹配。另行串行运行真实 supervisor/WM/launcher/
Host 的 normal 会话回归，Shell 授权、实例激活、订阅和进程清理通过。两者的覆盖范围
不同，模拟端点测试不冒充完整桌面操作验收。

初轮问题与最终结果分别保留：集成重建后 Scene 取消时间断言通过；一个生成的 worker
对象文件存在非法重定位，删除该对象并重新编译后链接通过；旧 DSL 负例仍把新增的
`dragging` 当作非法状态，已改为真正未知的状态名。首轮 GPU probe 的固定等待窗口未
确认新帧提交，独立复测通过后增加上述成功提交等待，最终六项全量复测通过。没有放宽
生产执行预算或强制额外渲染来通过验证。

证据位于 `dist/validation/layout-control-v1-20260930/`：`build-verified.log`、
`ctest-verified.log`、`launcher-routing.log`、`session-launch.log`、
`sdk-final/native-gates.json` 及各项 GPU 日志；代码规范检查记录为 `style-verified.log`。
格式、goto/800 行门槛及 `git diff --check` 通过；当前 319 个生产文件中最大为 627 行。
测试、C ABI fixture、原生 Wayland 客户端与虚拟输入 probe 均保持在 `tests/`，不进入运行包。
未部署或替换当前 Pi 桌面/VNC 会话；测试输入不等同实体触屏验收。

## 9. 第三步源码交付：鼠标组沉浸与恢复

本节记录 2026-10-01 接入的第三步，验证结果见本节末尾；第四步分隔线
尺寸约束仍未启用。本步优先鼠标，既有触摸协议继续兼容，新的触屏组控制不启用。

### 9.1 WM 模式与应用布局

`LayoutGroupMode` 增加 `Immersive=1`，C 模块获得对应具名枚举，快照字段布局保持。
组状态绑定当前支持的输出身份与 workspace；workspace 切换保留各自模式，输出丢失或
进入尚不支持的输出配置时恢复 Normal，mode_revision 不倒退。显式组意图推进模式修订，
主题和尺寸变化只推进对应布局修订。

Normal 使用主题 Shell 保留带与外边距；Immersive 使用完整输出、外边距 0，保留主题
内间距、树成员和分割比例。只在最终 End 接受后修改模式，不随每个 Update 配置窗口。
恢复使用当前主题、输出与存活窗口重新安排，不保存旧矩形用于回滚。

原生单窗 XDG fullscreen 仍是独立覆盖状态。EnterImmersive 不改客户端 fullscreen
标志，退出单窗覆盖后采用当前组模式；显式 ExitImmersive 与键盘恢复清除当前组的单窗
覆盖。Shell 不可用时恢复所有组的 Normal 并清除覆盖，保留普通窗口与树结构。

### 9.2 控制提交与幂等

`LayoutControlAuthority` 完成身份、输入、目标和序号验证后，先结束跟踪会话，再同步
调用独立 `LayoutControlApplier` 接口。WM 应用组意图并填写操作后的权威修订，返回
`Ended/applied=true`；None 仍仅结束跟踪，BoundaryGesture/ApplyBoundary 继续 Unsupported。
本阶段新的组意图仅接受鼠标输入证明。

正常隐藏与撤权分开：隐藏通过 `CancelInstance` 撤销输入和活动会话，保留有界重放日志；
真正 unmap、端点撤权或进程退出再执行 Revoke。已完成 End 在焦点变化、重复 Arrange、
临时唤出和收起后仍返回缓存结果，不重复修改模式或修订。

### 9.3 Shell 与鼠标恢复

Topbar 通过既有 C ABI 订阅布局，复制当前 supported output、active workspace、root
和修订。横线的鼠标下拖通过通用 DSL gesture 进入 Host 桥接；模块只在 End 选择意图。
实际强调色来自权威快照，成功提交请求或动画本身都不能代替 WM 已应用状态。

恢复入口采用第 4 节的中央顶边鼠标约定。WM 在 Down 决定输入归属，完整消费系统拥有
的按钮序列，应用已有按键或抓取不被接管；不把系统 Up 交给下面的窗口。普通鼠标
序列按最初 surface 计算移动坐标并交付释放，最后释放后重新命中；跨窗口或透明区域
不转移所有权。主键设备移除先取消客户端输入，再解除 seat 按键状态，避免虚假点击。
临时 Topbar
使用主题给定的 ShellRect，WM 限制输入范围并裁剪显示区域，普通窗口不因唤出反复 resize。
拖动释放并立即移出时，有效的未消费 Down 或活动会话会暂缓收起，最多沿用原有的
2 秒释放宽限；End(None)、取消或宽限到期后解除保护。WM 事件循环检查收起条件，不要求
用户再移动一次鼠标，也不添加固定渲染循环。

`Super+Shift+F` 由 WM 直接恢复，不依赖 Shell 回调；`Super+F` 保持单窗语义。Shell
不可用时鼠标边缘路径也可直接恢复。launcher 对故障 Shell 撤权、清理订阅和 worker，
保持其他进程运行，无自动重启；详细规则见 [会话运行规范第 6 节](SESSION_LAUNCH_RUNTIME.md#6-会话停止与故障)。

### 9.4 文件与验证

WM 模式、恢复输入分别位于 `wlr_server_group.cpp`、`wlr_server_group_input.cpp`；
Topbar 包保留独立 DSL 与业务模块。新增 `native_group_mode_test` 和
`topbar_group_control_test`，真实会话及故障 probe 均位于 `tests/`，不安装到生产包。

Pi 完整构建通过，CTest **72/72**、代码规范和 `git diff --check` 通过。生产文件最大
627 行，测试与 probe 不参与生产行数上限，也不进入运行包。

- 原生 fixture 验证两个窗口的拓扑和非等比分割、组模式与 XDG 覆盖交错、主题变化、
  输出缩放/重建、workspace 切换，以及键盘恢复。唤出/空闲不反复 configure 普通窗。
- 鼠标原生验证覆盖边缘按钮不泄漏、已有应用 Down 跨边缘后的移动/释放归属、临时
  控件显示与收起、延迟 Begin/End、End(None) 解除保护、隐藏后的 End 精确重放，
  以及 Shell unmap/revoke 后恢复安全布局。虚拟设备输入不代表实体触屏验收。
- Topbar 模块测试验证方向/距离阈值、快照驱动模式、快速释放早于 Begin ACK、拒绝、
  取消与断连。普通应用不可借用此系统控制能力。
- 隔离 V3D 完整会话使用实际打包的 DSL、主题、Topbar/Host/launcher/WM 和两个 demo。
  手柄位置由 DSL 布局计算为 `(640, 7)`，两轮鼠标进入、边缘唤出和恢复均成功。
  1280 × 720 输出上两个应用由各 620 × 540、y=66 扩展为各 634 × 720、y=0，保留
  12 像素内间距；实例/PID 不变，恢复后的 target 与 committed 几何完全回到基线。
- 真实会话依次 SIGKILL Topbar、Dock、Desktop，WM/launcher 与原应用保持运行；
  原实例可激活、新应用可启动和取消，无自动 Shell 重启，最后会话与进程清理通过。
- 独立故障 worker probe 验证三个 Shell 主动报告 Failed 后阻塞清理仍被回收；迟到的
  Registered/Mapped/Unmapped 不复活旧实例、不终止会话，后续普通启动仍可处理。

初轮结果保留：首次构建被终止后继续构建；两个枚举的 `None` 名称冲突已改为显式
限定意图枚举。首轮 CTest 有两项失败，分别暴露鼠标跨窗口后未保持原 surface 归属、
真实 unmap 与临时隐藏的取消原因混用，均修复后全量复测通过。完整会话 probe 首次
使用错误的 `prism-msg` 构建路径，修正测试路径后两轮实测通过；没有放宽行为断言。

本阶段证据位于 `dist/validation/group-immersive-v1-20261001/`：`build-verified.log`、
`ctest-verified.log`、`style-verified.log`、`group-session-final/group-session.json` 与
对应 session/pointer 日志、`shell-recovery.log`、`shell-worker-failure.log`。
本轮没有部署或替换正在使用的
桌面/VNC 会话，也没有重新测量帧率或新增触屏操作。

## 10. 第四步实施细则：鼠标分隔线与尺寸约束

本步新增可信 `LayoutControls` Shell 角色，仍由统一 app-host 承载 DSL 与业务模块。
WM 不链接客户端 Scene/Skia，也不绘制手柄；它只选择边界、设置控制面的几何和
允许输入区域。首版使用一个 48×48 的小控制面，鼠标接近边界中点时呈现对应的横线或
竖线，拖动期间保持该边界身份。首版不引入重复列表 DSL、触屏手势或装饰动画。

实施与验收顺序：

1. Window 记录 XDG committed min_width/min_height；树层递归汇总子树最小尺寸，
   split 主轴相加（含 gap）、交叉轴取最大，tabbed/stacked 加入已有标题区域。
   当前容器无法同时满足约束时禁止拖动，不通过负尺寸挤压窗口。普通 Arrange 的
   空间不足策略保持独立，本步不承诺所有窗口初始分配已经满足 min size。
2. 通过稳定 BoundaryId 计算可移动区间，位置夹取；只调整相邻 pair，保持 pair
   总比例和其他 siblings。取消只恢复同一 topology 下仍匹配最后预览指纹的 pair。
3. Begin 绑定真实鼠标 Down、所选 boundary、原始比例和尺寸约束。WM 记录 Down、
   motion 与 Up 的输出逻辑坐标，布局计算使用 WM 的位移记录；客户端局部坐标不
   能在控制面移动或消息延迟后被误当作全局坐标。Update 产生受约束的临时布局，
   End ApplyBoundary 确认最终比例，Cancel/断线/失效回收预览。
4. 会话记录自身更新后的 layout revision。自己的 preview 不使自己失效；外部
   拓扑、主题、workspace、输出或 committed min size 改变时取消。提交几何更新
   仍和 configure target 分开，不能把请求被接受写成客户端已经显示。
5. 每个 XDG 客户端最多保留一个未 commit 的尺寸 configure，期间只覆盖最新目标；
   收到对应 commit 后继续提交最新目标，避免慢客户端积压 resize 消息。
6. Snapshot wire v2 尾部增加所选控制面，解码兼容 v1；C ABI 用 struct_size
   可选尾指针扩展。WM、launcher 和 Host 成套更新。Topbar 仅控制组模式，
   LayoutControls 仅控制边界；普通应用与 Dock 无布局修改权限。

验证覆盖树约束与夹取、相邻 pair 和比例恢复、角色隔离、会话自身修订与外部失效、
真实原生输入和慢客户端提交。集成测试在隔离 headless Wayland 会话运行，保持
当前安装的桌面/VNC 服务不变。

## 11. 第四步交付与验证（2026-10-04）

### 11.1 已实现的行为

- `tree_constraints.cpp` 递归汇总 Window 最小尺寸和容器标题/gap，计算父局部轴坐标
  下的边界范围。ApplyBoundary 使用与真实 Arrange 一致的取整规则，只修改相邻 pair，
  保持总比例、pair 外边缘及无关 sibling 几何。恢复时核对 topology 和最后预览比例。
- 普通 XDG Window 采用 committed min_width/min_height。约束改变推进独立约束
  generation，从而推进 WM layout revision，即使当时目标几何没有变化也会使旧请求失效。
- role 4 `prism_layout_controls` 使用标准 Host、业务 C ABI 和 DSL；一个 48×48 控制面
  根据 WM 选中的边界轴展示 4×32 或 32×4 的 pill，颜色来自主题。鼠标接近边界中点
  24 像素范围时选择，实际输入和画面裁剪到分隔缝。小于 1 像素的 gap 不提供手柄，
  首版不扩大点击区域覆盖应用，不支持在无 gap 的界面中直接拖动。
- 操作使用实际 Pointer Down serial、surface/实例和选中 BoundaryId。WM 记录真实
  Down/motion/Up 位置，按初始分隔线位置加位移求值；任意客户端局部坐标不能移动窗口。
  Update 使用自己的新 layout revision 继续追踪，重复请求不再次应用；End 确认，
  Cancel、设备移除、Shell 退出、外部几何或 topology 变化撤销。
- XDG resize 只保留一个未 commit 的尺寸 configure 与最新目标。ack 本身不释放
  配额；客户端提交该 serial 或后续 serial 后才发送最新目标。夹取后比例没有改变时
  不重复 Arrange。快照始终分别报告目标和已提交几何，不声称已经显示最新目标。
- Tabbed/Stacked 的非 active 分支不显示、不发布可见分隔线。
- 初始隐藏控制面使用可信 WorkerBind 的 deferred_presentation 策略，保证提交与
  BackendReady 超时检查仍有效；真实 FirstPresented 等它可见后再报告。

### 11.2 验证范围

- 全量 CTest **75/75** 通过；代码规范检查通过，327 个生产文件、100 个测试文件，
  最大生产文件仍为 627 行。
- 树测试覆盖水平/垂直、多兄弟、嵌套 Tabbed/Stacked、极小区域、大 gap、NaN/Inf、
  分数尺寸、动态 min size、外部比例变更和 topology 防陈旧恢复。
- 原生 Wayland 测试覆盖真实鼠标 serial 和隔离进程、输入区域裁剪、实时预览、确认/
  取消、设备丢失、邻居退出、主题变化、真实 XDG 动态最小尺寸、角色权限隔离。
  SIGSTOP 慢客户端期间连续 20 次拖动更新，恢复后最多收到两次尺寸 configure，
  最后目标正常 commit；不以响应时间证明帧率或性能提升。
- 真实 Pi V3D 隔离会话使用正式包、Host、launcher、WM，两次组沉浸/恢复，以及
  水平分隔线 +96/-96 像素往返，验证相同实例/PID、目标与 committed 一致。
  初始隐藏 16 秒跨过 Host/launcher 启动 watchdog 后仍可激活。
- 协议 routing probe 验证 Topbar/Controls 的 operation 隔离及普通应用/Dock 拒绝；
  四 Shell 自报失败 probe 验证撤权、限时清理、迟到 WM 回复和继续创建普通应用；
  正式会话依次 SIGKILL 四个 Shell，WM/launcher 和原应用继续运行、普通应用可继续启动，
  没有自动重启 Shell，退出后所有测试进程回收。

证据保存在 `dist/validation/boundary-control-v1-20261003/`。初次完整会话暴露隐藏
控制面 Master 呈现超时，已通过显式按需呈现策略修复；失败日志保留，不能作为通过
记录。本轮未打包安装，现有 VNC 桌面保持原版本。

### 11.3 仍保留的边界与下一阶段

本步不把普通 Arrange 改成全局约束求解器：窗口初次分配的区域可能小于客户端声明
的 min size；整个父区域不足时禁止边界拖动，后续需要单独定义 overflow 策略。
当前只支持单输出活动 workspace，触屏设计仍延期；真实完整包往返测试为水平边界，
垂直和多层嵌套由纯树算法测试覆盖，尚无多显示器或实体屏幕呈现验收。

下一阶段可接入鼠标控制区的窗口级操作（单窗全屏/恢复、布局方向与分割调整），继续
复用同一权威目标/结果接口；之后再接动画呈现，不把动画插值掺入树比例提交路径。

## 12. 第五步：窗口级鼠标控制（源码交付，2026-10-04）

鼠标 `Super + 右键` 在一个可见普通窗口上打开图标控制面板，目标固定为当时的稳定
ViewNodeId，不在点击时重新取焦点。面板提供单窗全屏/恢复、父 split 容器横向排列、
纵向排列；“横向/纵向”明确修改所选窗口的直接父容器，影响该容器所有兄弟，不创建
新窗口或悄悄重挂树。Tabbed/Stacked 亦可显式转为 split。只有一个子节点时禁用方向
操作；全屏期间禁用方向操作，先恢复。此阶段不加入关闭应用等不可逆操作。

仍复用唯一 LayoutControls Host surface。普通左键拖分隔线行为不变；面板是显式
打开的临时控件，可以覆盖其有限矩形内的应用区域，点击外部或 Escape 关闭。打开
手势及关闭点击的整条鼠标序列由 WM 消费，避免右键或释放泄漏到被覆盖的应用。

协议新增 WindowGesture 和独立 target.node，禁止把 BoundaryId 当作窗口 ID。
Begin 绑定真实 Down、面板目标和初始 topology/layout；End 在有效鼠标释放之后执行
显式 EnterWindowFullscreen/ExitWindowFullscreen/SplitHorizontal/SplitVertical，
不使用非幂等 Toggle。自身动作导致的会话终止必须先完成，再关闭面板并安排窗口。
输出、workspace、目标卸载、外部 layout/topology 变化或输入设备丢失使面板和会话失效。

DSL 的 `.gesture(threshold: 0)` 定义为按下立即开始捕获，可用于需要真实输入凭据的
离散按钮；模块只有在同一个按钮内释放才选择最终 intent，移出释放为 Cancel。
默认非零 threshold 拖动行为保持原义。普通应用不会因使用该语法获得 WM 权限。

请求 wire v2 在 v1 的末尾增加 node u64，仅 WindowGesture 编码为 v2；旧操作仍用
v1。Snapshot v3 在 v2 handle 后追加 node u64，兼容读取 v1/v2。模块 C ABI 新字段
只放在 struct_size 可检查的尾部，既有结构前缀不变。测试覆盖兼容、权限、目标陈旧、
取消、重复 End、比例/拓扑保留及真实 DSL 完整链路；不替换正在使用的桌面服务。

### 12.1 交付与验证

- 新增 `wlr_server_window_control.cpp` 独立管理面板目标、显示、输入验证和意图应用；
  WM 不链接 DSL/Scene，不绘制按钮。LayoutControls 业务模块只订阅快照、选择 intent、
  发布 binding，前端仍由统一 Host 管理。
- 复用同一个 control surface，窗口面板与分隔线横线互斥。面板随主题/明暗变化，
  提供通用向量图标；不可用方向项保留低透明度图标，隐藏 gesture target。固定外层
  InteractionTarget 拦截空隙及不可用项，不把点击交给下面的应用。
- Begin 绑定稳定 ViewNodeId 和真实 Down proof。End 前必须有实际鼠标 Up；跨按钮
  释放由模块 Cancel，面板外释放由 WM 再次拒绝。身份、会话、workspace、输出和
  修订仍由权威控制接口验证；重复 End 从日志返回，不再次改树。
- 新快照编码为 v3，可读取 v1/v2/v3；窗口请求编码为 v2，旧组/边界请求维持 v1。
  模块 optional tail 不改变已有前缀。新增零阈值即时手势用例覆盖无移动点击和同批
  Begin/End；新增图标加入 Skia 损伤包围盒的全图标覆盖。

构建与代码风格检查通过，完整 CTest **76/76**，最大生产 C/C++ 文件 **627 行**。
新增 `native_window_control_test` 使用真实 Wayland surface/输入 serial 验证全屏恢复、
方向切换、比例及节点身份保留、重复 End、提前 End 拒绝、面板外释放拒绝、角色隔离、
关闭点击不透传、Escape、设备移除、workspace 切换和目标卸载。

真实 Pi **V3D 4.2.14.0** 的独立完整会话也通过：四个 Shell + Music/Preferences，
LayoutControls 隐藏等待 16 秒，两轮 Topbar 组沉浸/恢复、分隔线 +96/-96 往返，随后
使用虚拟鼠标/键盘打开真实包内 DSL 面板并依次执行全屏、恢复、纵向、横向排列。
几何验收等待 target 与 client committed 一致；所有测试进程回收，session exit=0。

证据：`dist/validation/window-control-v1-20261004/`。`ctest.log`、`style.log`、
`build.log` 为最终门槛；`session-verified/group-session.json` 中 `passed`、`reclaimed`、
`boundary_passed`、`window_passed` 均为 true。初次 Desktop 模块触发既有 cooperative
load entry budget 的记录，以及定位 probe 未先 flush 打开命令的 Wayland trace，均保留。
probe 已将 Roundtrip 放在等待面板提交之前，不修改生产入口预算或加入生产测试延时。

本阶段未替换已安装会话、未打 deb，未做触屏、多输出或实体显示器视觉验收。当前固定
168×56 的面板及三个 48×40 按钮是私有 Shell 布局约定；后续改尺寸须同步调整模块的
释放区域并通过实际 DSL 点击区测试，普通应用不依赖该约定。

下一阶段可基于已确认的组/窗口目标和快照，接入呈现过渡与打断/反向恢复；动画只消费
已确认状态，不以每帧 intent 重写 BSP 比例，也不把客户端动画值作为 WM 几何权威。

## 13. 第六步：独立 Motion 与控制面板开合（2026-10-04）

规范见 [Motion 文件与呈现适配](MOTION_PRESENTATION_SPEC.md)。先建立 MotionSet →
ThemeSnapshot → 所有者适配层的通路：主题 schema 3 引用独立 motion.prism，Scene
支持命名 Transition；WM 只消费 typed 时间曲线，不读取客户端 DSL。

本阶段接入窗口控制面板透明度开合与控制图标/分隔线的背景色反馈。关闭立即撤销输入，
退出画面通过有界 buffer 引用淡出；同位置重开可反向，主题替换和对象消失负责清理。
提供无平台依赖的 GeometryTimeline，覆盖时间采样、矩形重定向及已提交几何的逆映射。

下一步是单窗全屏/恢复的 native buffer、装饰、效果与鼠标映射一致提交，再进入 BSP
共同边界协调。当前全屏仍是既有的立即布局操作；不通过动画逐帧修改 BSP/resize。

本阶段验证：完整构建、CTest **78/78**、Pixman 和 V3D/GLES 原生窗口控制通过；
真实 Host/launcher 隔离会话的组模式、分隔线、窗口控制与进程回收通过。没有活动
轨迹时的实际帧提交停止增长已验证。证据为 `dist/validation/motion-v1-20261004/`。
当前交付范围与退出玻璃冻结、逐层透明度、C0 连续等限制见 Motion 规范；未替换已安装会话。

## 14. 第七步：单窗全屏/恢复几何呈现（2026-10-04）

SurfaceGeometry 消费 `window.geometry`，布局一次确定目标，输出帧按单调时间映射
当前客户端 buffer。新 buffer 可沿同一轨迹交接；装饰和玻璃区域使用呈现矩形。
鼠标命中及隐式抓取使用最后成功提交的逆映射，途中反向从该矩形继续。
终点等待慢客户端时停止持续重绘；生命周期或主题变更撤销临时节点参数。

首版为单输出单个过渡，效果 buffer 尺寸缓存、触屏及多输出协调尚未实现。
下一步进入组级共同时间与共享边界，避免相邻窗口独立插值产生缝隙。

本阶段完整构建及 CTest **79/79** 通过；V3D/GLES 原生输入与真实 Host/launcher 隔离
会话验证通过，代码规范检查通过。证据为
`dist/validation/fullscreen-motion-v1-20261004/`，详细范围见 Motion 规范第 8 节。
未部署新 deb，也未把上述正确性测试当作动画性能或实体显示器视觉验收。

## 15. 第八步 A：组几何与共享边界核心（2026-10-04）

已实现无平台依赖的 GroupGeometryTimeline：稳定边界引用、固定间距、单时间样本、
共享边一次取整、整组候选及成功提交确认。从最后提交帧反向，拒绝过期候选和拓扑
变化，终点在提交成功后停止帧需求。详细契约与限制见 Motion 规范第 9 节。

本步没有接入真实组动画。下一步 B 将 BSP 快照转换为边界图，并扩展原生呈现适配器
同时管理多个 surface；输出成功后统一更新输入基线。拓扑改变先使用明确的立即布局
降级，不在第一版猜测跨拓扑动画。单窗路径继续保持现有行为。

验证：相关构建、动画核心测试 **3/3**、代码规范检查及 diff 空白检查通过。证据为
`dist/validation/group-geometry-core-20261004/`。本步未重新打包或部署。

## 16. 第八步 B：原生组沉浸往返（2026-10-04）

接入 BSP 快照转换与 GroupSurfaceGeometry，普通分割布局/组沉浸共用一份候选帧，
原生 buffer、效果和鼠标提交基线共同更新。Motion 包新增可选 `group.geometry`；
缺少接口及不支持的拓扑使用立即布局。分隔线拖动仍保持直接跟手。

首版范围、固定偏移不兼容的降级、焦点切换收束与 Shell 立即显隐见 Motion 规范第 10 节。
后续优先在这些真实路径上评估帧成本，再扩展 Shell 呈现与布局切换；不把组过渡核心
的数学保证当作实际 GPU 帧耗时或远程显示流畅度保证。

验证：完整构建、CTest **81/81**、V3D/GLES 原生组/单窗门槛、真实 Host/launcher
隔离会话及代码规范检查通过。证据为 `dist/validation/native-group-motion-20261004/`。
当前安装桌面未替换，Topbar/Dock 联动滑动及动画性能量化仍属于后续工作。
