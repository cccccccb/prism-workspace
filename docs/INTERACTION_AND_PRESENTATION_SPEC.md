# 通用交互状态、呈现属性与桌面控制区

日期：2026-09-29。状态：**第一阶段输入状态已经验证；第二阶段固定交互目标、状态规则与
装饰子树呈现已完成源码与 Pi 隔离验证；第三阶段布局输入快照与 touch 已接通，验收见第 14 节**。
当前接口见第 12 至 14 节；交互节点整体变换、手势识别、组控制与保留层继续后续实施。
时间语义见 [动画运行时规范](ANIMATION_RUNTIME_SPEC.md)。本文同时保留产品设计，标明
未来接口与当前源码的边界；源码修改不会自动更新已安装的 Host 或 VNC 会话。

## 1. 产品目标与当前事实

Prism 使用类 i3 的平铺容器树组织窗口，以简洁的图标、玻璃材料和短横线表达控制层次。
横线可以是运行指示，也可以是控制入口；二者共享视觉语言，但交互语义分别声明。
Topbar 的控制横线面向整个平铺组，窗口之间的控制横线面向分割边界或明确选中的窗口。
通用状态与呈现接口先完成，组全屏、窗口控制和复杂手势按后续阶段接入。

下表保留输入改造前的差异，第一、二阶段完成范围分别见第 12、13 节。

| 当前实现 | 对设计的影响 |
| --- | --- |
| Dock 运行指示是静态 `Progress(value: 1)`，Topbar 横线是嵌套 `Card` | 现有图元不是手势控件；不能只加动画就声称完成控制区 |
| `Scene::SetPointer` 内部维护 hover，按 action 字符串再次寻找节点 | 多个节点共用 action 时存在身份歧义，先改为带 NodeId 的命中结果 |
| SDK 在主按钮按下时触发 action，释放未参与动作裁决 | 手势识别前要建立按下、移动、释放、取消与捕获的统一生命周期 |
| Scene 没有 DSL 状态规则与 transform/opacity 属性 | 需要准备期编译、运行期求值和呈现数据传递，不能在 Shell 模块里逐帧计算样式 |
| DisplayList 已有 PushTransform/PopTransform，没有子树整体 opacity 命令 | 已有后端图元不等于 Scene 属性已接通；整体透明度要补正确合成语义 |
| 原生全屏只针对一个 XDG view，隐藏其他普通窗口和 Topbar/Dock | 新的组全屏必须独立建模，保留组内全部平铺窗口 |
| TreeEngine 可有同方向多个子节点，原生路径还没有分隔线拖拽控制 | 一个容器可有多个边界；边界身份不能只用容器 ID |
| WM 与 SDK 尚无完整 touch 事件链 | 鼠标测试不能代替平板触摸验收 |

依据：[Scene](../prism/runtime/scene.cpp)、[SDK 输入处理](../prism/runtime/client_application_render.cpp)、
[输入契约](../prism/include/prism/contracts/events.hpp)、[DisplayList](../prism/include/prism/contracts/display_list.hpp)、
[原生窗口安排](../prism/src/wm/wlr_server_views.cpp)、[容器树](../prism/src/tree/tree_container.cpp)。

## 2. 控制范围与系统所有权

### 2.1 三种操作不能共用一个 fullscreen 布尔值

| 操作范围 | 目标行为 | 状态所有者 |
| --- | --- | --- |
| 平铺组沉浸模式，用户所称“组全屏” | 隐藏 Topbar/Dock，整个组扩展到输出可用范围，保留所有成员、拓扑和比例 | WM 的输出/工作区状态 |
| 单窗突出显示 | 临时突出选中的窗口，保留它在树中的位置和兄弟关系，退出后恢复平铺 | WM 的窗口/容器视图模式 |
| 应用 XDG fullscreen | 遵循 Wayland 客户端请求及现有单窗全屏语义 | WM 与对应 XDG view |

首版“组”以当前输出上的活动 workspace 平铺根为作用域；以后可扩展到选中子树。
请求从一开始就携带输出、workspace 与目标身份，不以“现在的第一个窗口”作为隐含目标。
当前多输出工作区策略尚未实现，以上作用域字段是设计约束，不是多屏完成声明。

组沉浸模式不把每个客户端都设为 XDG fullscreen。退出时按当前输出尺寸、当前主题和
存活窗口重新安排，避免恢复陈旧矩形。单窗突出显示建议默认占满组的工作区域；需要占满
输出时使用明确的单窗全屏操作。组模式与单窗模式分别保存；组状态修订未变时，退出单窗
模式回到进入它之前的组模式。期间若有更新的组模式操作，退出服从最新有效状态，不用旧
快照覆盖新操作。恢复桌面组可以明确清除覆盖其上的单窗模式。最终模式变更由 WM 确认。

### 2.2 视图与系统操作解耦

- 通用前端负责命中、局部交互状态、状态规则、呈现属性、动画和语义动作。
- 普通业务模块提供业务状态与动作处理；hover/pressed 不通过业务模块往返。
- 受信任 Shell 负责组/分隔线控制 UI，将语义动作转换为带类型的系统请求。
- WM 负责平铺树、焦点、工作区、分割比例、约束、全屏模式、系统级输入仲裁与控制权限。
- 渲染线程只消费不可变呈现数据，不解释 action 名称或决定窗口操作。

Topbar 控制内容继续由统一 Host/DSL 渲染。未来分隔线控制 UI 建议由受信任的布局控制
Host 承载：WM 发布边界描述并限定 surface 位置与输入范围，前端按通用 DSL 绘制。
这一角色及协议需要单独接入当前 Shell 授权链；不能把应用 Scene 或组件绘制重新放进 WM。

### 2.3 隐藏控制栏之前先保证恢复路径

隐藏的 Topbar 无法继续接受输入。组沉浸模式必须同时具备：

1. WM 保留的窄范围边缘唤出入口，唤出受信任 Shell 的恢复控制视图。
2. 可直接恢复组模式的键盘命令；当前 Super+F 仅控制单个 XDG view，不能算作已完成。
3. Shell 退出/崩溃时仍可恢复的 WM 路径，以及输出改变或工作区切换时的清理规则。

2026-10-01 用户确认：主体功能优先采用鼠标，触屏交互设计与验收延期；下列触摸约束
保留为未来设计边界，不作为本轮交付门槛。当前鼠标操作与键盘恢复约定见
[布局控制计划](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md#第三步鼠标操作约定2026-10-01)。

唤出区域与应用输入的归属在接触开始时决定。WM 保留的边缘起始区域进入系统手势，已交给
客户端的触摸序列保持客户端归属，除非协议明确允许带 cancel 的接管；不得中途重放一次
点击给下面的应用。不能用全屏透明 surface 长期吞掉应用输入。具体边缘尺寸、方向与
手势阈值在控制功能阶段校准，首期不把它们写成动画核心常量。

## 3. 一个控制区由三部分组成

```text
InteractionTarget：稳定身份、可访问语义、感应范围、允许的手势
    └─ Visual subtree：横线、底板、焦点提示、有限阴影
        └─ Presentation values：当前颜色、局部变换、透明度
```

短横线的可见高度不决定可操作面积。鼠标和触摸可以使用不同的感应范围；建议触摸起点
按约 40–48 逻辑像素的短边校准，实际范围由布局与主题共同约束，不能覆盖邻近应用控件。
不够宽的分割缝应由 WM 明确划出控制范围或唤出控制浮层，不让客户端跨 surface 偷占输入。
这些数值是初始设计参数，逻辑像素与设备物理尺寸仍需实机校准。

首批横线运动发生在固定 InteractionTarget 内的装饰子树；装饰子树不接受输入，横线
缩放或变淡不改变手势归属。通用交互节点将来移动时使用视觉变换对应的逆变换命中，不能
把“固定点击区”作为所有控件的全局规则。

## 4. 通用状态模型

状态是可同时存在的带类型值，不把所有状态压成唯一的 Normal/Hover/Pressed 枚举。

| 状态 | 来源与语义 |
| --- | --- |
| hovered | 具备悬停能力的设备命中该目标；触摸不伪造持久 hover |
| pressed | 有尚未结束的有效按下序列，以 seat/contact 身份跟踪 |
| focused / focusVisible | 前端焦点及是否应显示键盘导航提示；与桌面活动窗口分别处理 |
| captured / dragging | 输入已归属目标 / 拖动识别已成立，两者不等价 |
| enabled | 交互是否允许；禁用立即拒绝新动作并取消现有交互 |
| selected / checked / busy | 来自 typed 业务或控制模型，不由动画推测 |
| windowActive / groupMode | 来自平台权威快照；不拿最近一次 launch Activated 回复代替全局焦点 |

输入先产出 `HitResult`，包含 UI load、节点身份与代数、当前命中快照版本。
action 字符串只用于分派动作，绝不承担节点身份。同一个 action 可以出现在多个位置，
例如 Dock 的固定段与运行段；它们必须有各自的 hover、pressed 与捕获状态。

父控件向其视觉子树暴露只读状态作用域；子节点引用所属控件的状态，无须把状态通过
业务 binding 广播。跨组件引用只允许准备期可解析的明确作用域，不能靠运行期遍历
同名节点找到“最近的按钮”。动态安装区域沿用 NodeId/UiLoadId 的代数与卸载规则。

## 5. 输入序列、捕获与取消

通用输入适配器区分鼠标指针、触摸接触和其他设备，定义 Enter/Leave/Down/Move/Up/Cancel，
携带 seat、pointer/contact identity、按钮、坐标、序号与时间信息。平台原始时间若不是
已确认的 CLOCK_MONOTONIC 域，必须保留其来源并明确换算；动画求值仍用本地单调时钟。
当前用 `(-1,-1)` 表示 leave 的做法在支持溢出与扩展命中后不再可靠，改用明确事件。

默认控件动作在释放时裁决。按下只建立 pressed 状态与待定识别；拖拽、长按等候选成立后，
取消同一序列的点击候选，防止拖动过程中已经误触发全屏。拖出后释放不执行普通点击；
返回范围内释放是否执行，按控件手势策略明确规定。一次序列最多产生一次相应结束动作。

这是对当前按下即执行 action 的明确行为迁移，首个实现阶段应更新现有按钮、Dock、键盘
激活测试。业务收到的具名 action 可以保持，不同时保留两个互相竞争的分派路径。
确需按下触发的控件以后显式声明激活策略，不能在应用名分支里例外处理。

捕获针对稳定目标身份。前端捕获只在 surface 已获得的输入流内生效，不能冒充跨应用
全局抓取；系统分隔线拖动的捕获由 WM 管理。目标卸载/隐藏/禁用、surface 销毁、设备移除、
触摸 cancel、失去必要焦点、工作区或控制权限变化都必须收敛到取消，清除 pressed/dragging。
鼠标正常离开目标不自动撤销已成立的拖动捕获。多点触摸不能共享一个全局 pressed 布尔值。

键盘 Enter/Space 遵循同一激活语义并抑制 repeat 的重复提交；拖动调整提供方向键替代，
Escape 取消当前控制会话。无 hover 的触摸设备通过接触与捕获反馈确认操作。

## 6. 状态规则、属性来源与主题

准备期把状态声明编译为 `StateRule`，包含状态谓词、属性赋值与保留的主题引用；优先级由
公共状态契约定义，首期不接受用户数字 priority。运行期只做状态求值，不重解析 DSL 或执行
任意脚本。下列仍是设计意图示意；可复制的第二阶段语法见第 13 节：

```text
目标：一个固定感应区中的横线视觉子树
基础：主题颜色与基础长度
hovered：中心 scaleX = 1.15
pressed：中心 scaleX = 0.94
dragging：中心 scaleX = 1.20
disabled：交互关闭，使用主题的不可用样式
```

基础值继续按现有主题/显式属性/binding 规则解析。局部状态覆盖只计算目标值，不调用
公开 SetProperty 抹掉 theme_refs 或作者值；状态结束后重新解析仍在生效的基础来源。
同一轮输入与状态更新完成后统一解析属性目标，再让 Transition 从当前呈现值重定向。

第二阶段交互通道的优先级为 disabled → pressed → captured → hovered → base，
只针对写入的同一属性竞争；尚无 dragging 状态或手势。焦点提示使用独立属性：focused 或
focusVisible 的目标属性如果也由其他条件写入，准备期保守拒绝；可将焦点颜色与悬停缩放
分别声明。相同条件对同一属性重复声明也拒绝，不用源码排列顺序决定结果。
selected/active 等业务或系统状态的组合接口仍待扩展，不能由 hover 推测。

状态规则首期只允许经过 schema 审核的呈现属性，不允许用 hover 每帧重写 action、
节点身份、资源或 BSP 几何。手势连续值走明确的交互控制通道，避免与 Transition 同时
写同一属性：拖动时跟手，释放后才由动画接管收敛；取消按控制模型确认的值恢复。

主题后续增加带类型的 MotionSpec 引用，定义反馈时长、缓动、幅度；平台通过带类型的
MotionPolicy 决定减弱动效等用户策略，主题参数服从该策略，不能反向重新启用被禁用的运动。
当前 `.transition` 仍只接受字面量 durationMs/easing，不提前宣称支持 motion token。
不同材质和明暗配色共享交互语义，可以替换横线形状、颜色与动效参数。主题不能改变
系统权限、把运行指示改成未经声明的全屏动作，或禁用必要的键盘/焦点可见反馈。

## 7. 通用呈现接口

第二阶段 `Visual` 提供逻辑坐标下的二维平移、正比例缩放、归一化变换原点和 opacity。
属性名称、范围、有限数检查与真实失效成本归同一 PropertySpec schema；完整范围见
第 13 节。以下包含已实现的装饰语义与未来交互变换必须满足的边界。

- 布局盒用于测量和排列；呈现变换不改变兄弟布局、不修改业务宽高。
- 变换组合顺序固定并测试：父变换 × 平移 × 原点平移 × 缩放 × 原点逆平移。
  节点本地裁剪随其变换，祖先裁剪继续约束最终范围。
- 未来交互节点整体移动时使用相同变换的逆矩阵命中；当前只移动不接受输入的装饰子树，
  `InteractionTarget` 的布局盒保持固定，不开放其 transform/opacity 属性。
- 装饰子树显式不参与命中。其他节点不能因 opacity 为零自动改变输入行为；visible 与
  enabled 负责相应生命周期，退出动画阶段需显式关闭交互。
- 子树 opacity 是整组离屏合成后的透明度，不能简单降低每个绘制命令的 alpha，造成
  重叠子节点混合错误。当前 `Visual.opacity` 使用整组图层语义；它不是持久 GPU 缓存。
- 首期只开放客户端本地视觉子树。含 backdrop 区域的子树、窗口根轮廓或跨 surface 阴影
  要在效果与输入同代协议完成后开放变换，不能只移动内容留下原地玻璃。

变换开始时可以仍走 Paint 与现有 Skia 回放，并准确记录该成本。DisplayList 的已有
PushTransform 并不提供保留层。只有保留资源、失效和 GPU 生命周期全部接通后，稳定
子树的运动才可以跳过子树重复绘制。damage 以最后成功提交和当前呈现范围的并集计算，
包括裁剪、边线、阴影外扩、缩放和 buffer age 修复；不能只损伤横线的新位置。

## 8. 呈现快照与输入一致性

UI 线程从同一 Scene 修订生成绘制、裁剪、效果区域与不可变 `InputSnapshot`，通过
`FramePacket.input_snapshot` 发布 `shared_ptr<const InputSnapshot>`。快照包含独立 Scene
身份、该 Scene 内递增版本、节点身份/代数、布局盒、圆角、祖先裁剪、可用性与 action。
相同节点索引或相同版本数字不足以证明来自同一 Scene。当前 Visual 子树完全不参与
命中；交互节点整体变换仍需后续逆变换契约，不能用布局快照宣称已经支持。

worker 只在成功采用提交包之后切换当前输入快照，并将同一个不可变对象附在随后收到的
`SequencedWindowEvent.input_snapshot`；提交结果同样引用实际采用的 FramePacket。
UI 按反向队列顺序应用提交快照，使用 `Scene::HandleInput(event, snapshot)` 解释事件。
首个成功提交前没有可用快照，指针或触摸按下/释放不能激活动作；configure 与关闭事件
仍正常处理。UI load 已被替换的输入确认消费后丢弃，不能落到新 Scene 的同索引节点。

worker 当前版本、在途候选、已提交包与仍排队的输入分别持有强引用；最后一个拥有者
释放后对象自动回收。当前输入版本即使暂时没有事件引用也由 worker 保留。候选替换、
队列消费/销毁、UI 卸载与失败清理释放各自租约；有界队列维持有界在途保留，不建立
无限增长的版本历史表，也不以 UI 的最新 Scene 指针代替事件自带的对象。

尚未提交的布局或动画候选不能提前改变已提交的命中几何。与像素绑定的 surface 输入/
效果更新跟随像素一起提交；独立 State 更新继续走现有通道。成功提交确定 worker 的
输入版本，只表示提交边界；物理呈现仍由 presentation feedback 单独关联。

相邻 motion 合并要求相同 UI、窗口、逻辑 source（seat/device/generation）和同一个
snapshot 对象；touch 还要求相同 contact。不同对象即使版本数字相同也不能合并，
Down/Up/Cancel/Frame 与其他有序事件保持边界。已捕获序列锁定目标身份，但后续坐标
根据事件所带的已提交快照解释；不能因下一帧命中了另一个按钮而转移捕获。
隐藏、禁用、卸载、action 变更、拓扑失效与取消优先于迟到动作，不允许旧快照复活目标。

## 9. 后续组与分隔线控制协议

以下是接口职责草案，不是现有 ABI：

| 类型/阶段 | 必需信息 |
| --- | --- |
| ControlTarget | output、workspace/group、稳定 node/boundary ID 及代数 |
| LayoutSnapshot | topology revision、实际模式、目标几何、可用操作与尺寸约束 |
| BeginControl | 目标、输入序列、期望 revision、已获授的 Shell 能力 |
| UpdateControl | 会话 ID、单调序号、归一化进度或边界位置 |
| EndControl / CancelControl | 明确提交/取消及最终意图，只生效一次 |
| ControlResult | request/session ID、接受/拒绝/取消、WM 当前 revision 与真实状态 |

当前调试树 JSON 使用对象地址标识节点，不能成为持久控制 ID。类 i3 容器可有多个子节点，
BoundaryId 必须明确父容器与相邻子节点对，或由 WM 分配独立身份；不用短暂数组下标。
手势绑定的边界、窗口或输出消失、拓扑被其他操作修改时取消会话，按当前权威布局恢复，
不能用旧快照覆盖新的合法操作。缩放比例受所有受影响子树的最小尺寸与父容器范围约束。

请求走具备能力限制的 typed Shell 通道，沿用当前 launcher/WM 的会话、进程与 ShellPermit
身份核对。action 的字符串内容不产生系统权限，现有调试用 `prism-msg` 文本命令不能
直接成为生产手势协议。Dock 的活动指示需要真正的活动实例广播，不能仅观察自身请求回复。

组模式切换提交明确的目标工作区几何，不用视觉动画的每个样本反复改 Shell 保留带并
configure 全部客户端。连续分隔线拖动可更新实时目标，但按提交能力合并中间位置，保持
configure/客户端 buffer 与输入的同步。视觉预览、请求已接受、客户端已换尺寸、实际呈现
分别记录；旧 buffer 的缩放过渡不能冒充新布局已经可交互。

## 10. 动画与材料的设计方向

下表保留完整运动方案的待校准参数，第二阶段实际采用的字面量见第 13.5 节。
玻璃、圆角、阴影的具体样式由主题决定。

| 场景 | 初始视觉方案 | 调度/交互约束 |
| --- | --- | --- |
| hover/focus 进入控制区 | 横线从中心伸展约 10–15%，轻微提亮，120–160 ms | 固定感应区；键盘焦点有独立清晰提示 |
| 按下/接触 | 约 60–90 ms 的轻微收缩或强调 | 立即反馈，不等待启动/系统请求往返 |
| 拖动已成立 | 有限跟手位移或长度变化，显示操作方向 | 当前输入直接控制目标；不叠加一个滞后的 ease 动画 |
| 释放/取消 | 从当前样本向权威目标收敛，160–220 ms | 首期用时长曲线，弹簧接入后再校准过冲 |
| Dock 运行/活动指示 | 运行横线常驻，活动态与悬停分别强调 | 状态变化来自实际实例/焦点，动画不伪造运行或激活成功 |
| 组沉浸进入/退出，后续 | 控制区给方向反馈，Shell 整体收起/恢复 | 先保证恢复入口；几何、buffer 和输入通过协议对齐 |
| 分隔线调节，后续 | 边界横线与受影响子树形成一致反馈 | 只调所选边界，保持树约束；误触或取消可恢复 |

默认静止时不循环呼吸、闪动或弹跳。时钟每秒更新不启动控制横线动画。小横线优先使用
局部颜色和变换，保持玻璃模糊半径和大面积阴影稳定；下层内容变化仍按真实依赖更新
背景效果，不能承诺整个面板动画时零 blur 成本。

减弱动效模式保留清晰的 pressed/focus/实际状态反馈，缩短或取消位移、弹簧与装饰过渡。
直接操纵过程仍及时响应，完成事件、控制请求和最终布局不依赖动画是否播放。

## 11. 实施顺序与验收

1. **输入身份与局部状态。** HitResult/NodeId、明确 leave/cancel、按下/释放、捕获和状态
   作用域；把旧即时 hover/focus 绘制迁到统一状态结果。先保持现有静态视觉，验收重复
   action 的不同节点、拖出释放、隐藏/卸载/Close 取消、键盘 repeat 与焦点变化。
2. **状态规则与呈现属性，第二阶段源码已接入。** 纯准备期类型校验、状态优先级、来源保留，
   以及本地视觉子树的变换/裁剪。先接固定感应区内的横线反馈；验证无 Layout 增量、
   旧/新位置修复、主题切换、重新定向和结束后停帧。整体 opacity 必须通过重叠子树对照。
   当前运动参数为字面量，MotionSpec/MotionPolicy 的设计与接入另行实施。
3. **完整输入与快照协议。** 贯通真实 touch 事件、设备身份、取消与坐标，以及交互变换
   的同代命中/效果、输入事件引用与资源回收；鼠标、触摸、键盘分别验收。
4. **权威布局控制。** 稳定边界身份、typed 控制会话、活动实例订阅、组沉浸与边缘恢复，
   再接 Topbar/分隔线的点击或手势映射。具体映射在这一阶段冻结，不提前塞入动画核心。
5. **保留层与窗口运动。** 沿通用呈现模型接入资源保留和合成采样，再实现窗口/组过渡，
   分别测真实 resize、旧 buffer 预览、玻璃依赖与输入；WM 复用时间核心且独立拥有其轨迹。

每一步保持测试/probe 在 tests/，遵循 [代码规范](CODING_STYLE.md)；按
[动画假设清单](ANIMATION_RENDERING_HYPOTHESES.md)记录功能、开销与设备范围。
各阶段分别记录代码能力与实际部署，不把准备好的状态类型当作组全屏或触摸控制已完成。

## 12. 第一阶段源码实现（2026-09-29）

前序动画核心与本规范先提交为 `2f9345b`，随后开始本阶段。当前实现包括：

| 接口/路径 | 当前能力 |
| --- | --- |
| Scene::HitTest | 返回当前 Scene 内完整 NodeId 与局部坐标；action 不参与身份匹配 |
| Scene::HandleInput | 接受 typed WindowEvent，返回局部状态变化与确认完成的 Activation |
| Scene::State / SetEnabled / CancelInput | 查询统一状态、对子树启用/禁用输入、明确清理当前 Scene 输入 |
| InteractionState | hovered、pressed、captured、focused、focusVisible、enabled；按来源/seat维护后向节点聚合 |
| Wayland 适配 | 显式 enter/leave/cancel；InputSource 标识 seat、逻辑设备及重建代数；xkbcommon 解析修饰键 |
| SDK/worker 队列 | 输入携带 UiLoadId；旧加载输入只确认消费、不派发；motion 合并不跨来源/加载/控制事件 |
| 生命周期 | 隐藏、禁用、action 改变、区域移除、失焦、设备撤回、UI 替换及 Close 取消对应输入 |

主按钮按下只建立捕获，在原目标内有效释放后激活一次。移出目标时 pressed 撤销而捕获
可保留；返回原目标后释放允许激活。显式 leave 后 button 事件中的缓存坐标不能重新
建立 hover，必须由 Enter/Motion 恢复，避免在窗口外释放仍触发按钮。键盘 Enter/Space
释放时激活，repeat 不重复提交；Tab/Shift+Tab 导航，Escape 取消当前 seat 的待定激活。

输入状态放入不可变 SceneSnapshot；现有 hover/focus 呈现从该状态取值，鼠标聚焦与
键盘可见焦点分别处理。未改变状态的同目标 motion 不产生像素失效，局部状态变化不触发
Layout；静止指针只在命中几何发生变化时重新命中，不因普通 Paint 动画反复扫描。
事务提交后的输入清理不分配新存储，保留区域不会因无关区域安装丢失捕获。

本阶段运行接口详见 [Scene](CLIENT_SCENE_RUNTIME.md) 和 [SDK](CLIENT_APP_SDK.md)。
SetEnabled 目前是 C++ Scene 接口，未扩展 DSL schema。该阶段结束时，声明式状态作用域、
StateRule、独立感应范围与 transform/opacity 尚未接入，第二阶段进展见下节。dragging/
触摸识别、成功提交对应的命中快照、组沉浸和分隔线系统操作继续按第 11 节实施。
平台目前只适配一个 Wayland seat；纯状态
测试中的多 source/seat 隔离不代表多 seat 桌面或触摸已接通。

验证：Pi 上完整 GLES 构建成功，最终 CTest **59/59** 通过，含新增 Scene 输入矩阵、
真实 Wayland Shift/普通 Tab、设备撤回/重建、configure 与释放同批处理、动作中替换 UI
后的旧输入隔离，以及原有主题、区域事务和动画回归。隔离 V3D headless 的 prepared UI、
SDK 提交、局部损伤、动画四项门槛均通过。风格、800 行与差异检查通过。
记录位于忽略的 `dist/validation/interaction-v1-20260929/`，最终全量测试为
`ctest-final.log`，GPU 门槛为 `native/native-gates.json`。本阶段未重新打包或部署到当前
VNC 会话，也不据此宣称触摸、组控制、真实显示 FPS 或平台间性能比较已验收。

## 13. 第二阶段：状态规则与装饰子树呈现（2026-09-29）

输入阶段先提交为 `9748bdb`。本阶段扩展通用前端与绘制契约，不在 WM 或业务模块中加入
横线、应用名称、悬停样式或逐帧 binding 更新。以下语法需要包含本阶段源码的 Host/SDK。

### 13.1 固定目标与装饰子树

`InteractionTarget` 是采用 Card 叠放布局的输入容器。它接受常规容器布局、样式和可选
命名 `action`；即使无 action，也能形成 hover/pressed/captured 状态，但释放不派发空动作。
命中使用目标的稳定布局盒、圆角和祖先 clip。它不接收本阶段的呈现变换或 opacity 属性。
`Visual` 也是叠放容器，其自身和所有子孙不参与命中与 surface 输入区域收集。
以下控件片段放入应用已有的 Card/HStack 等父容器；Scene 根仍按 viewport 布局，
固定目标尺寸由父容器内的子节点布局约束。

```prism
InteractionTarget(width: 96, height: 40, action: "panel:toggle", justify: "center") {
    Visual(width: 64, height: 4, anchor: "center",
           background: "@indicator", cornerRadius: "@indicator_radius")
        .state(when: "hovered", scope: "target", scaleX: 1.15)
        .state(when: "pressed", scope: "target", scaleX: 0.94)
        .state(when: "focusVisible", scope: "target", background: "@accent")
        .transition(property: "scaleX", durationMs: 140, easing: "easeOutCubic")
        .transition(property: "background", durationMs: 120, easing: "easeOutCubic")
}
```

示例 action 由应用业务定义，不自动获得窗口或组控制能力。无 action 的 Topbar 控制区
当前只呈现输入反馈。`InteractionTarget` 不使用旧按钮的隐式 hover/focus 叠加图元，作者应
通过状态规则明确提供所需反馈；已有 Button/IconButton 等继续保持原有主题 Controls 行为。

`Visual` 可以嵌套，但不能作为组件根。其子树内禁止 `InteractionTarget`、`Slot`，以及
任何 `action`、`material`、`backdropBlur`、`inputShape` 属性/绑定/引用声明，空字符串或
零模糊也不例外。需要静态材料时，把材料放在 Visual 外部的目标或普通 Card；本阶段
不移动背景模糊域、窗口根或外部装饰。DSL 准备、布局 Slot 转换和 Scene 安装分别检查
各自边界，直接构造 Blueprint 不能绕过这些约束。

### 13.2 状态规则的静态契约

`.state` 要求命名 `when`、`scope` 和至少一个属性。`scope` 当前只能为字符串 `"target"`，
表示结构上最近的 `InteractionTarget` 祖先；声明节点必须位于该目标的 Visual 子树中。
准备期验证结构，安装期关联稳定目标身份，不在每个输入事件中按 action 或名字搜索。
状态只读，不发给业务模块。当前不支持任意祖先选择器、表达式、脚本或状态组合语法。

| when | 语义 / 优先级 |
| --- | --- |
| `disabled` | 目标或祖先禁用；同属性优先级 400 |
| `pressed` | 有效按下；同属性优先级 300 |
| `captured` | 指针序列仍被该目标捕获；同属性优先级 200，移出后也可保持 |
| `hovered` | 指针在该目标内；同属性优先级 100 |
| `focused` / `focusVisible` | 焦点与键盘焦点提示；使用独立属性通道 |

普通条件可覆盖同一属性，按上述优先级选择；不匹配时回到当前基础值。focused 或
focusVisible 声明的属性不得被任何不同条件再次声明，包括另一种焦点条件；这样焦点提示
不会被悬停悄悄覆盖。同条件同属性重复声明拒绝；同条件分多条声明不同属性允许。
规则内只接受合法的数值/颜色字面量或 `"@token"`，拒绝 `$binding`。基础属性仍可绑定业务值。
未激活规则的主题引用也必须在安装与候选主题验证时合法，不能等第一次 hover 才发现错误。

规则保存 `StateRule { condition, properties, theme_refs }`，沿 PreparedNode、Blueprint、
组件组合与区域事务传递；向量与字符串容量计入准备结果 retained bytes。没有新增 JSON
协议或任意源码在运行期求值。组件数、源文件与 AST 既有上限继续适用。

### 13.3 呈现属性范围

| 属性 | 默认值 / 有限范围 | `.state` | `.transition` |
| --- | --- | --- | --- |
| `Visual.translateX/Y` | 0；−8192..8192 逻辑像素 | 允许 | 允许 |
| `Visual.scaleX/Y` | 1；0.01..8 | 允许 | 允许 |
| `Visual.originX/Y` | 0.5；0..1，在自己的布局盒内归一化 | 允许，立即取值 | 拒绝 |
| `Visual.opacity` | 1；0..1 | 允许 | 允许 |
| `Visual.background` | 透明色或作者值 | 允许 | 允许 |
| Visual 子树内直接绘制节点的 `foreground` | 沿用原有属性 | 允许 | 沿用原有允许范围 |

这里直接绘制节点为 Text、Icon、IconButton、Progress、Toggle；Button 的生成 label 不
隐式继承状态规则。`Progress.value` 保留已有业务目标 Transition，但不作为状态覆盖属性。
布局尺寸、visible、action、资源、材料、模糊、边线、阴影等不在本阶段状态/过渡扩展范围。
width/height 仍决定布局；scale/translate 不改变兄弟分配、目标的感应面积或 BSP 配置。

目标解析保留基础常量、主题引用及 binding。状态覆盖与动画样本分层保存，退出状态读取
最新基础来源；输入快速变化从当前呈现样本重定向，不先跳回基础。主题候选先验证，成功
后取消旧轨迹并采用当前状态下的新主题结果；失败保留旧主题和呈现。隐藏、卸载、禁用和
取消继续遵循通用输入/动画生命周期，不保留失效按下动作。

### 13.4 绘制、输入与成本

每个 Visual 的变换和整体透明度随不可变 Scene snapshot/FramePacket 进入 RenderTree 与
DisplayList。变换作用于自己的背景、子孙、局部裁剪和阴影；组合顺序为父变换 × 平移 ×
原点平移 × 缩放 × 原点逆平移。opacity 对整组完成的像素合成一次，两个重叠子项不各自
降低 alpha；具体命令见 [DisplayList 契约](DISPLAY_LIST_CONTRACT.md)。

这些属性当前标记 **Paint**。UI 采样、更新列表，Skia 回放，group opacity 可能需要临时
图层；尚无持久 GPU 保留层，也没有渲染线程自行计算 Scene 轨迹。变换不产生 Layout
失效，但不是零绘制开销。损伤必须覆盖旧/新范围、阴影与嵌套裁剪；无法证明安全时完整
回退，buffer age 修复仍相对最后成功提交计算。实现细节与证据见
[渲染失效规范](RENDER_SCHEDULING_AND_INVALIDATION.md)。

因为 Visual 完全不参与输入，目标的命中几何与 surface input 不随装饰动画变化，
无需为 Visual 发布逆变换命中几何。第二阶段结束时尚未完成第 8 节输入快照协议，
第三阶段的布局快照与 touch 进展见第 14 节。
opacity 为 0 只隐藏装饰像素，不禁用目标；真正禁止操作仍使用输入生命周期接口。

### 13.5 真实 Shell 接入与验证范围

- Topbar 保留现有主题布局，横线外增加 96 × `@topbar_handle_plate_height` 固定目标；
  当前主题高度为 8。无 action，hover scaleX 为 1.12，pressed 为 0.96。
- Dock 保留应用中心、固定应用、运行应用三段。仅运行段 Music/Preferences 使用完整
  `@dock_icon_size` × `@dock_slot_height` 目标，仍派发原 `app:launch:*`；图标和 dockTile
  材料静止，运行横线 hover scaleX 为 1.15，pressed 为 0.90。显隐仍来自真实实例状态。
- 两处横线 scale 过渡为 140 ms、颜色为 120 ms，均为 easeOutCubic；键盘焦点以
  `@accent` 提示。颜色与尺寸继续引用主题，运动幅度/时长目前是 DSL 字面量。

这些范围沿用当前鼠标布局；Topbar 的 8 像素目标不宣称已满足触摸体验。MotionSpec、
减弱动效策略、拖动/多点触摸、窗口分隔线、组沉浸/恢复和真实活动窗口广播仍未实现。
本阶段没有重打 deb、替换当前 VNC 会话或宣称平台间性能优势。

本阶段验证：Pi 完整 GLES 构建和最终增量确认通过；CTest **60/60** 通过，包含新增
`scene_state_test`、DSL 正反例、真实 Shell 模板、主题与区域事务、输入和原有动画回归。
状态变化、动画重新定向与结束停帧通过；纯呈现变化不增加 Layout，命中范围保持稳定。

Skia 损伤测试通过 86 帧 × 1/2/3 buffer 轮转。隔离 V3D Wayland 完成 85 场景、86 次整目标
读回对照，覆盖整体 opacity、重叠内容、嵌套变换、裁剪和非等比缩放阴影；partial/full/empty
修复计数为 58/17/11。证据见 `dist/validation/state-presentation-20260929/`。

SDK 的 prepared UI、提交、损伤、动画与新增输入动画 **5/5** 门槛通过。新增 native probe
使用真实 virtual pointer 验证静止指针因布局变化重新命中、hover/press/release/leave、
仅一色阶的前景动画在连续同目标 motion 后仍完成，以及 UI 替换取消旧动画。每段结束后
停止构建/渲染/交换；逻辑设备取消另由协议 fixture 和 Scene 测试覆盖，不把移除某个物理
virtual pointer 等同于撤销客户端的逻辑 wl_pointer。

初次并行编译期间，旧 SDK 损伤 gate 有一次等待提交超时，原日志保留；独立复查和两次
原顺序复查均通过，未放宽 5 秒或 24 帧断言。此次没有确认该超时根因，不据此给出吞吐或
FPS 结论。两处新测试 fixture 的目标范围假设也已按既有根节点 viewport 布局规则修正。
最终记录为 `dist/validation/state-visual-v1-20260929/ctest-final.log` 和
`native-final/native-gates.json`；风格、800 行、文档链接和差异检查通过。所有隔离 WM
已回收，当前 VNC 会话未替换。


## 14. 第三阶段：成功提交输入快照与 touch（2026-09-29）

第二阶段提交为 `2219115`。本阶段连接第 8 节的不可变命中快照租约，以及 WM →
WaylandWindow → SDK → Scene 的 touch 事件。输入 source 继续表示客户端的逻辑 seat/
device/generation；触摸 contact 在该 source 内区分，接触移动和释放不伪造持久 hover，
也不合成为鼠标点击。原生 pointer button 与 touch down 的 `protocol_serial` 作为 opaque
平台凭据保留；WM 控制仍需核对 seat/surface、有效输入序列和操作能力，不能只相信
客户端提供的 serial/source/time。手势识别、拖动阈值与组控制不属于这次接线。

Scene 的 `CaptureInputSnapshot()` 在布局完成后独立冻结候选，随 FramePacket 发布；
action-only 或 State-only 更新不增加显示列表构建尝试和像素版本。`InputGeometry()`
读取最近冻结的候选；`ApplyInputSnapshot(...)` 只在
有序成功提交边界更新已提交几何与静止指针重新命中。事件通过自带快照命中，仍须用
当前 Scene 验证节点存活、action 和可用性。候选布局移动/缩小同一个节点时，积压旧
输入应继续命中旧提交几何；随后收到的输入切换到新提交几何。整个 UI 替换会隔离旧
Scene 身份和 UI load，即使新节点索引、action 或几何相同也不能承接旧序列。

测试资源全部保留在 `tests/`：队列测试覆盖 snapshot 对象/版本隔离、contact 边界和
`weak_ptr` 租约回收；协议 fixture 验证首帧前配置可确认但点击不可激活；新增
`sdk_input_snapshot_probe` 在独立 V3D WM 中验证 UI 暂停后积压点击、绑定移动/缩小、
新提交几何与动作中替换 UI 后的旧输入隔离。该 probe 的注入通过 compositor roundtrip
后等待 worker 调度，公开 SDK 当前没有输入接收 fence；单窗口/单输出坐标前提由初始
实际点击验证。

Pi 原生 V3D 的六项 SDK 门槛通过：prepared-ui、submission、damage、animation、
input-snapshot、input-animation。首次集成暴露 State-only 冻结调用 Build 导致构建尝试
增加，现已拆出 `CaptureInputSnapshot()`，原有零构建断言保持不变并通过复测。
新增 Scene 回归检查 action-only 快照版本、旧快照不可变和无额外像素/构建工作。
最终完整构建、CTest **61/61**、clang-format 19/行数/goto 检查与 `git diff --check`
通过；297 个生产文件中最大文件为 594 行。

`virtual_input_test` 经真实 wlroots touch 信号、WM、Wayland 协议及客户端验证多设备
同号触点隔离、多触点、拖出原 surface 后坐标、up/frame/cancel、设备撤回与重建、窗口
卸载和关闭清理，以及原生 serial 保留。此项是协议注入，尚未进行实体触屏验收。

证据位于 `dist/validation/input-snapshot-v1-20260929/`：`native/native-gates.json`
记录六项结果与隔离 WM 回收，初轮失败保留于 `native/initial-state-composite-failure/`。
最终 Capture 圆角/裁剪修正后的 prepared-ui 与 input-snapshot 定向复测也通过，记录于
`native-final-capture/native-gates.json`。新增布局用例曾把填满 viewport 的根控件宽度
误当成声明宽度，现已改为真实子节点验证；初始失败保留于
`ctest-initial-fixture-failure.log`。
Wayland 首帧 fixture 也修正了异步调度假设：初始状态检查后显式释放 configure/input，
以 xdg ping/pong 确认 worker 已分发整批事件。原先自动发送 configure 可能先于 Open
返回，被初始零计数断言误报；原零动作/零 GPU 断言保留，定向连续 30 次与最终全套通过。
初始失败与修复记录为 `ctest-initial-wayland-timing-failure.log`、
`ctest-wayland-fixture-fix.log`。
最终完整构建、CTest 与格式结果分别由 `build-final.log`、`ctest-final.log`、
`style-final.log` 记录。本轮使用既有 `build-gles`；全新配置时发现系统
`/usr/share/cmake-3.31/Modules/CMakeParseImplicitIncludeInfo.cmake` 含损坏字节，尚未修复
该环境问题，因此不宣称全新目录配置通过。

当前布局快照不提供交互节点逆变换，Visual 仍是装饰子树；成功提交也不等于显示端已经
呈现。本轮未重打 deb 或替换当前 VNC 会话。组沉浸、连续手势和分隔线控制按
[后续实施计划](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md)推进。

## 15. 连续手势与 WM 控制会话（2026-09-30）

在第 14 节的已提交输入快照基础上，InteractionTarget 新增 `.gesture`，提供
Begin/Update/End/Cancel 与 `dragging` 状态；SDK 在 owner 线程交付有界事件序列，支持
回调中替换 UI 与取消。Host/module 通过可选 C ABI 尾部消费手势和 WM 布局快照，
经既有 worker/launcher 私有通道向 WM 建立带真实输入凭证的控制会话。

Scene 不包含 Shell 操作策略；Host 处理在途请求和终止顺序；WM 检查实际 surface、
注册身份、原始 Down serial、目标和修订。当前交付为跟踪会话，尚不应用组沉浸或
分隔线几何。普通应用可使用本地连续手势，系统控制权限仍受 Shell 身份限制。
接口示例、权限矩阵、容量、撤销与自动化结果集中于
[布局控制计划第 8 节](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md#8-第二步源码交付连续手势与-typed-控制会话)。

## 16. 鼠标组沉浸与恢复（2026-10-01）

第三步在上述权威会话中应用组模式。Topbar 横线的鼠标下拖在释放后提交意图，快照
驱动实际模式；沉浸扩展整个 BSP 组并保留分割关系。中央顶边鼠标入口临时唤出原
Topbar，`Super+Shift+F` 提供 WM 独立恢复，单窗 `Super+F` 继续独立。

鼠标按下后跨越窗口或透明区域，移动和释放保持最初 surface 的坐标与归属；最后释放
后才重新命中。系统边缘点击从 Down 起归 WM，不传给应用。临时控件收起等待仍在途的
有效控制输入结束，避免鼠标 Up 比异步 Host 请求先到达而撤销恢复操作。

Shell worker 故障撤权并恢复安全组状态，保留 WM 与普通应用，无自动重启。当前阶段
以鼠标和键盘验收，新的触屏操作延期；细节与验证记录见
[布局控制计划第 9 节](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md#9-第三步源码交付鼠标组沉浸与恢复)。
