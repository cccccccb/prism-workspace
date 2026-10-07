# Owner任务Provider、业务ABI与共享DSL面板

日期：2026-10-07。界面系统5b，**源码实现与自动验证完成**。本规范承接
[5a任务会话与局部模态](OWNER_TASK_AND_MODAL_SCOPE.md)，定义首个真实Confirmation
provider及业务接入边界。构建、自动测试与实机视觉结果由执行计划分别记录；正式v26
没有本阶段接口，不能根据新文档推断已安装环境支持它们。

## 1. 分层与阶段范围

| 层 | 职责 | 不持有的内容 |
| --- | --- | --- |
| 业务模块 | 请求语义、标题/说明/选择、接受终态后执行业务 | Scene、Wayland、Skia、owner权限凭证 |
| ModuleSession | C ABI校验、复制输入、关联ID、一次终态及回调生命期 | 面板布局和绘制对象 |
| Host任务控制器 | 绑定实际前端owner、stage请求、推进SDK会话、驱动provider | WM布局策略和应用文件业务 |
| Confirmation provider | 将受信输入选择转换为typed选择结果 | 文件系统路径和业务状态修改权限 |
| 共享DSL面板 | 文本、布局、材质、状态反馈、可滚动内容 | 任务完成判定和owner授权 |
| SDK/Scene | 真实输入采用、局部作用域、捕获取消、焦点及生命周期 | 应用确认含义和具体绘制分支 |
| WM | 通用窗口与输入归属、合成、桌面布局 | 任务标题、选项及应用DSL解析 |

5b建立Confirmation。5c扩展系统文件模型、目录异步加载、Open/Save/SelectDirectory和
覆盖恢复，详见[文件任务规范](FILE_TASK_PROVIDER_CONTRACT.md)；Notepad真实接入和
异步关闭续接已在5d源码实现，见[业务与Close](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)。
不要返回演示路径、把普通
action字符串当作文件服务，或在第三方包内复制一个系统选择器。

Confirmation本身只返回用户选择。`Success`表示用户选择已被接受，不表示保存、删除
或后台业务已经执行成功；后续业务由模块按自身状态与异步工作契约处理。

## 2. C ABI兼容与能力

独立头文件为`prism/contracts/app_task.h`。`PrismTaskStringViewV1`包含
`const char *data`和`size_t size`，是借用UTF-8字节切片，不要求NUL结尾。
所有对象只用于进程内调用，不能把含指针的结构直接序列化为IPC。

`PRISM_APP_ABI_V1`与入口符号保持不变，仅向已有结构末尾追加完整字段。

| `PrismHostApiV1`尾字段 | 返回及含义 |
| --- | --- |
| `uint32_t task_capabilities(void *context)` | 当前支持位；0表示不可用 |
| `uint64_t request_task(void *context, const PrismTaskRequestV1 *)` | 非零关联ID表示请求已接受；0拒绝 |
| `int32_t cancel_task(void *context, uint64_t request_id)` | 0表示匹配请求的取消已接受；非零拒绝 |

`PrismAppModuleV1`尾字段为
`void on_task_completed(void *instance, const PrismTaskResultV1 *)`。
Host loader按`offsetof(field) + sizeof(field)`逐个检查后复制完整可选字段；缺失或只
广告了部分指针的旧模块仍将该回调视为null，不读取尾部不可访问字节。业务同样必须
先检查Host的`struct_size`，然后检查相应函数指针，不能仅根据ABI版本访问新尾字段。

目前唯一能力位为`PRISM_TASK_CAP_CONFIRMATION_V1 = 1u << 0`。
能力需要模块具备完成回调、Host任务sinks已配置、当前Master包含受信共享面板并且
前端存活、逻辑viewport至少240×180。该尺寸是本provider的新请求能力门槛，不是
WM承诺的最小窗口尺寸，也不保证任意文案一定可呈现。它不是请求成功的保证：正在
准备另一任务、owner不可用或资源准备失败仍
会拒绝。`create()`可查询能力，但不能提交请求；请求须在业务实例建立后发起。
全部Host API和完成回调在owner线程执行；scheduler worker不能访问它们。

## 3. 请求与选项

`PrismTaskRequestV1`包含：

| 字段 | 约束 |
| --- | --- |
| `uint32_t struct_size` | 至少覆盖当前V1全部字段 |
| `uint32_t kind` | `PRISM_TASK_CONFIRMATION_V1 = 1` |
| `PrismTaskStringViewV1 title` | 非空，最多256字节，单行 |
| `PrismTaskStringViewV1 message` | 可空，最多2048字节，允许显式LF/TAB |
| `const PrismTaskChoiceV1 *choices` | 指向有效数组，仅调用期间借用 |
| `size_t choices_size` | 1或2 |

每个`PrismTaskChoiceV1`包含完整`struct_size`、非零`uint32_t id`、非空UTF-8
`label`及`uint32_t role`。同一请求内ID必须唯一，label最多96字节且为单行。

| role | 数值 | 呈现和语义 |
| --- | --- | --- |
| `PRISM_TASK_CHOICE_SECONDARY_V1` | 0 | 普通备选动作 |
| `PRISM_TASK_CHOICE_PRIMARY_V1` | 1 | 主操作，使用主题主按钮配色 |
| `PRISM_TASK_CHOICE_DESTRUCTIVE_V1` | 2 | 不可轻易恢复的业务操作，提供清楚文字与辅助图标 |

最多一个Primary及一个Destructive，至少存在其中之一。允许一个Primary加一个
Destructive，例如后续保存确认中的Save与Discard。**Cancel由系统始终提供**，
不占这1—2个业务选择，也不能将取消编码成一个成功choice ID。

标题、正文与所有label合计最多4096字节。请求先校验切片边界和长度，再复制为
`contracts::OwnerTaskRequest`及`OwnerTaskChoice`，最后进行完整值校验。输入包含
未知kind/role、零或重复ID、非法角色组合、长度越界或不完整结构时原子拒绝。

UTF-8必须是完整Unicode scalar编码；拒绝过长编码、非法continuation、截断、代理
区和超过U+10FFFF的值。NUL、C0/C1控制字符拒绝，message及失败diagnostic仅例外
允许LF和TAB；U+2028/U+2029同样拒绝，换行须显式使用LF。输入不按JSON或DSL解析，
不展开action或路径。UTF-8合法不等于当前字体已具备全部字形或输入法能力。

## 4. 关联身份、stage与生命周期

模块不能提交owner数字、Scene节点、seat凭证或自选任务ID。ModuleSession为accepted
候选发放独立非零`uint64_t request_id`，单调增加、不回绕、不复用，包括曾经stage
失败而消费的ID。此ID与business work的`task_id`互相独立。

Host通过实际ModuleSession、ClientApplication与已安装Master绑定SDK的
`TaskIdentity`及作用域token。模块的request ID只是本实例关联，不是跨进程权限；
两个owner出现相同关联数字也不会让请求相互匹配。以后跨进程provider仍需验证真实
连接、窗口与实例归属，不能相信数字或面板action自带授权。

一个ModuleSession最多一个pending请求，直到其终态被取出。提交时先复制输入、记录
pending，再交给Host owning sink。sink只stage请求，不能在`request_task`调用内部
交付`on_task_completed`。owner Pump随后安装可见投影并建立SDK局部作用域。
接受ID仅表示该次请求已接受，不代表面板已经可交互、呈现或后台操作已经完成。

Preparing只有在worker实际采用匹配当前UI、Scene及`owner_modal_epoch`的输入快照
后进入Ready。队列成功、动画完成或像素是否变化不能替代采用身份。Confirmation
只有在该请求Ready及真实面板输入通过当前scope/快照门禁后才接受业务choice。
读取当前值并虚构action、旧提交几何或来自正文的字符串均不能绕过身份校验。

`cancel_task`只能操作本模块精确匹配的pending。Host先接受取消时，保持ID忙，直到
`Cancelled / User`（UserCancelled）终态交付；随后发生的provider失败、Esc、UI替换
或迟到成功均不能覆盖该取消。重复取消可安全返回已接受。SDK若已先接受业务选择、
失败或取消终态，后来的`cancel_task`明确拒绝并保留首个终态。该先后关系以实际接受
为准，不能以回调尚未交付推断任务仍可取消。`OwnerClosed`始终撤销请求并抑制业务
回调，即使Host此前已接受取消，也不再向关闭的owner交付UserCancelled。

Esc来自真实输入作用域，外点消费但不取消，切到其他owner保留请求；主题/resize
保持有效任务，只有后续准备确实失败或UI被替换时才进入对应终态。

创建或stage失败清理该次尚未交付的请求，拒绝不留下孤立Preparing。输入发布可能
同步产生旧控件/手势Cancel，但终态仍通过owner Pump延后处理。回滚只针对原始
identity，不能抹掉已接受终态或同步回调发起的下一请求。
SDK另保存单调的Confirmation适配generation与installed UI。请求发布过程中，
Cancel回调可能接受首个终态、关闭owner或同步开始下一任务；异常清理只有在原
generation仍匹配时才能隐藏原投影。不能因旧调用栈恢复就清掉新请求、跨UI改binding
或提前领取下一任务的终态；已接受的首个终态保持。代次达到上限后拒绝新请求。

## 5. 结果与一次交付

`PrismTaskResultV1`包含完整`struct_size`、`request_id`、`outcome`、`choice_id`、
`cancel_reason`、`failure_code`及借用`diagnostic`。C++ owning版本为
`contracts::OwnerTaskResult`。输入结果必须与当前pending精确匹配并通过以下校验：

| outcome | 数值 | 合法payload |
| --- | --- | --- |
| `PRISM_TASK_SUCCEEDED_V1` | 0 | 原请求已有的非零choice ID；无取消、失败或diagnostic |
| `PRISM_TASK_CANCELLED_V1` | 1 | choice为0，有效非None取消原因；无失败或diagnostic |
| `PRISM_TASK_FAILED_V1` | 2 | choice为0，取消None，有效非None失败分类；可选diagnostic |

取消原因：None=0、User=1、Escape=2、UiReplaced=3、Unavailable=4、FrontendFailed=5。
失败分类：None=0、PreparationFailed=1、OperationFailed=2、ProviderUnavailable=3。
diagnostic最多1024字节，按前述UTF-8规则验证，供必要诊断使用；不要用它作为业务
选项、身份或自动重试条件。

Host先移出已接受终态、收回匹配输入作用域与面板投影，再交给ModuleSession。
ModuleSession复制结果并**先移出pending再调用业务**。回调可以同步提交下一请求，
其身份必须全新。重复、迟到、未知choice、混合payload与非法enum全部拒绝，不改变
当前请求。借用视图在回调返回时失效，模块如需稍后使用必须复制为自己的值。

owner关闭顺序是：拒绝新请求，永久退休SDK owner，撤销controller与scope，丢弃
待交付结果，再StopWork、join工作并销毁模块。退出或前端失败后不调用业务完成
回调，不把旧终态投递给新实例。关闭单个任务不调用永久StopWork。launch/theme
连接断开与本地owner退出分别处理，不能错误退休仍运行的本地Confirmation。

5b保留同步`on_close_requested`/bool关闭接口。确认选择、保存完成和自动继续关闭
不是同一个事件；5d源码的[Close契约](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)
追加独立关闭身份与DEFER/complete_close，Host在回调后消费；5b任务本身不自动关窗。

## 6. 共享面板、保留命名与主题

Host在Preview建立、业务模块能力确认之后为Master准备受信共享面板。
共享资源是`resources/ui/owner-task-panel.prism`；应用包不能替换这个来源。
Preview仍是轻量启动内容，不注入任务面板、业务provider或额外系统资源。

目前只对普通Box/Card根、且根本身不是命名region的Master进行composition。
保留原应用树与root Popup/Menu，将隐藏、已挂载的普通任务region放在正文之上、
root Popup/Menu之前。其余根形式按原树运行，能力返回不可用，不能为了任务支持
静默改写布局语义。composition与应用component组合仍检查节点数及深度预算。

所有`__prism_task_`前缀名称保留给框架，包括binding、region、action、popupFor
及gesture action。Interface顶层binding与component ID同样检查，即使未被当前
UI引用也不能使用保留名。应用声明进入该空间时明确失败；业务`set_binding`也拒绝该
前缀。Host生成的可见性、标题、正文、角色和动作投影使用相同命名空间。业务只
通过typed任务ABI参与请求，不能用字符串修改面板或伪造系统选择。
内部任务binding保存在独立投影中，不并入应用业务状态表。整UI安装重置该投影，
deferred区域安装保留当前真实任务投影；外部binding候选、未使用声明或区域重挂载
不能覆盖可见性、选项或标题。绑定更新与组合入口都检查保留名，不能只封住某个
on_action字符串入口。

输入scope覆盖owner正文；面板从owner下缘展开，保留背后的工作上下文。不同owner
不会出现全局屏幕锁定。当前模态期间拒绝Popup及嵌套scope，焦点和Esc规则沿用5a。
面板的显示/隐藏是呈现状态，业务终态和scope关闭仍由任务控制器处理。

视觉参照图03与图04：简洁标题、可读说明、稳定底部动作区，避免重复应用名、大图标
和层层厚卡片。确认操作需要清楚文字，不能因“图标为主”删除危险操作的意义。
宽窗口横排动作，窄窗口重排，短窗口降低非关键装饰高度；核心动作与正文阅读能力
须保留。边距按当前14/16体系，组内4/8、分区12，不能在多层重复加入根留白。

主题引用现有`card`材质、`@controlSecondary`、`@controlOutline`、`@controlPrimary`、
`@controlPrimaryText`、`@controlPrimaryHover/Pressed`、`@controlFocus`、`@hover`及
`@accentSoft`，字号与行框引用`@font_title/body/caption`、`@line_title/body/caption`，
圆角引用`@control_state_radius`。透明度、模糊、边框、内外阴影和Square的方正形状
跟随当前主题，禁止将Glass、Light或某个demo的颜色写成业务常量。新增语义token
需先进入通用主题契约，不能在应用中凭空引用。

正文根据实际内容宽度和文本度量准备可读折行，显式换行保持；超出可用高度放入
ScrollView，动作区不随正文滚走。不能仅把长文本clip掉并称为自适应，也不能假定
Text已有新`wrap`属性。最窄、最矮BSP视口和主题切换后须重新核对折行及命中。
标题同样折行与滚动；准备以Unicode scalar边界分割，使用实际region宽度和字号，
每一输出行再用真实shaper复测有限、有效的宽高及可用宽度，不能只按字符数量估算。
TAB按普通空格呈现，业务输入中的LF保持；单个scalar无法适合宽度时明确失败，
不丢字、不拆成非法UTF-8或无限重试。

标题与正文的测量region位于ScrollView内时，`TextLayoutInRegion`返回的可用高度是
父ScrollView的实际viewport高度，而非随文本增长的内容region高度。每行的真实
文字高度必须适合该viewport；完整折行内容可高于viewport并通过滚动阅读。由此
区分可读行高与总内容高度，不为通用Text增加或假定`wrap`属性。

choice label必须完整适合当前实际标签盒宽高，不能通过clip、省略后直接接受请求，
也不强制所有标签缩小成caption。合法请求已stage但布局/标签无法适合时，投递typed
`PreparationFailed`，收回面板与scope，业务保留原状态。初始低于240×180时capability
不可用，`request_task`返回0；请求中的长文本、不同主题字号或后续resize仍可能使
已接受任务准备失败。滚动与焦点显露遵循现有通用契约。

框架先用Scene的ResolveLayout及TextLayoutInRegion准备测量，保留Paint待正式Build。
两者不采用输入、不Submit、不Render/Swap；准备后的文字和几何继续通过同一帧包
提交，Ready以精确采用身份判定。测量接口说明见
[Scene运行时](CLIENT_SCENE_RUNTIME.md#任务面板测量与布局准备2026-10-075b源码)。

5b只复用`control.feedback`的轻量状态反馈，不引入新的Timer或强制重绘。
图05翻起、owner退后、收回与中断/reduced motion作为后续命名主题运动接入；
它们仍不能代替Ready、终态或输入采用身份。

## 7. 业务模块示例

下例在既有业务实例的具名action处理中调用。`host`在该实例生命周期内有效，
`pending_task`由模块保存；`create()`只复制Host指针，不发起请求。

```c
static int HasConfirmation(const PrismHostApiV1 *host)
{
    if (!host || host->struct_size <
                     offsetof(PrismHostApiV1, cancel_task) + sizeof(host->cancel_task)) {
        return 0;
    }
    return host->task_capabilities && host->request_task && host->cancel_task &&
           (host->task_capabilities(host->context) & PRISM_TASK_CAP_CONFIRMATION_V1);
}

static uint64_t AskToRemoveDraft(const PrismHostApiV1 *host)
{
    if (!HasConfirmation(host)) {
        return 0;
    }

    const PrismTaskChoiceV1 choice = {
        sizeof(choice), 7, {"Remove draft", sizeof("Remove draft") - 1},
        PRISM_TASK_CHOICE_DESTRUCTIVE_V1
    };
    const PrismTaskRequestV1 request = {
        sizeof(request), PRISM_TASK_CONFIRMATION_V1,
        {"Remove this draft?", sizeof("Remove this draft?") - 1},
        {"The current in-memory draft will be removed.",
         sizeof("The current in-memory draft will be removed.") - 1},
        &choice, 1
    };
    return host->request_task(host->context, &request);
}
```

`request`与`choice`可以放在栈上，Host返回前已经复制。返回0时保留当前业务状态，
提供合适的可用性反馈。非零时先记录ID，等待具名`on_task_completed`；该回调先
核对结果结构大小、pending ID和outcome，成功且choice=7才移除自己的内存草稿。
Cancelled保留草稿，Failed保留并提供可恢复说明，随后清理模块自身pending。
如果完成回调会立即发起另一请求，应先清理旧pending再调用`request_task`。

模块API表需在尾字段注册具名完成回调。不捕获Scene，不保存结果借用视图，不从
业务work线程提交请求，也不把`Success`自动解读为已保存到某个文件。

## 8. 验证门槛与后续顺序

5b已完成以下自动验证，结果与失败修正见
[执行计划第五阶段5b](INTERFACE_SYSTEM_PLAN.md)及
`dist/validation/interface-system-step5b/`：

1. ABI完整尾字段与guarded partial field、旧模块无回调、owner线程和create拒绝。
2. 切片/结构边界、UTF-8、长度、roles、unique ID、owning复制及单请求忙状态。
3. 真实Host stage、Master composition、Preview隔离、保留前缀和无panel能力拒绝。
4. 实际输入快照采用前拒绝成功，采用后provider确认；Esc、业务取消、作用域失效。
5. 终态一次、重复/迟到/未知choice、callback同步发起下一请求、退出与失败不回调。
6. 宽/窄/短视口、长正文滚动与折行、材质/明暗、正常控件和Popup恢复。

纯策略、隔离SDK夹具、真实GPU/Wayland和实机视觉分别记录；不能用模拟metadata
通过推断真实提交、帧率或视觉已验收。测试和probe留在tests，不进入生产安装包。

5b完成后接5c目录与文件请求模型，再接5d Notepad真实流程。图04轻量反馈和图05
主题运动继续使用本协议的请求身份、owner边界及不变输入快照，不新建第二套任务
生命周期或在WM加入业务渲染。
