# Owner任务会话与局部模态作用域

日期：2026-10-07。界面系统5a；承接图03文件任务、图04应用确认和图05任务运动。
本阶段建立C++运行时核心、Scene输入作用域与SDK生命周期适配。系统文件服务、业务
C ABI、跨进程owner授权、文件列表及任务面板DSL声明在后续阶段接入；v26不包含5a。

## 1. 三种独立状态

| 状态 | 负责层 | 含义 |
| --- | --- | --- |
| 任务会话 | Host/SDK与纯TaskSession | 请求身份、业务阶段、成功/取消/失败 |
| 局部模态作用域 | Scene及提交输入快照 | 当前owner允许交互的子树、捕获取消、焦点恢复 |
| 呈现与运动 | DSL、主题与既有动画/渲染管线 | 面板内容、开合、owner退后、材质与配色 |

动画完成不代表业务完成，业务终态也不代替面板收回。任务控制器负责准备与隐藏面板，
输入作用域接口不修改应用的visible绑定，不为具体应用绘制UI。WM继续处理通用窗口
归属、呈现与跨surface约束，不解析文件列表、请求内容或应用DSL。

5a先支持每个owner同时一个任务；不同owner可以并发。嵌套任务必须显式拒绝，不能
静默替换旧请求。普通非模态Popup/Menu保持原行为。

## 2. 身份与生命周期

`TaskOwnerId`与`TaskRequestId`是不同的strong type，组成`TaskIdentity`。
owner由最终前端运行实例发放，request由其TaskSession单调发放，均不回绕或复用。
不能借用业务的work task_id、可复用窗口索引、PopupToken或候选UiLoadId。
预热种子不提前发放owner；SDK在已打开并安装UI的前端首次启动任务时建立会话。

这些身份是process-local关联元数据，**不是跨进程权限凭证**。后续服务协议必须由
Host验证真实窗口/连接归属，不能相信模块提交的owner数字。

适配器另保存实际`installed_ui`与Scene作用域token。准备新UI、取消候选加载、失败
候选不会撤销仍显示的旧任务；成功全UI替换以`UiReplaced`结束旧任务。区域替换、隐藏
或禁用作用域根使其不可用时，以`ScopeUnavailable`取消。普通主题/尺寸变化保留任务。

owner关闭先撤销任务和输入作用域，再停止工作、销毁业务与前端。关闭或前端失败后
不再向业务投递终态；单个任务取消不调用永久关闭工作池的StopWork。

## 3. 任务核心

`prism/runtime/task_session.hpp`提供owner线程使用的纯策略对象，不持有Scene、文件
路径、渲染对象或回调。

| 操作 | 允许条件 | 结果 |
| --- | --- | --- |
| Begin | owner存活，无活动任务及待取终态 | 新request，Preparing |
| SetReady | 精确匹配Preparing/Working | Ready |
| SetWorking | 精确匹配Ready | Working |
| Succeed | 精确匹配Ready/Working | Success，移出活动任务 |
| Cancel | 精确匹配任意活动阶段，有效取消原因 | Cancelled，移出活动任务 |
| Fail | 精确匹配任意活动阶段，有效typed失败 | Failed，移出活动任务 |
| RetireOwner | 一次性永久退出 | 活动任务以OwnerClosed取消，不再接受Begin |
| TakeTerminal | 存在待取终态 | 先移出，再允许新请求或业务回调 |

Preparing阶段不能宣称成功。SDK仅在匹配当前作用域的输入几何实际被渲染工作线程
采用后，将Preparing推进Ready；排队或调用Begin不代表可以交互。Working回Ready
用于可恢复错误/重新选择，错误文案与表单内容由控制器保留。

一个会话最多保存一个活动请求或一个待取终态。终态接受后不可改写，重复、迟到、
外来owner和旧request全部拒绝，拒绝不改变现有任务。取出终态后再调用业务，支持
业务同步发起下一个请求；退出时已接受的终态由适配器丢弃，不改写成另一种结果。

`TaskTerminal`区分Success、Cancelled、Failed，只携带对应的取消原因或失败结构。
失败由typed code分类，诊断为可选且最多1024字节；非法枚举和越界诊断原子拒绝。
5a不提供文件结果payload，也不把空路径作为取消或失败。

## 4. Scene作用域

`BeginOwnerModal(root, seat)`接受已安装、可见、可用的普通子树，返回单调非零token。
`EndOwnerModal(expectedToken)`只结束精确匹配的作用域。Scene产生含token与原因
Escape/Unavailable/Ended的owning closure，SDK据此终结匹配任务。

作用域约束所有seat；发起seat只用于初始焦点。打开时保存返回焦点，关闭已有Popup，
撤销其原生采用，取消正文的pointer/key/gesture/slider流，随后将焦点移入作用域。
取消事件仍可交付，旧按下的松开、repeat不能激活新界面。

- 鼠标、滚轮、文本、值控件、Tab/Shift+Tab均限定在作用域内。
- 外点消费输入但保留任务；其他应用窗口照常操作。
- Esc按下只产生一次安全取消；对应释放被消费，不穿透到正文。
- 切出owner取消当前seat捕获，保留任务；切回恢复作用域内有效焦点。
- 结束后返回仍可用的原焦点，否则选择稳定有效入口；不能恢复被删除/隐藏/禁用节点。
- 第一版作用域期间拒绝全部OpenPopup，暂不提供模态内菜单或嵌套作用域；后续扩展
  需带同一作用域身份及返回规则，不能绕过门禁。
- 本阶段不增加触屏交互；已有取消和生命周期路径继续保留。

## 5. 快照与真实提交

`InputSnapshot::owner_modal_epoch`独立于popup_token与普通几何version。初始为0，
打开、关闭、失效均推进；不存在作用域时也保留最新epoch，不能复用关闭前的0。
正常布局、主题与背景metadata不推进该epoch。

实时命中、提交快照命中、可交互判定、ApplyInputSnapshot和事件dispatch共同检查
当前epoch。门禁覆盖Popup/滚动/Slider/文本等路径，不能只限制最终Activation。
旧、null或外来Scene快照不能开始交互；Focus/Close等生命周期仍执行取消和清理。
原生Popup的`scene == 0`局部描述符不是普通Scene快照，仍完全拒绝根输入入口；
它只能经已采用的child身份进入原生浮层输入接口，不能伪造失焦或取消来修改根状态。

原生Popup的局部快照必须保留该身份，迟到plan/adoption拒绝；SDK将root键盘转发到
child时同样校验epoch。打开模态先撤销旧child，不能从旧root快照跳转到另一份已提交
child几何。只修改输入meta也需通过已有State/checked None真实采用路径，不能以新
task token或PublishFramePacket返回值代替采用确认。

状态修改在UI/owner线程进行，渲染线程消费既有不可变帧包。任务不是新的动画Timer
或渲染循环；本阶段不添加固定轮询、空提交或同步GPU等待。

## 6. SDK入口与后续接口

SDK/Host以已安装的命名region确定作用域根，使用BeginOwnerTask、阶段切换、成功/
取消/失败和TakeOwnerTaskTerminal；闭包关联由SDK持有，业务不持有Scene指针。
关闭与失败永久退休owner；成功UI替换仅取消当前request，保留前端owner生命周期。

创建拒绝或发布异常撤销尚未交付的Preparing及作用域，不留下无法关联的活动请求。
发布可能同步交付已有控件/手势的Cancel，回调可以读取当前身份、接受终态或发起下一
请求。已被回调接受的首个终态保持，异常处理不能将其抹掉，也不能撤销/领取下一请求
的终态。回滚只针对原始identity；Begin返回的identity表示该次请求，调用者不能假定
同步回调之后它仍然活动。作用域创建异常会撤销未返回token，但不重放已取消的捕获
或被替换的Popup。

5a保留旧同步OnCloseRequested/on_close_requested契约。5d源码随后追加typed
关闭续接与重复请求合并，Notepad实际保存后自动继续关闭，见
[文件业务与Close](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)；不是5a核心单独提供的能力。
5a不会通过现有action字符串伪造文件服务，也不要求第三方复制一个选择器。

## 7. 实施与验收顺序

1. 5a：TaskSession纯核心、Scene局部模态、SDK输入采用/退出/UI替换适配。
2. 5b：任务请求/结果与业务ABI、Host/provider交接、共享DSL任务面板及可信归属。
3. 5c：文件请求、异步目录模型、打开/保存/目录任务、覆盖与失败恢复。
4. 5d：Notepad真实接入，打开/另存为/未保存确认及关闭续接。
5. 图04轻量反馈，再按图05接主题化翻起/收回及中断/reduced motion。

5a验证包含独立owner并发、重复/迟到终态、退休、候选加载失败、真实UI替换、区域
失效、多seat、旧/null快照、旧child adoption、捕获与Slider取消、Esc、焦点恢复、
主题/resize及渲染metadata采用。测试和probe独立放在tests，生产包不安装它们。
实机部署和视觉面板验收由后续步骤单独记录，本阶段不据纯核心测试推断帧率。
