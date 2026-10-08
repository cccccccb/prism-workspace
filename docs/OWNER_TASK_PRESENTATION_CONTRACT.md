# Owner 任务呈现生命周期契约

阶段：Interface System **7a1 / 7a2 / 7b1 / 7b2**。对应设计图 05「翻起的是任务面板，退后的是工作
上下文」。7a1建立零时长生命周期与真实采用关联；7a2接入独立只读绘制值的捕获和保留。

## 1. 范围与当前状态

本契约的7a1与7a2已在工作区源码接入。第11节保留7a1验证记录；7a2验证与证据以
执行计划对应记录为准。不能仅凭文档或源码存在，就假定正式VNC会话或旧Host已具备
这些能力；当前正式安装版本仍为0.1.0-26，本轮没有部署。

7a1 增加 SDK 内部的 typed 呈现状态和帧关联，不增加普通业务模块 C ABI、不增加 DSL
节点或属性，也不授予新的文件、窗口布局或系统会话权限。业务继续使用已有 owner task
请求、取消和一次结果回调。SDK 的 `OwnerTaskPresentation()` 查询用于诊断，不能作为
普通业务的新能力入口、结果来源或输入授权。

7b2源码已将标准provider的固定几何透明度开合接入统一采样与实际采用；Opening
期间阻止控件操作，业务Ready仍独立推进。Closing立即退休旧输入，受支持来源可继续
绘制最后实际采用的独立片段；即时来源则只绘制最新正文。phase本身不能证明面板
仍绘制、可交互或已被compositor呈现。时序、样本和降级规则见
[任务开合运动契约](OWNER_TASK_MOTION_CONTRACT.md)。owner正文退后、截图缓存、
透视翻起、通用资源lease及固定帧率tick仍未实现；真实透视能力进入7c。

关联的已有契约：

- [任务业务状态与局部模态](OWNER_TASK_AND_MODAL_SCOPE.md)。
- [共享 provider 与业务 ABI](OWNER_TASK_PROVIDER_CONTRACT.md)。
- [文件任务 provider](FILE_TASK_PROVIDER_CONTRACT.md)。
- [渲染线程与不可变帧](CLIENT_RENDER_THREAD_MIGRATION.md)。
- [界面系统执行计划](INTERFACE_SYSTEM_PLAN.md)。

## 2. 业务、输入与呈现保持独立

| 层 | 状态与职责 | 不承担的职责 |
| --- | --- | --- |
| `TaskSession` | Preparing、Ready、Working；产生 Success、Cancelled 或 Failed terminal | 不决定动画已结束、画面已显示或系统权限 |
| `Scene` owner modal | 当前作用域、epoch、捕获取消、焦点与输入快照校验 | 不等待退场动画才退休输入，不自动隐藏应用自定义区域 |
| SDK 任务呈现 | Opening、Open、Closing、Closed；关联实际采用的帧 | 不延迟业务 terminal，不改变 TaskPhase，不证明 compositor 已呈现 |
| Host provider | typed 请求、共享面板、文件工作及结果交付 | 不在模块请求调用栈内回调，不以动画时长决定业务结果 |

例如，文件枚举期间可以同时是 `TaskPhase::Working` 与 `Open`。目录或列表语义刷新会
使任务重新 `Preparing`，同一面板仍可保持 `Open`。不能以 `Open` 替代 Ready 检查，
也不能从 Ready 推断呈现终值已被采用。

Ready 沿用现有门禁：当前 Scene 实际接受匹配的输入快照后，Preparing 才推进 Ready。
本步不把动画时长、固定等待或新的呈现状态加入这个判据。原输入快照的作用域、几何
和节点身份校验继续独立生效。

## 3. Typed 身份与投影

以下为**协议示意**，用于解释关联字段；不是可直接复制的 DSL 或业务 ABI。
最终 C++ 字段与签名以匹配本步的 SDK 头文件为准。

```cpp
enum class TaskPresentationPhase { Opening, Open, Closing, Closed };
enum class TaskPresentationEndpoint { Open, Closed };
enum class TaskPresentationSampleKind { Intermediate, Terminal };

struct TaskPresentationIdentity {
    TaskIdentity task;
    UiLoadId ui;
    std::uint64_t cycle;
};

struct TaskPresentationStamp {
    TaskPresentationIdentity identity;
    std::uint64_t projection;
    TaskPresentationEndpoint endpoint;
    TaskPresentationBinding binding;
    std::uint64_t frame_sequence;
    TaskPresentationSampleKind sample_kind;
};
```

字段语义：

| 字段 | 约束与来源 |
| --- | --- |
| `task` | 已成功建立的 SDK `TaskIdentity`，不是普通模块自报的 request ID |
| `ui` | 实际安装的 `UiLoadId`，包含 frontend owner 与 UI generation |
| `cycle` | SDK 分配的非零、单调、不复用呈现周期；新任务建立新周期，耗尽时拒绝复用 |
| `projection` | 同一周期内非零、单调投影代际；旧投影不得确认新投影 |
| `endpoint` | 本段呈现的目标Open或Closed，不是业务Success/Cancelled；仅Terminal证明达到 |
| `sample_kind` | Intermediate只更新采用证据，Terminal才完成phase；缺省Terminal保留SDK即时行为 |
| `binding` | 本帧真实输入、UI、几何、已接受主题和 surface 配置的精确关联 |
| `frame_sequence` | 本次不可变 `FramePacket` 的非零 sequence，不能由排队候选推定采用 |

`TaskPresentationBinding` 至少关联当前 Scene/input 身份与版本、owner modal epoch、
已接受主题 generation、configure count、buffer size 和 scale。SDK 从实际 Scene、
已接受主题与窗口配置构造该值；业务不提供它。

以下变化使投影失效并分配新代际：作用域 epoch、输入几何、已接受主题身份、configure、
buffer size 或 scale 变化。目录/列表等语义刷新保留 task 与 cycle；若原状态是 Open，
继续保持 Open，等待新投影采用，不重新转为 Opening。普通正文状态更新不因捕获了新
frame sequence 就重播任务入场。

FramePacket 中的任务 stamp 是可选元数据。没有 owner 任务呈现关联的普通帧不添加
stamp。所有 stamp 与同一个包的输入快照、像素或已验证相同的像素基线一同准备，不能
从旁路 callback 或另一个包拼装关联回执。

## 4. 状态推进与真实采用门禁

| 原状态 | 事件 | 新状态 |
| --- | --- | --- |
| Closed / 尚无周期 | 成功开始任务及模态作用域 | Opening，新 cycle |
| Opening | 当前 Open 终值帧实际采用且全部关联匹配 | Open |
| Opening / Closing | 当前Intermediate帧实际采用且关联匹配 | 保持phase，记录采用sequence |
| Opening / Open | 同任务 Refresh 或活动投影变化 | 保留原 phase 和 cycle，更新 projection |
| Opening / Open | 完成、取消或失败，且该 cycle 曾有效采用 | Closing；业务与输入已退休 |
| Opening | 尚未有效采用便取消或失败 | Closed，无需显示从未采用的面板 |
| Closing | 当前 Closed 终值帧实际采用且全部关联匹配 | Closed |
| Closing | 新任务开始 | 退休旧周期，开始新周期 Opening |
| 任意状态 | owner 退出、前端失败、UI 替换 | Interrupt，直接 Closed |
| Closing | 主题或 surface 几何变化 | Interrupt，直接 Closed |

只有生产 `HandleSubmitted` 消费真实 worker 事件的采用进度点可推进终值。必须同时满足：

1. `metadata_prepared` 为真，事件携带实际消费的不可变 frame。
2. frame 的 UI、sequence 与事件一致，stamp 的 sequence 与所属 frame 一致。
3. stamp 的 task、UI、cycle、projection、endpoint与sample_kind匹配当前呈现关联及
   已发布序列区间；中间帧不能改标终值。中间采用不推进Open/Closed。
4. 当前 Scene 已接受该 frame 的输入快照；Scene/input、UI、几何与 modal epoch 匹配。
5. stamp 的主题身份与当前已接受主题匹配，configure、buffer、scale 与当前 surface
   配置一致，不能由旧配置的采用回执确认新投影。
6. Open 终值仍关联活动任务作用域；Closed 终值关联的是退休后当前输入域，不能继续
   借用旧模态 token 或旧 provider 节点身份。
7. 7a2同时核对frame的resource epoch与当前资源表；资源变化后的旧回执不能登记旧
   绘制值。新包仍沿原采用流程推进，不改变业务TaskReady门禁。

Scene接受过旧snapshot不等于当前live几何仍相同。采用时遇未解析Layout直接等待后续
发布；已解析时捕获当前输入，核对scene/version/root/viewport/epoch。布局binding更新
后新包尚未发布的窗口内，旧回执也不能推进开合终值。此校验只管呈现，不改TaskReady。

成功 Pixels、成功 State 与 **checked-identical None** 都可参与关联。后两者由 worker
验证已有像素基线与当前包一致；普通返回 None、排队成功或无 frame 的事件不能充当采用。
不为零时长生命周期强制额外 Swap，不把 worker 采用称为 compositor Presented。
若需要实际显示时间，继续使用独立的 presentation feedback 契约。

逻辑示意同样是**伪代码**，不引入新的公开函数承诺：

```cpp
void HandleSubmitted(const SubmittedFrameEvent &event)
{
    ValidateActualConsumedPacket(event);
    ApplyCurrentInputSnapshot(event);
    AdoptOwnerTaskInputThroughExistingGate(event);

    if (event.metadata_prepared && MatchesCurrentPresentation(event) &&
        MatchesCurrentSceneInputUiGeometryAndTheme(event)) {
        AdoptTaskPresentationEndpoint(event.frame->task_presentation);
    }
}
```

任务已退休后的晚 Open 回执不得复活它；旧 task、旧 UI、旧 cycle、旧 projection、旧
configure 或主题回执不得关闭或打开新任务。重复回执不产生第二次状态推进或业务结果。

## 5. 完成、取消与回调重入

成功、取消和失败继续使用现有 TaskSession terminal。关闭呈现不是新的业务 terminal，
也不是终值结果被消费的前提。

必须保持以下顺序：

1. 接受匹配任务的第一个业务终态。
2. `EndOwnerModal` 撤销当前作用域与捕获，改变输入 epoch，并恢复仍有效的 owner 焦点。
3. 共享 provider 清除可见 binding 和请求数据；旧输入立即按现有身份检查拒绝。
4. 呈现进入 Closing 或直接 Closed，发布退休后当前输入域上的关闭终值包。
5. Host 取走 terminal、退休自己的 pending binding，再调用业务完成回调。
6. 后续真实采用推进 Closing → Closed，不再次触发业务结果或焦点恢复。

「立即拒绝」指 SDK 已退休旧输入身份；不表示网络或 compositor 已经显示关闭后的画面。
新正文输入仍需对应当前可接受的输入快照，不能把旧面板上的 release 当作正文点击。

```text
任务 A：Begin → Opening → 采用 Open → Open
                                ↓ 完成/取消
       terminal → EndModal / 隐藏共享面板 → Closing
              → Host 先退休 pending → 回调 A
              → 采用 Closed → Closed

回调重入：回调 A → Begin B → 新 cycle Opening
                    ↓
             A 的晚回执只被忽略，不能关闭 B
```

Callback 可以立即开始任务 B，不等待 A 的 Closing。任何在 Publish、输入取消或业务
交付期间发生的重入，都只能清理原 identity/cycle，不能用无条件 reset 撤销 successor。
同一 frontend 永久退出后不再开始新周期，不等待渲染回执才清理任务。

## 6. 共享 provider 与通用区域的区别

标准 Confirmation/File provider 的可见 binding 由 SDK 管理，业务终态后先隐藏面板，
因此 Closed stamp 可准确关联面板撤下后的当前输入与像素基线。

通用 `BeginOwnerTask(region)` 只建立局部模态，不自动隐藏应用区域。结束任务后，该
region 可以继续作为普通内容显示。此时 Closed 表示**这次任务呈现关联已经退休**，
不能报告「应用区域已从屏幕消失」，也不能偷偷修改业务的 visible binding。

7a1 Closing 不保留旧节点、旧输入快照或旧模态作用域，也没有退场视觉。整窗照常产生
最新正文帧；不得用先前的整份 FramePacket、DisplayList 或截图冻结应用正文来模拟退出。

## 7. 中断与刷新规则

- **Refresh**：换新的 modal epoch / 输入投影，保留 task 与 cycle。旧按下、释放和采用
  回执不能按新列表或新确认内容解释；Preparing → Ready 仍沿原 TaskSession 门禁。
- **活动任务尺寸变化**：保留业务与呈现周期，失效投影并重新关联真实配置。继续执行
  provider 的可读布局校验；不足最小尺寸或主操作无法完整显示时，沿既有失败路径收回。
- **活动任务主题变化**：以已接受主题身份失效投影，保持原 phase。被拒绝的主题候选
  不能成为 binding；不能因主题事务成功就提前承认新帧已采用。
- **Closing 尺寸/主题变化**：直接 Interrupt → Closed，不等待旧配置回执，不恢复任务。
- **UI 替换**：旧周期立即退休，旧 UI 的所有迟到回执拒绝；业务取消原因沿 UiReplaced。
- **owner 退出或前端失败**：直接退休，清理关联，不等待 Closed 回执，不再交付迟到结果。
- **快速重复关闭**：第一次业务终态获胜；后续事件不创建新的 Closing 或第二个 terminal。
- **新任务替代 Closing**：旧周期立即退休。新任务只能使用自己的作用域和投影，不能
  继承 A 的采用状态或旧焦点记录。

若关联代际耗尽，不能回绕或复用旧值。实现必须沿现有失败/退休路径收敛，不留下可接受
输入的任务却再也无法关联呈现的半完成状态。

## 8. 7a2：独立只读任务绘制片段

### 8.1 捕获与实际采用

生产SDK在最终活动任务帧准备后调用`Scene::CaptureTaskPaint`，从已解析RenderTree
按任务根导出独立命令值，不再Build或解析新的布局。SDK只为当前UI的标准Confirmation /
File provider生成`FramePacket::task_paint_candidate`；普通`BeginOwnerTask(region)`
不自动获得片段，也不因此隐藏应用region。

候选不是已采用证据。只有生产`HandleSubmitted`通过第4节全部门禁、当前Open目标
stamp实际采用成功后，SDK才把同包候选登记为唯一权威缓存`owner_task_paint`。7b2允许
Intermediate候选采用，缓存仍是未乘动画alpha的原始值，实际采用reveal独立记录。
新采用帧若没有可用候选，清除旧缓存；不能回放较早的可用面板冒充最新采用画面。
尚未采用的Refresh不替换权威缓存，同cycle此前最后有效采用的片段仍可用于关闭关联。

`TaskPaintFragment`拥有任务子树的绘制命令，包括必要祖先clip/transform/opacity、
实际shaped glyphs、图标、边框与内外阴影，命令栈保持平衡。祖先正文与兄弟节点不被
复制；不借用活Node/Scene、不保留旧整窗DisplayList或FramePacket，不按整窗command
index切片。源关联`TaskPaintSource`保存task/UI/cycle/projection/sequence、configure
count、buffer size、scale、theme generation及resource epoch；不带业务callback、动作、
输入区域、旧输入身份或可复用modal token。

### 8.2 关闭、失效与绘制组合

关闭先按第5节退休输入并收回共享provider，`ClosingOwnerTaskPaint`仅选择同cycle
最后实际采用且环境仍匹配的独立值，不采集当前Scene的未采用内容。Refresh允许源
projection/sequence早于当前投影；task/UI/cycle及配置、尺寸、scale、主题和资源epoch
必须匹配。配置或主题失效、新任务建立、UI替换、owner退出、前端失败与作用域失效
清除缓存。旧回执不能登记旧片段或清理successor。

7a2生产关闭为零时长；7b2对受支持来源组合最新正文和最后实际采用的独立值，从
实际采用reveal退出，不使用更晚计算或排队的值。Closed终值包仍只包含最新正文与
当前输入域，不添加旧面板命令。Closing Intermediate保留缓存，匹配Closed终值实际采用后释放。不能
把“缓存已保留”写成“退场动画已显示”，也不能让仍画着面板的包确认Closed终值。
捕获失败与安全降级只省略片段，不推迟输入退休、terminal或业务完成回调。

`ComposeTaskPaint(body, paint, opacity)`提供最新正文与独立片段的整体透明度组合，
7b2受支持退场链使用该接口；即时路径不重放旧面板。完整DisplayList继续进入现有
旧/新绘制损伤比较，撤下或移动需覆盖旧、新范围及阴影。该接口不提供计时、退场期限、
输入授权或业务ABI；运动由独立生命周期适配器管理，不冻结整窗或延后业务结果。

### 8.3 首版资源范围

向量与glyph值可保留；glyph仅使用本应用固定字体，依托应用字体生命期及上述环境
门禁。它不等于通用字体lease。图片仍不支持；复制DrawImage、image_uses或resource
epoch不能替代图片版本注册、lease、释放顺序和退出回放资源管理。

任务子树及祖先的backdrop依赖均拒绝；当前Scene与解析RenderTree同时核对，避免
Composite-only模糊变化借用旧候选。native popup已采用时也省略导出，避免跨target
片段不完整。采用后的不支持候选清除原有缓存，降级路径继续即时关闭。

这包括保守拒绝祖先window材料的模糊：当前Glass的window有blur，即使任务card本身
blur为0，也不会导出片段。不能移除正常窗口材质来绕过门禁，或宣称Glass非零退场已
实现。图片/backdrop lease、跨target关联与泛用任务区域导出仍是后续独立能力。

## 9. 7b / 7c 与图 05 的关系

7b 使用主题或独立 `.prism` 声明面板开合、owner 退后/恢复，并复用单调 Clock、Pump
和不可变帧。instant / reduced motion 保留相同业务结果、输入退休和一次采用关联，
不靠业务 schedule_tick、固定等待或帧数计算时长。本步没有增加 reduced-motion 偏好
API，也没有让现有 `panel.visibility` 自动具备 mount/unmount 或退出延迟能力。

现有 Visual 的 2D 绘制变换不自动改变外层命中几何。因此 7b 不能把整面板移动与未对齐
按钮输入同时开放。需要选择固定命中、仅装饰层运动，或明确准备期及终姿势采用后才
接受对应输入；必须单独验证取消、Esc 与最后一帧。Closing 片段已经没有输入，不需要
借用活动任务命中域。

图 05 的翻起需要真实 projective 变换、裁剪、逆映射、损伤与后端契约，进入 7c。平移、
缩放或透明度只能报告为 2D 过渡，不能标记透视翻起已完成。稳定界面保持平直可读，
实际帧时间与正文持续更新需要对应验证。

## 10. 本步验收边界

至少验证首次采用、尚未采用便取消、Closing 终值采用、State/checked-identical None、
错误或重复 stamp、旧 UI/cycle/projection/configure/theme、refresh 不重播入场、回调
立即开始 successor、owner 退出和尺寸/主题中断。共享面板关闭后的当前输入域与普通
正文更新须继续有效；通用任务结束不得自动隐藏应用 region。

7a2额外验证祖先scope与命令栈、独立轮廓/glyph/阴影值、Composite-only模糊降级、
只按真实采用登记候选、刷新未采用时选择旧值、最新采用不支持时清空缓存、关闭后
正文更新与输入先退休、资源变化后的首次旧回执、新cycle隔离及所有退出清理。
独立值组合用完整/局部重放对照验证正文变化、移动阴影与撤下损伤；这不等于生产
非零退场已经启用。

纯状态测试不能证明生产 HandleSubmitted 门禁已经接入；生产 adopted receipt 不能
代替 compositor 显示、用户视觉或非零动效验收。实现、验证、打包和正式部署分别报告。

## 11. 7a1源码验证结果（2026-10-08）

前序6b1提交为`786cec8`。7a1生产构建通过，相关CTest最终**34/34**通过，含纯核心
11组及SDK适配11组具名用例；不是全仓库测试。SDK用实际共享DSL、字体和不可变帧，
控制metadata回执验证None/State、旧/缺失/错误stamp、Refresh、Working、即时输入退休、
回调重入、UI替换/退出、主题/尺寸中断及新布局未Publish时拒绝旧几何回执。这类回执
夹具不是原生提交证据。

独立Wayland/V3D门槛**23/23**通过：Provider14、Notepad真实文件/关闭4、Tooltip5。
Provider/Notepad在Ready检查当前Open呈现与实际采用帧identity/projection/binding/
sequence匹配；恢复后检查Closed及无任务/作用域，并校验仍携带stamp的关闭回执。
请求若在provider启动前取消，则允许从未建立呈现周期，但仍须实际采用正常输入且
没有pending、任务或模态token。临时WM全部回收；Square Light、鼠标/键盘，不包含
全主题GPU视觉、触摸屏、FPS或非零动效验收。正式VNC仍为v26。

初轮构建缺测试链接依赖，已补齐；初轮原生探针错误地要求未创建面板的提前取消请求
必须有Closed状态，已修正检查范围。两轮相关CTest各有同一control_value_abi_test加载
超预算失败，第二轮记录load_ns=6398774745、create_ns=0。加载计时包含dlopen/dlsym/
entry/ABI复制，不能定位为业务入口耗时。未改代码单项、补诊断后的既有两项及最终34项
通过；增加了仅失败时输出的线程CPU/缺页/上下文切换诊断，没有预热fixture或改变预算。
初始原因未确定，不能把复测通过宣称为环境根因已修复。相关原始日志保留。

证据位于`dist/validation/interface-system-step7a1/`；安装、archive及边界审计与规范
检查另见执行计划。验证后曾发现旧独立构建对象重定位损坏，已从当前有效archive成员
恢复并复核，未改源码、archive或已验证程序；损坏根因仍未确定，不能宣称环境根治。
本步没有生成deb、部署、添加任务运动或保留退出视觉。
