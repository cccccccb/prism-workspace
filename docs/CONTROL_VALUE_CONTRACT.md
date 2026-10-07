# 值控件：交互、业务状态与呈现契约

日期：2026-10-05。界面系统第 3 阶段，3a 值交互核心及 3b Checkbox、Radio/Segment、Slider 通路。

## 1. 能力与执行顺序

| 子阶段 | 内容 | 当前状态 |
| --- | --- | --- |
| 3a | 类型化值、范围归一、预览/提交/取消、业务修订与过期交互 | 已实现独立 C++ 核心与测试 |
| 3b | Scene 控件、DSL schema、成功提交快照命中、SDK/Host/模块值事件 | Checkbox、Radio/Segment 组与水平 Slider 已接入 |
| 3c | 四种控件的主题呈现、完整鼠标/键盘、滚动容器与实际截图 | 待实现 |

本文件包含后续 3c 约束。Checkbox、RadioGroup/Radio、SegmentGroup/Segment 与水平 Slider 在本轮源码中可用（Slider 见第 11 节），
旧 v22 不支持。现有 Toggle/action 语义不变。未更新运行中桌面。核心不是业务 C ABI，
模块通过独立 C 结构接收值，不把 std::variant 跨动态模块边界传递。

## 2. 职责与值域

核心位于 `prism/runtime/control_value.cpp`，公开接口为
`prism/include/prism/runtime/control_value.hpp`，随 `prism_client_scene` 构建。
它不依赖 Wayland、Skia、主题或具体应用，不创建线程、计时器或渲染任务。
所有调用在前端所有者线程完成，渲染端仅接收复制出的最终呈现值。

| 控件（待接入） | 类型 | 值域 | 业务绑定 |
| --- | --- | --- | --- |
| Checkbox / Toggle | Boolean | false / true；不隐式转换 0/1 | checked |
| Slider | Number | 有限 minimum < maximum，step ≥ 0 | value |
| Radio 组 / Segment | String key | 非空、唯一、稳定 key 集合 | selectedKey |

Radio 的值属于组；不能让多个独立 bool 绑定相互竞争。选项显示标题可本地化，key
不随显示文本改变；选项索引不是持久业务身份。C++ 核心校验 key 属于集合，Scene 已约束单项禁用和隐藏。选项集合/身份创建后固定，
动态替换集合尚未提供；重建整组时由节点生命周期清除原会话。

Number 不接受 NaN/Inf，先 clamp，再按 minimum 为原点四舍五入到 step，最后 clamp。
step=0 表示连续值；中点朝较大值取整。minimum/maximum 始终可直接到达，即使 maximum
不是完整步长。这保证 Home/End 能到达端点。非有限域、反向/零长度范围及负步长拒绝。
初始值和业务同步也使用同一归一规则；应用应发布已归一值，不能靠前端容纳非法类型。

## 3. 会话与修订

每个逻辑控件保存一个 `ControlValueSession`，包含：

- authoritative：最近一次有效业务同步值；控件不能自行把用户意图当业务结果。
- presented：交互期间的暂时显示值，非交互时等于 authoritative。
- revision：业务状态修订，从调用者传入；同一值不同新修订也视为新权威状态。
- interaction：本 session 内递增的非零序号；不能作为跨节点/跨窗口全局身份。

`Begin()` 只开启会话，不发送事件。活动期间再次 Begin 属于调用错误，不覆盖旧事务。
`Preview(id, value)` 对归一后重复值不发送事件；新值只改变 presented。
`Commit(id, value)` 始终生成一次终结事件，即使值未改变；随后关闭会话，呈现恢复权威值。
`Cancel(id, reason)` 发送一次取消事件并恢复权威值；取消不能使用 None 原因。
迟到的 Preview/Commit/Cancel 序号与当前会话不匹配时忽略，不影响新会话。

事件携带 interaction、开始时的 revision、phase、before、value、取消原因。
Preview/Commit 的 value 是提议值，Cancel 的 value 是要恢复的原业务值。
只有 `Synchronize(value, newerRevision)` 接受权威值，不把 Commit 等价于业务成功。
同步 Commit 回调可立即发布新状态，下一帧呈现接受后的值；异步业务应显式显示 pending，
不能让控件永久保持未经确认的乐观值。

同步规则：

1. 更旧 revision 忽略，包括迟到异步结果。
2. 同 revision 同归一值幂等；同 revision 不同值拒绝，状态保持不变。
3. 新 revision 先校验值，活动会话生成 Superseded 取消，再安装新值与修订。
4. Superseded 事件携带旧修订/旧值，不能当成要求业务写回旧值的命令。
5. 无效值不得取消当前会话或破坏原值。

输入路由接入后，事件还必须携带节点生命周期身份与具名回调目标；不能只凭 interaction
把消息交给重建后的同名节点。节点删除/隐藏/禁用取消为 Unavailable，窗口失焦为
FocusLost，Esc 为 Escape。离开 Slider 矩形不等价取消，捕获有效时允许拖到边界外。

## 4. C++ 示例（当前可用）

```cpp
using namespace prism::runtime;
ControlValueSession volume(NumberDomain{0, 100, 1}, 40.0, 12);
const auto interaction = volume.Begin();
const auto preview = volume.Preview(interaction, 67.2); // 67，仅暂时呈现
const auto proposal = volume.Commit(interaction, 67.2); // revision=12，提议 67
// 业务校验并接受，发布新状态：
volume.Synchronize(67.0, 13);
```

Preview 不应默认写盘、发网络请求或触发昂贵工作。Commit 交给业务确认，Cancel 负责
结束暂时效果。需要实时音量预览的业务可以显式消费 Preview，并处理 Cancel 回退；
通用控件不内置音量、主题或设置业务。

## 5. 鼠标与键盘规则（本轮源码已接入）

| 控件 | 鼠标 | 键盘 |
| --- | --- | --- |
| Checkbox | 主键按下捕获，在目标内释放切换并 Commit；外放取消 | Tab 聚焦，Space 释放切换；Esc 取消未结束按键 |
| Radio | 点击可用选项提交其 key；当前项重复点击不产生第二次业务动作 | 组为单一 Tab 停靠点；方向键选择可用项并提交，Home/End 到两端 |
| Segment | 与 Radio 相同，选中指示与焦点环分别呈现 | 与 Radio 相同 |
| Slider | 轨道按下与拖动 Preview，释放 Commit，Esc 回退 | 方向键步进，Home/End 到两端；按下/重复 Preview，释放一次 Commit |

Slider 连续域键盘步长为范围的 1%，离散域为 step；PageUp/PageDown 为十步。
键盘与指针不能同时拥有同一值事务；来源切换必须先取消旧事务。
禁用组/祖先 veto 当前输入，已提交快照不能复活已禁用控件；焦点只落到可用选项。
主题或动画只消费状态，不发业务值事件。交互数值按输入事件变化，不受渲染帧率控制。

## 6. 输入和模块接入计划

Scene 输出增加独立 typed value event；保留原 Activation/TextEdit 兼容路径。
SDK 用具名回调接收该事件。业务 C ABI 以尾字段扩展值事件回调，先检查完整 struct_size；
C 结构使用值种类、显式数值/布尔/字符串字段及阶段，借用字符串只在回调内有效。
不得把 `volume:67` 塞入 action，也不得引入业务侧 JSON 字段猜测。

Slider 几何取自成功提交输入快照，业务值从当前 session 取得；渲染不得修改 session。
一次输入的状态、布局/绘制失效与事件投递按现有所有者线程顺序完成。Cancel 先结束旧
事务，再允许新 Begin；回调重入、移除节点及同步状态回写需有集成回归。

滚动与值提交是不同接口：滚动更新 viewport/offset，不能为了复用 Slider 伪装业务值。
通用滚动容器在控件输入链稳定后实现，包含裁剪、焦点可见与嵌套边界策略。

## 7. 验证边界

`control_value_test` 覆盖归一、端点、预览去重、终结一次、非法值原子性、迟到事件、
修订冲突/过期、新状态取消、拒绝提议、稳定选项 key 与会话重入拒绝。
本测试不代表 DSL、真实鼠标捕获、键盘、C ABI、视觉或 GPU 性能已经验收。

## 8. Checkbox 接入（3b 首步，当前源码可用）

Checkbox 是有值语义的容器，采用与 InteractionTarget 相同的 Box 布局，支持 checked
Boolean、action 具名事件标识、enabled、visible、尺寸及容器属性；子树负责呈现。
需要交互时必须设置非空 action；它路由到 **OnControlValue / on_control_value**，
不再另发 OnAction。普通 Toggle 的旧行为保留，不静默改变现有应用。

```prism
Checkbox(action: "notify", checked: $notifications, height: 44) {
    Visual(paddingX: 8, justify: "center") {
        HStack(spacing: 12, height: 24, align: "center") {
            Visual(width: 22, height: 22, background: "@controlField",
                   cornerRadius: "@control_state_radius",
                   borderWidth: 1, borderColor: "@controlOutline") {
                Icon("check", width: 16, height: 16, anchor: "center",
                     visible: $notifications, foreground: "@controlPrimary")
            }
            Text("Desktop notifications", flex: 1, font: "@font_body",
                 lineHeight: "@line_body", foreground: "@text")
        }
    }.state(when: "pressed", scope: "target", opacity: 0.7)
}
```

完整的焦点、悬停、禁用及已选背景配方见
[Checkbox 样例](../tests/fixtures/interface-system/checkbox.prism)。checked 与勾选图标
共用业务 binding；控件不会偷改子树内容。主题配方可自由更换，WM 与 Skia 不增加
Checkbox 特定绘图分支。checked 不允许 state/transition 写入；视觉动画仍作用在 Visual。

离散 Checkbox 的按压阶段只建立输入捕获，不启动值预览；有效释放时原子 Begin/Commit，
只发送一次 Commit。外放、Esc、失焦、隐藏/禁用、按压期间 checked 值改变等都抑制提交，
因为尚未产生 Preview，所以没有业务 Cancel 事件需要回退。不要据此推断 Slider 的连续
预览也可省略 Cancel。Tab 聚焦、Space 释放提交，重复按键不重复提交，Enter 不切换。
触控路径暂不提供 Checkbox 值提交。

Scene 使用每节点 checked 修订（初始 0，值变化递增）校验捕获，包含原子 Preflight 的
绑定投影；false→true→false 也会取消旧捕获。与布局/悬停 revision 分离，画面刷新不会
取消按压。同值 set_binding 是幂等操作，不产生新值修订；现有 set_binding ABI 没有业务
revision 参数。事件 revision 因此是前端值修订，**不是数据库/后端版本**；第 3 节中
调用者显式传入新 revision 的通用 C++ 会话语义仍成立。

业务回调示意（完整可编译版本见 tests/fixtures/control_value_module.c）：

```c
static void ControlValue(void *instance, const PrismControlValueEventV1 *event)
{
    App *app = instance;
    if (!event || event->struct_size < sizeof(*event) ||
        event->phase != PRISM_CONTROL_COMMIT_V1 ||
        event->action.size != 6 || memcmp(event->action.data, "notify", 6) != 0 ||
        event->value.kind != PRISM_VALUE_BOOL_V1) {
        return;
    }

    app->host->set_binding(app->host->context,
        (PrismStringViewV1){"notifications", 13}, event->value);
}
```

模块导出表末尾设置 `.on_control_value = ControlValue`。Host 仅复制 struct_size 覆盖的
完整尾字段，缺失/不完整回调保持空；不退回 action 字符串模拟值事件。事件及字符串只
在回调内有效，需异步处理时复制业务数据。node_index/generation 是本次 UI 内身份，
不可跨 UI 替换持久保存；SDK 在完成 Scene 输入处理后投递，并检查 UI 及节点有效性。
模块退出、错误线程或未创建实例不投递。

本轮用独立 fixture 验证 Scene 点击→ModuleSession→真实 C 模块→set_binding→Scene，
并构建正式 Host/SDK 通路；不将测试模块安装进系统。视觉 probe 使用真实 Skia raster
和四主题的明暗、320/640 宽度；这不等价于已部署 VNC 或验证 GPU 性能。

## 9. Radio 与 Segment 单选组（当前源码可用）

组级 `selectedKey` 是唯一业务状态，使用稳定 String key；选项不维护各自的 Boolean，
标题、翻译和图标不充当 key。复用 OnControlValue/on_control_value，无新增 ABI 字段。

```prism
RadioGroup(action: "scheme", selectedKey: $scheme, spacing: 8) {
    Radio("light", height: 44) {
        Visual(paddingX: 8, justify: "center") {
            HStack(spacing: 12, height: 24, align: "center") {
                Visual(width: 20, height: 20, cornerRadius: 10,
                       borderWidth: 1, borderColor: "@controlOutline", justify: "center") {
                    Visual(width: 10, height: 10, anchor: "center", cornerRadius: 5,
                           background: "@controlPrimary", opacity: 0)
                        .state(when: "selected", scope: "target", opacity: 1)
                }
                Text("Light", font: "@font_body", foreground: "@text")
            }
        }
    }
    Radio("dark", height: 44) {
        Visual(justify: "center") {
            Text("Dark", font: "@font_body", foreground: "@text")
        }
    }
}
```

此短例重点是值与 selected 规则，Dark 的完整圆环/焦点配方见
[Radio/Segment 示例](../tests/fixtures/interface-system/choices.prism)。Segment 使用
`SegmentGroup`（横向布局）和直接子级 `Segment("key", flex: 1)`；RadioGroup 为纵向
布局。组默认在主轴按内容尺寸排列，显式 flex 才拉伸，避免与其他区域平分巨大空隙。
组可用 spacing/padding，选项内部是 Box 布局，可以组合任意纯呈现内容。

### 结构与状态约束

- 一组 1..128 个直接选项，RadioGroup 只含 Radio，SegmentGroup 只含 Segment；用
  spacing 分隔，不插入 Spacer 或异步 Slot。组不能嵌入 Visual 或另一个选项。
- key 必须是非空唯一字面量，最长 128 字节；DSL 中不以 @ 开头。不能绑定 key、从
  主题读取 key 或运行时 SetProperty 改 key；改变标题不影响身份。
- action 属于组，选项不声明独立 action。组 action 可以更新，更新会取消旧输入捕获。
- selectedKey 为当前集合内的 key；未知 key/错误类型使整次绑定投影失败，不更新一半。
  初始 binding 未就绪或显式清空为 `""` 时，整组暂不可交互；业务发布有效 key 后启用。
  业务可保留一个已禁用/隐藏项为选中值，前端不会擅自选择其他项。
- selectedKey 不可动画或由 .state 写入。新增只读 `selected` 条件供 Visual 子树使用，
  Checkbox 也可复用；选中与 focusVisible 是不同状态，应使用独立 Visual 表达。
  selected 优先级 75，低于 hovered 100 / pressed 300 / disabled 400。
- 更新选择只使必要的状态呈现失效，不改变 Radio/Segment 的布局策略；不创建周期 timer。

### 输入与业务回写

Tab/Shift+Tab 每组一个停靠点：优先当前选中且可用的选项，否则第一个可用项。未初始化
或全不可用的组跳过。即使焦点因未确认提议停在其他选项，Tab 仍正确退出整组。
四方向键按声明顺序移动并循环，Home/End 到第一个/最后一个可用项；跳过隐藏/禁用项，
并以已提交输入快照核实可交互性。方向键/Home/End 在按下时提交，忽略 repeat。
Space/Enter 在有效释放时提交，Esc 取消未完成按压。触控暂不接入。

选择当前权威 key 只更新焦点、不发重复 Commit。新选项提交 String before/value；
业务用 set_binding 确认后才改变 selected 呈现。业务拒绝不需要写回旧值。多次快速
操作的顺序以交互序号辅助识别；后台任务仍需由业务模块处理取消与迟到结果。

组级值修订阻止旧按压在状态变化后提交，包括 A→B→A；直接绑定和 Preflight 原子投影
均适用。鼠标和键盘切换会取消同组原捕获。UI/节点/动作/值修订不匹配的事件不投递。
组选择仍是离散事务，不在按压阶段发 Preview，取消按压也不发业务 Cancel。

业务可参考 [独立 C 模块样例](../tests/fixtures/choice_module.c)：校验组 action 与 String
类型，按白名单接受业务值，再通过 host->set_binding 发布；模块不接触 Scene/Skia。
示例的颜色方案名称只是测试状态，不调用桌面主题服务，不改变正在运行的系统配色。

## 10. 连续值事件投递边界

`ControlValueDelivery` 位于 `prism/runtime/control_value_delivery.cpp`。现有 Checkbox 与
Radio/Segment 的 Commit 已通过这条正式 SDK 链路；独立数值事务测试覆盖 Preview /
Commit / Cancel。投递器准备阶段没有注册 Slider DSL 节点；其后的输入与呈现接入见第 11 节。

### 所有权与调用顺序

1. Scene 完成输入处理，产生拥有自身字符串和值的 ControlEdit；SDK 将其与完整
   UiLoadId（owner、generation）一起排队。队列不保存 Scene 指针或 C ABI 借用数据。
2. 手势回调完成后，值事件投递前重新核实当前 UI、节点、可用性、action 和值修订。
   已替换界面的事件不能落到新界面的同号节点；更新后的旧值提议也不再交给业务。
3. 调用前复制接收函数，先完成队列状态变更再执行回调。回调内 SetBinding、关闭前端、
   换 UI 或请求再次投递均允许；值回调不递归进入，当前回调返回后继续处理。
4. 每次回调后再次核实已投递的预览。业务替换权威值为 Superseded；界面退休、节点
   不可用或 action 更换为 Unavailable。生成的 Cancel 保留原 interaction、revision、
   before 和 action，value 回到原 before，不把新业务值冒充旧交互的回退值。
5. Commit 提议在调用业务前已结束该预览，业务同步写回不会额外产生 Cancel。
   没有被业务观察过的 Preview 被丢弃时，不制造业务取消事件。

Scene 提供 `ControlEditInvalidation` 返回具体原因，原 `IsCurrentControlEdit` 是其 Boolean
包装。投递器只负责业务通知；Slider 的 Scene 输入控制器还须结束自己的捕获和
本地呈现 session，不能把发出 Cancel 当作已清理输入状态。

### 终结、接收者与异常

- 相同交互、相同预览值不重复通知；新交互接管同一节点时，先取消已观察的旧预览。
  同一交互不得改变 action/revision/before。生产者仍负责单调序号及只产生一个终结事件，
  投递器不保存无限期的已完成事务历史，也不充当不可信协议输入的去重服务。
- 显式 Escape/FocusLost 等 Cancel 仅通知已观察相同 UI/节点/interaction 的接收者。
  终结后清除记录；重复 Cancel 或未观察的 Cancel 不产生回调。
- `OnControlValue({})` 或替换处理函数是**接收者撤销边界**：清空待投递与预览记录，
  不调用旧接收者，不把旧预览的终结事件转交新接收者。模块拆卸需先撤销回调，再销毁
  业务对象；当前 Host 已按此顺序执行。业务在自己的 disconnected/销毁阶段释放预览资源。
- 回调可改变应用状态，但不能在仍执行的成员函数内销毁 ClientApplication 对象本身。
  投递与状态检查均在所有者线程，不引入线程锁、周期 timer 或新的渲染请求。
- 队列上限 1024，活动预览上限 256，单次投递轮次上限 4096。超限显式失败；回调异常
  清空剩余队列和预览记录并传播至调用者，事件泵沿用现有前端失败收尾。

### 验证范围与下一步

`control_value_delivery_test` 验证重入、界面替换、权威值更新、关闭、撤销/更换回调、
回调异常后的队列清理、数值事务预览与取消、拥有值的复制、队列上限，以及真实
Checkbox → 投递器 → C 业务模块 → binding 确认链路。

第 11 节已接入 Slider 的数值域、成功提交快照几何、鼠标捕获、键盘连续预览和主题
配方，并验证 Scene 输入取消与本节通知取消共同闭合。当前未部署到运行中的 VNC。

## 11. Slider：水平连续值控件（本轮源码）

### 数值与业务契约

`Slider(action, value, minimum, maximum, step)` 已接入 Scene → SDK → Host →
`on_control_value`。minimum/maximum 默认 0/1，允许 -10¹²..10¹² 且 minimum < maximum；
step 默认 0 表示连续，正数按 minimum 为起点量化，两个端点始终可达。
minimum/maximum/step 在本版必须是数字字面量，准备后不可绑定、主题化或 SetProperty
修改；改变范围需安装新组件。value 可绑定，必须是域内有限 Number，拒绝越界/错类型
的整次绑定投影，不把 Progress 的 0..1 限制改成任意数值。

业务权威值可以不在 step 网格上，before 保留其原值；只有用户提议会量化。初始绑定
尚未发布时使用 minimum。交互需要非空 action；不会产生 on_action。Preview 仅更新
本地滑块与预览通知，不写权威 binding；Commit 仍需业务 set_binding 确认。没有确认时
恢复权威值。业务可用独立文本 binding 显示预览读数，不能在每次 Preview 把同一 value
当作已确认值写回，否则会以 Superseded 结束正在进行的交互。

### 输入与生命周期

- 鼠标主键在轨道按下即定位并开始预览，捕获后可拖出矩形，数值钳制到端点；释放
  只提交一次。非主键不修改值。离开 surface 后没有新 motion 的释放提交最后预览值，
  不使用适配器残留位置重新计算。Esc 取消；PointerCancel/隐藏/禁用为 Unavailable。
- Tab/Shift+Tab 使用一个停靠点；左右/上下每步 ±step，连续域每步为范围的 1%。
  Home/End 到两端，PageUp/PageDown 为十步。按下与重复键产生去重 Preview，匹配释放
  发一次 Commit；迟到 repeat/release 不能复活取消的操作。切换方向键会取消前一操作。
- 换焦点、Tab 离开、seat 失焦为 FocusLost；权威 value 修订、包括 A→B→A、Preflight
  更新为 Superseded。鼠标/键盘接管同一 Slider 时先取消旧会话。触控不在本次范围内。
- 轨道几何来自成功提交的 InputSnapshot，在捕获期间冻结。slider_track 的 x/y 表示
  行程左端与垂直中心，width 为水平行程、height 为可用垂直空间。新提交几何改变轨道，或
  目标不再可交互时取消旧捕获；不会用尚未显示的布局让滑块突然跳位。
- 事务收尾仅标记 retained stream，不在 noexcept 提交阶段分配事件字符串。
  `Scene::TakeControlEvents()` 提取拥有数据的 Slider 事件；SDK 在交互处理、绑定更新、
  UI 替换和关闭边界收集它们，再走第 10 节的投递器。直接 Scene 用户也必须主动 drain。
  Checkbox/Radio/Segment 的离散结果仍在 InteractionResult.control_edit。

### 外观接口与设计图对应

Slider 是语义命中容器，直接子级均为 Visual。必须包含三个不重复的叶子 Visual：
`sliderPart: "track" / "fill" / "thumb"`。角色必须是固定字面量，不能绑定、修改或
作为区域独立加载。额外无角色 Visual 可绘制焦点框。角色只指定几何用途，不指定颜色
或阴影；仍使用通用 Visual、状态配方和普通 DisplayList 绘制。

- 三个部件显式声明 height，thumb 还声明 width。框架负责部件 x/y 和 track/fill 宽度；
  不在这些叶子上用 anchor/inset/translate 来改变轨道语义。整个轨道由 Slider.paddingX
  和 thumb 宽度留出端点空间，所有部件垂直居中，滑块不会伸出左右边缘。
- 对照控件设计图：默认轨道 4px、滑块 16px、命中高度 44px；track 低对比、fill 主题
  蓝色，深色配浅滑块、浅色配深滑块。标题/名称和数值保持可读，不用大图标替代内容。
- 四材质复用 `indicator_height`（4）、`space_lg`（16）、`control_state_radius`（Square
  为 0，其余为 8）、`controlDisabled`（轨道）和 `text`（滑块）。这些角色的尺寸与
  明暗颜色已经覆盖本配方，不新增重复 token 或扩大主题协议条目上限。
  Focus 使用独立边框，hover/pressed/disabled 由 Visual.state 定义，不自动改变全局透明度。
- 运动只修改帧快照中三个部件的几何，不更改父布局与命中范围。数值预览使 Paint 失效，
  不单独触发 Layout；相同量化值不反复通知或重绘。应用主动改文本 binding 仍遵循现有
  文本布局失效规则，不能据此声称整个含读数页面从不布局。
- Progress 保持只读、没有滑块。不要用 Progress 伪装 Slider，也不在此阶段加入跳动、
  弹性或常驻刷新动画。

```prism
Slider(action: "volume", value: $volume, minimum: 0, maximum: 100, step: 1,
       height: "@control_hit_height", paddingX: "@space_xs") {
    Visual(sliderPart: "track", height: "@indicator_height",
           cornerRadius: 2, background: "@controlDisabled")
    Visual(sliderPart: "fill", height: "@indicator_height",
           cornerRadius: 2, background: "@controlPrimary")
    Visual(sliderPart: "thumb", width: "@space_lg", height: "@space_lg",
           cornerRadius: "@control_state_radius", background: "@text")
}
```

完整标题、数值、禁用与焦点布局见
[独立 Slider 示例](../tests/fixtures/interface-system/sliders.prism)，业务事件示例见
[独立 C 模块](../tests/fixtures/slider_module.c)。这些样例和测试不进入生产安装包。
本版仅水平、左到右、单滑块；垂直/RTL/双滑块范围与滚动容器尚未实现。
