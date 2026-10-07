# Session confirmation：关联契约与生产接入边界

阶段：Interface System 6b1，参考设计图 04 的「结束这次工作？」系统会话确认。

## 1. 当前交付与能力状态

本阶段建立 typed 内部契约和串行状态核心，供后续 launcher / WM 的真实系统入口使用。
`CurrentSessionConfirmationAvailability()` 始终返回 `Unavailable`。即使测试中的纯核心进入
`Ready`，该查询也保持 `Unavailable`。

本阶段没有接入系统会话模态，没有增加普通业务模块的 C ABI，没有创建系统确认窗口，
没有新增 poweroff、logout 或结束会话执行器。Topbar 现有电源图标也不会因为这份契约而
获得结束会话能力。状态核心的 `Ready` 只说明两类关联回执已齐备，不证明生产平台完成
了输入阻断、认证或焦点接管。

实现文件：

- `prism/include/prism/contracts/session_confirmation.hpp`
- `prism/contracts/session_confirmation.cpp`
- `prism/include/prism/runtime/session_confirmation.hpp`
- `prism/runtime/session_confirmation.cpp`
- `tests/session_confirmation_test.cpp`

## 2. 三种界面保持不同权限边界

| 界面 | 当前归属 | 权限与结果 |
| --- | --- | --- |
| Tooltip | owner 的只读提示 | 不抢焦点、不改变模态、不执行动作 |
| Owner feedback / owner task | 已注册应用自己的 Host 生命周期 | 非模态反馈或 owner 内任务；不拥有系统会话权限 |
| Session confirmation | 后续平台内部 coordinator 与 WM | 必须来自真实系统入口；普通应用不能请求创建系统会话模态 |

应用能绘制相似卡片，不意味着该卡片属于系统。普通模块不能通过主题名称、应用 ID、
DSL 节点名称、任意数字 token 或 `trusted=true` 提升权限；本接口没有这样的绕过字段。
`SessionConfirmationProof` 中的 `Proof` 指内部关联回执，不能当作认证凭据传递给普通应用。

## 3. 现有启动链能提供什么

仓库已经具备以下入口基础：

1. `prism/launcher_runtime/session.cpp` 为 WM 和 launcher 建立私有 socketpair，绑定父进程。
2. `prism/launch/control_protocol.cpp::VerifyControlPeer` 检查父子 PID、`SO_PEERCRED` UID/PID
   与 socket 类型。该控制端点不同于普通公开 launch 请求端点。
3. launcher 在 `BootstrapShell` 中创建平台 shell job，角色由 launcher 分配。
   公开 `Request` 不允许申请 shell package；`LayoutOwner` 根据实际当前 job、worker
   状态、已绑定实例和私有 worker channel 校验调用资格。
4. WM 只接受私有控制通道下发的注册许可；xdg surface 注册绑定实际 Wayland client
   UID/PID、instance 和存活的 pidfd，应用声明的 app ID 不授予 shell 角色。

后续 session-confirmation admission 必须沿用这种实际 job / 私有端点关联方式。不能在
公开 PRL 请求中加入一个自报的角色字段，再把它转发给本核心。真实入口可来自 WM 原生
系统手势，或由 launcher 从当前已分配的平台 Topbar worker 私有 channel 收到的系统
动作；资格来自私有通道和实际 job，而不是消息载荷中的名称或布尔值。

本阶段没有新增上述 admission 分支或相应 wire 消息。核心构造参数仅检查 session 数字
非零，不识别调用者、socket、PID 存活性或 job 资格。后续 adapter 必须在进入核心前完成
实际端点校验；把相等字段传给核心，无法替代这些校验。

## 4. 仍缺少的生产条件

当前 WM 没有系统会话确认专用 presenter / role，没有贯穿 desktop、窗口、shell 的完整
session input barrier，也没有系统确认键盘焦点接管与取消恢复路径。现有 owner-local
modal 只约束自己的 Host。`FocusXdgView` 也不能直接把现有 shell surface 当作普通可聚焦
会话确认窗口使用。

virtual keyboard / pointer global 当前按同 UID 过滤，并没有把同 UID 普通客户端与获准
的远程输入代理区分。后续系统确认不能把任意 virtual device 的事件或一个 Wayland
serial 直接视作可信用户确认；必须先完成实际输入来源和当前 barrier 关联策略。

Supervisor 当前负责进程信号、回收和整体退出，没有业务退出协商或安全终止执行接口。
因此本阶段不得把 `ConfirmedIntent` 接到现有进程 kill 路径，并声称已经完成安全结束
会话。显示一张卡片或在纯核心中接受两份回执，同样不能据此开放生产 capability。

## 5. Typed 数据与校验

所有数据都是拥有明确字段的内部结构；本阶段不规定 wire 编码，不接入 JSON 或业务
模块 ABI，不接受应用提供的任意标题、正文、动作来塑造系统权限界面。

| 类型 | 字段与约束 | 含义 |
| --- | --- | --- |
| `SessionConfirmationIdentity` | 非零 `session`、非零 `request` | 实际会话 lifetime 与该 lifetime 内单调 request |
| `SessionConfirmationPresenter` | 非零 `instance`、非零 `pid` | 后续 adapter 已验证的平台 presenter 关联值 |
| `SessionConfirmationScope` | 非零 `output`，`seat` 可为 0 | 目标输出与输入 seat 的关联，当前 compositor 使用 seat 0 |
| `SessionConfirmationProjection` | 非零 `input_epoch`、`ui_generation`、`frame_sequence` | 同一请求的输入屏障代际、语义 UI 代际与被采用帧 |
| `SessionConfirmationPlan` | `EndSession` intent、合法 presenter/scope、非零绝对 deadline | 内部 coordinator 选择的计划，无执行命令 |
| `SessionConfirmationProof` | 合法 identity/presenter/scope/projection | 已校验端点产生的关联回执；结构体自身不是认证凭据 |
| `SessionConfirmationDecisionProof` | 合法 proof，Cancel/Confirm，非零 sequence/serial | 当前关联投影上的一次用户意图记录 |

PID 数字合法不等于进程存活；serial 非零不等于用户事件已被认证。output/seat 字段也
不保证整个桌面已阻断。session-wide 的真实屏障范围、live surface 和输入来源，都必须
由后续平台 adapter 验证。本核心只比较关联值。

## 6. 串行状态协议

`SessionConfirmationSession` 在一个内部 owner 线程上使用；核心不创建线程、不调用业务
回调，也不接触 Scene 或 wlroots。它不可复制或移动，避免复制 request 分配状态。
真实 session lifetime 由后续 coordinator 分配，整个会话生命周期内不能复用；本核心
只负责该 lifetime 内 request 单调增长、非零、拒绝整数耗尽后的新请求。

执行顺序：

1. `Begin(plan, now_ns)` 创建 `Preparing`。plan 非法、已 busy、已退休、终态待消费、
   deadline 已过、时间倒退或 ID 耗尽，都拒绝且不创建新 request。
2. coordinator 准备实际平台 presenter、屏障和对应语义帧。`BindProjection` 对当前
   identity 只允许绑定一次。它只记录投影关联，不显示窗口、不安装输入屏障。
3. `AcknowledgeBarrier(proof, now_ns)` 与 `AdoptPresenterInput(proof, now_ns)` 记录两类
   回执，可以任意先后。两者都必须与 active identity、presenter、scope 和已绑定
   projection 完全相同；未绑定、错误 PID/instance、旧 epoch/frame/generation、重复
   回执均拒绝。只有两者都接受后，关联核心才进入 `Ready`。
4. `Decide(decision_proof, now_ns)` 只允许 `Ready` 的匹配请求。Cancel 产生
   `Cancelled/User`；Confirm 产生 `ConfirmedIntent`，并退休当前 active request。
5. `TakeTerminal()` 移除待消费结果，然后调用者才可处理结果。结果取走后才能开始新
   request；旧 request 的回执、取消和确认不能影响新 request。

这里的两类回执必须由后续 adapter 从不同实际平台进度点产生；普通客户端能拼出相同
字段，不意味着能合法调用两种方法。核心没有任意 `SetReady` 或可提交的 trusted 字段，
但它也不会认证调用这两个方法的进程。

任意 UI 语义投影变更都需要以 `ProjectionChanged` 取消当前请求，再分配新 request。
不允许绑定另一帧替换旧关联，这样旧按下/释放、旧确认或旧屏障回执不能被重新解释成
新界面的确认。只有外观动画而不改变语义投影的情形，后续集成需明确 adopted frame
规则后另行设计，不在本阶段放松关联要求。

## 7. 超时、失效与消费

`deadline_ns` 是内部 coordinator 的绝对单调时钟值，覆盖 `Preparing` 和 `Ready`，
不是动画时长，也不是关闭进程的等待预算。本核心没有默认超时，不读取系统时钟，不
创建 timer；平台必须从自己选择的单调时钟采样 `now_ns`，并驱动 `Expire(now_ns)`。
不得把应用提供的时间值转发为 coordinator 时间。

- 在 deadline 的前一刻仍可接受关联进度；`now_ns >= deadline_ns` 产生一次
  `Cancelled/Timeout`，不能在该时刻确认。
- 合法的绑定、回执或确认会检查 deadline；平台还需主动驱动 `Expire`，覆盖没有事件
  的等待期。无效或过期 request 的消息不作为当前请求的时间驱动入口。
- 核心记录已接受的时间观测值，拒绝倒退的时间；等待 timer 同样不能回退该水位。
  全部使用绝对值比较，不通过 `now + duration` 产生溢出风险。
- `Cancel(identity, reason)` 允许取消 Preparing 或 Ready；它是内部 coordinator 操作，
  不证明对应的 WM 清理或焦点恢复已经完成。
- presenter 不可用、projection 变化、端点断连或实际 session 退出时，内部 coordinator
  应取消 / 退休相应请求。`Retire` 永久拒绝新请求；默认原因为 `SessionRetired`。
- 一次 request 只接受第一个终态。退休不能覆盖已经接受的结果；结果保留是为了记录
  已接受的意图，不延长 session 权限或 presenter 生命周期。

`ConfirmedIntent` 明确表示「当前关联请求接受了确认意图」。它**不是终止授权，不是
业务退出安全判定，不表示保存完成，不表示会话已结束**。后续 executor 若要执行独立
的会话操作，必须重新验证活跃的内部 session / endpoint 和自己的操作策略；不能把
取出一个旧 receipt 当作存储后可重放的执行凭证。本阶段没有这样的 executor。

## 8. 后续真实 WM 接入与取消恢复

生产 capability 开放前需逐项完成并验证：

1. 私有系统请求入口和 launcher admission，根据实际 live job / channel 分配身份，
   普通 app launch/module 请求始终不具备系统会话创建接口。
2. 固定平台 presenter，明确 session / instance / PID / output / seat 生命周期绑定，
   presenter 内容与最终动作由平台定义。
3. WM 安装完整 session barrier，阻止普通窗口及 shell 的后台操作，确认 keyboard、
   pointer、现有 WM shortcuts 和允许的 remote input 路径在该阶段的行为。
4. 用真实已采用的 presenter 帧与输入语义投影形成 association proof；画面提交成功
   或业务模块 Ready 不能单独解除 Preparing。
5. 用户取消、Esc、超时、surface 消失和私有端点断连时，先清理真实 barrier / presenter，
   再按有效窗口 lifetime 恢复工作上下文。保存 window identity、workspace 和 scope，
   不跨异步边界保存裸 `WlrXdgView *`。原目标已退出、隐藏或不再属于当前 scope 时，
   使用当前工作区的有效焦点候选。
6. 系统执行器单独定义业务退出协商、拒绝与取消，以及 supervisor 收尾结果；不让
   UI receipt 承担这些尚未实现的语义。

本阶段只为这些步骤提供严格的关联核心。测试不能替代真实 WM barrier、焦点恢复、
私有通道 admission 和用户输入来源的集成验证。

## 9. 测试范围

`session_confirmation_test` 覆盖合法/非法 typed 字段、始终 Unavailable、未绑定和未
Ready 拒绝确认、两份匹配回执的顺序、重复回执、错误 PID/instance/output/seat、旧
epoch/frame/generation/session/request、UI 更换、新请求隔离、所有阶段取消、终态
单次消费与同步重入、退休、ID 耗尽、deadline 边界和单调时间回退。

这些是纯核心测试，不创建 Wayland 客户端，不产生原生输入，不终止进程，也不宣称已经
验证桌面模态或安全结束会话。
