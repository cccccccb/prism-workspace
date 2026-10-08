# Owner任务开合运动与输入策略

阶段：Interface System **7b**。承接
[呈现生命周期与只读绘制值](OWNER_TASK_PRESENTATION_CONTRACT.md)，对应设计图05。
本阶段分三步实施：7b1协议与时序核心；7b2单调采样、输入门禁与受支持面板的像素组合；
7b3原生交互、降级和视觉验证。正式VNC仍为0.1.0-26。

## 1. 目标与实施边界

开合是一条随单调时间求值的呈现轨迹。业务TaskPhase、输入授权、动画求值完成、worker
实际采用及compositor Presented分别记录。低帧率可以跳过中间值，不能延长动画或伪造
实际采用。业务结果不等待退场动画；旧任务的绘制值没有输入、请求或回调能力。

7b1提供中间/终值帧协议、命名时序与纯数值轨迹；7b2源码接入统一采样、标准provider
的入场门禁与固定几何透明度组合。不支持独立导出的来源、instant及合法旧包继续使用
即时路径。源码能力与已安装桌面分开记录，不能把测试或资源Transition写成正式VNC
已经更新，也不能把worker采用写成用户视觉认可。

设计保持Concept B的紧凑布局、可读标题与辅助文本。首版运动先保持几何不变，只改变
任务层的整体可见程度；平移、缩放和owner正文退后须先明确独立绘制边界、效果关联和
输入策略，不能把Visual2D外观变化当作命中几何已经变化。真正透视及逆映射仍为7c。

## 2. 主题和独立.prism时序

沿用MotionSet v1及Theme schema 3，不增加脚本、业务ABI或应用节点。主题准备阶段
读取独立文件并产生typed MotionSet；运行期不读取文件。WM仍不解析客户端DSL。

```prism
MotionSet("example", version: 1) {
    Timing("taskEnter", durationMs: 180, easing: "easeOutCubic")
    Timing("taskExit", durationMs: 140, easing: "easeInCubic")
    Transition("task.open", timing: "taskEnter")
    Transition("task.close", timing: "taskExit")
}
```

上面是合法独立包的任务语义示例；目录应为`motions/example/motion.prism`，主题根引用
`motion: "example"`。它只提供宿主所声明的开合时序，不描述业务操作或任意视觉属性。
现有其他语义，例如`group.geometry`，由使用它的宿主独立解析。
作为当前系统motion包使用时，还须保留现有group.geometry、window.geometry、
panel.visibility与control.feedback等宿主所需名称；不能只用这两项替换完整系统包。

| 包 | task.open | task.close | 曲线 |
| --- | --- | --- | --- |
| prism | 180ms | 140ms | 开EaseOutCubic，关EaseInCubic |
| subtle | 120ms | 100ms | 同上 |
| instant | 0ms | 0ms | 同上，立即得到精确终值 |

这些数值在资源文件中定义，不写进SDK或WM。`ResolveTaskMotionSpec`分别按语义名解析：
旧合法包缺少某项时，仅该项降为零时长；旧schema的完全空MotionSet也降为零时长。
非空畸形集合仍校验失败，不能静默猜包ID或借用`panel.visibility`。

目前没有系统级Reduced Motion偏好。使用instant包可以明确禁用运动；不能宣称已经
读取系统无障碍设置。未来偏好应产生统一typed policy后再选择零时长，不由业务猜测。

## 3. 数值核心与绝对时间

`TaskMotionTimeline`借用已有AnimationClock和ScalarTimeline，不建立新Timer或线程。
核心只持有数值轨迹与目标，不持有Scene、Node、TaskSession、资源或业务回调。

- `BeginOpen(spec, now)`：从reveal=0到1。
- `BeginClose(spec, adopted_reveal, now)`：从调用者给出的最后实际采用值到0。
- `SampleAt(now)`：按开始时间、duration与easing直接求值，返回reveal、采样时间、
  目标endpoint和Intermediate/Terminal。`Sample()`取借用Clock当前值。
- `IsActive`和`CompletionDeadlineNs`为后续统一driver提供活动性及一次性完成期限。

reveal的范围是[0,1]。它是呈现数值，不是任务准备进度或文件操作进度。SDK适配器必须
把最后实际采用值和task/UI/cycle/projection一起保留；仅调用Sample不能证明值已采用。
关闭中途从实际看到的值开始，不从更晚排队的值或尚未采用的当前轨迹起点重新推算。

采样次数不参与计算；按有效单调时间取样，相同轨迹在相同时刻得到相同值。已经完成
的轨迹保持精确终值，迟到的更早时间戳不复活运动。MotionSet v1解析不含delay；无延迟
的零时长在开始时求得终值，
但仍须后续匹配帧实际采用才能推进呈现phase。时间使用CLOCK_MONOTONIC纳秒，继承
ScalarTimeline的期限饱和规则；饱和deadline不表示已到终值，仍以Sample的kind判断。
无固定60Hz忙循环、累加delta或按帧递增。
非法reveal或easing不能破坏已有轨迹，新周期可明确替代旧轨迹。

## 4. 中间帧与终值的采用协议

TaskPresentationStamp增加typed `sample_kind`，缺省Terminal以保持7a1/7a2调用行为。
endpoint表示本段轨迹的目标：Opening/Open为Open，Closing为Closed。Intermediate
描述一张尚未达到目标的帧，不能因为endpoint是Open就提前转为Open。

| 当前phase | 实际采用的sample_kind | 结果 |
| --- | --- | --- |
| Opening | Intermediate | 保持Opening，记录实际采用sequence，允许保留该绘制值 |
| Opening | Terminal | Open |
| Open | Terminal | 保持Open，更新当前投影采用值 |
| Closing | Intermediate | 保持Closing，旧输入已退休 |
| Closing | Terminal | Closed，释放退场绘制值 |

Open不接受新的Intermediate；同投影一旦发布Terminal，也不再倒退发布Intermediate。
绑定变化分配新projection，原序列边界随之清空；已Open的刷新保持Open，不重播入场。
呈现核心记录当前投影第一张Terminal的sequence，将中间与终值分成两个递增序列区间。
采用时核对区间和sample_kind，拒绝把已发布的中间帧改标Terminal或反过来改标。
区间不是运输授权：SDK仍须验证真实worker消费的不可变包及全部输入/环境关联。

中间帧也可构成“此周期曾有效采用”的证据。因此入场中取消可以进入Closing；入场
候选从未采用便取消则直接Closed。迟到的Open、中间或Closed回执不得复活旧周期或
影响新任务。projection/cycle耗尽、异常及退出沿已有明确失败/Interrupt清理。

## 5. 7b2输入策略：鼠标优先

输入作用域必须保持有效，才能准备和采用；不得通过`enabled=false`模拟入场禁用，
否则Scene会认为模态根不可用而退休任务。门禁放在Scene owner-modal输入处理入口，
先于文本、Slider、Scroll、手势与普通Activation分发，使用明确的呈现输入状态。

| 呈现阶段 | 任务输入 | owner正文输入 |
| --- | --- | --- |
| Opening及终值尚未采用 | 消费指针/按钮/滚动和操作按键，不激活控件；Esc可取消当前任务 | 按现有模态屏障阻止穿透 |
| Open | 当前准备/采用门槛和TaskPhase允许后正常操作 | 模态屏障仍有效 |
| Closing | 立即失去全部旧task输入，不注册只读片段命中 | 模态结束后按当前输入快照恢复 |
| Closed | 无任务输入 | 正常输入 |

Esc只取消当前owner路由中的当前scope；沿用所有seat共享局部模态的契约，发起seat
仅决定初始焦点，不作为取消授权。Esc释放抑制仍按完整InputSource配对，不让旧释放
或重复按键影响正文与successor。平台关闭、焦点丢失、捕获取消、owner退出等
生命周期事件仍按现有清理流程处理。不能把关闭前旧按钮release映射成正文点击。
Ready仍由真实输入快照采用推进，不按运动时长等待；Ready可以与Opening同时存在，
但呈现门禁此时不授予操作。触屏暂不作为本轮开发或验收范围。

## 6. 7b2渲染、资源与降级

进入非零模式前先核对标准provider和只读导出的支持范围；普通应用region不自动加入。
首版只组合可独立导出的固定字体、无图片、无backdrop依赖的标准任务层。无法保证
退场来源及效果正确时，本周期开关均用零时长，不把问题藏进不完整动画。

关闭先接受业务terminal、EndModal并隐藏活provider，再组合“本帧新正文 + 最后真正
采用的独立任务绘制值”。不缓存整窗或冻结监控/编辑内容，不复活请求/输入；背景与
主题变化导致旧值失效时直接撤下，并沿当前有效退休域发布终值或Interrupt。

当前7a2的Glass窗口祖先blur导致保守导出失败；不能改正常材质或丢弃blur来强行通过。
图片、backdrop退场要先实现资源保活与明确的效果合成关联，再扩展支持。文件列表
刷新不能沿用已失效资源的旧回执来登记片段。

现有导出已包含祖先clip/transform/opacity。真实平移/缩放必须先建立“祖先上下文—
任务子树”的typed独立边界，避免移动整个窗口clip；不得用整窗command索引切片。
全局opacity不代表已有GPU保留层，首版仍沿DisplayList差异、损伤及Skia回放处理。

## 7. 7b2统一调度与完成

复用ClientApplication既有FrameOpportunity/Answer、Clock、不可变FramePacket和
render worker。活动性为Scene动画或任务轨迹；期限为各活动轨迹下一必要期限的最小值。
主线程求值、准备typed样本和绘制包；渲染线程仅消费冻结值，不访问活Scene或业务状态。

每次实际数值变化使任务呈现包失效，并通过现有发布链提交；几何/主题等环境变化
先处理失效再采样。相同值不强迫重绘。完成后仍保持最后有效终值包直到worker消费，
不能因Clock已过duration就提前停driver、释放片段或推进Open/Closed。
真实采用的匹配Terminal才完成呈现生命周期；Presented仍使用独立反馈。

`FramePacket::task_motion`是可选typed冻结值，含task/UI/cycle identity、generation、
revision及reveal/endpoint/kind/time_ns。首次开始、开转关与即时降级分配新的非零generation；
语义样本变化才推进revision，相同数值不为时间戳制造一张新帧。计数耗尽明确失败，
不绕回重用。关闭另存最后真正采用的样本，独立绘制缓存保持未乘动画alpha的原始值。
最初即instant、合法旧包或无法导出的任务可以省略task_motion，但仍发布Terminal呈现
stamp并等待匹配采用；不能以“没有轨迹”绕过入场门禁。
这个即时决策按完整任务cycle锁定；同周期即使导出后来恢复支持，也不重新开始入场。
新任务重新判断，opening为零而closing非零的合法配方仍保留退场轨迹。

SDK检查当前任务、UI、投影与完整环境关联后，才接受样本。当前revision必须与准备值
精确相同；更早revision允许由worker真实消费的不可变包迟到采用，未来revision、generation、
时间或非法reveal拒绝。SDK内部关联不暴露为可外部伪造的协议，也不保留无限帧历史。
Pixels、有效State及已校验相同像素的None沿现有metadata采用规则推进；最后终值即使
像素指针不变也不能提前丢弃。FrameOpportunity可以回答空帧并设置一次性完成期限，
没有连续数值变化时不忙轮询。

Scene门禁使用精确modal token，不改变enabled、布局、几何或输入快照有效性；标准
provider在Begin时关闭操作门禁，匹配Open终值采用才打开。通用BeginOwnerTask仍保持
已有应用管理方式。门禁切换取消旧输入流，旧press/release不能在打开门禁或撤销模态后
重播。关闭时先EndModal、退休输入，再保留独立只读绘制值；正文按新快照继续处理。

透明度导出保留整棵解析树的原始顺序，只在目标子树外增加group opacity；不能逐命令
乘alpha改变重叠元素的结果。关闭组合保留祖先clip及原始内部opacity，在新正文上组合
最后采用的独立片段。alpha=0移除任务命令，alpha=1保持原命令；导出/组合/可选列表
分配失败即时降级，业务任务仍按已有失败和退出规则处理，不由可选动效拖延。

## 8. 顺序与验证门槛

1. **7b1**：规范、typed中间/终值协议、task.open/task.close资源时序、纯数值轨迹。
   使用fake Clock验证稀疏/密集采样一致、精确终值、最后采用值续关、原子拒绝、旧包
   降级、刷新/替代与晚回执隔离。现有SDK默认Terminal行为保持。
2. **7b2**：Scene明确输入门禁、独立任务采样接统一driver、不可变样本与像素组合；
   补受控SDK测试，验证Opening无误激活、Esc、Close立即退休、新任务替代、最终包
   采用前不完成，以及正文独立更新和资源降级。
3. **7b3**：串行原生Wayland/V3D验证鼠标与键盘、输入快照、首/末样本、环境变化、
   actual worker adoption及损伤；记录无支持材质的零时长路径。后续打包部署按用户
   已授权任务执行，并让用户对照设计图验收；测试通过不等于视觉获认可。

本轮不宣称FPS、GPU保留层、全主题退场、资源lease、触屏或透视能力。生产文件继续
遵守800行与编码规范，测试和probe独立于源码安装/正式包。

## 9. 7b1完成记录（2026-10-08）

中间/终值协议、纯TaskMotionTimeline和三套命名时序已在源码实现；相关CTest12/12、
零时长兼容的独立Wayland/V3D Provider14/14通过。新增轨迹15组及呈现6组边界覆盖
见[执行计划](INTERFACE_SYSTEM_PLAN.md#step-7b1任务开合时序与中间帧协议2026-10-08)。
规范、Skill、资源安装隔离与构建产物一致性检查通过。没有打包或替换正式VNC。

这些结果验证7b1核心及当时的生产即时行为；7b1完成时尚未接入7b2输入封锁、非零
像素与统一driver，不据此宣称图05运动或用户视觉验收。7b2结果单独记录。
