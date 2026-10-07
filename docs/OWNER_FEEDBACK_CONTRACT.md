# Owner 轻量反馈契约（6a）

本轮接入图04的业务反馈：成功、信息与可恢复错误。参考
[图04](design/interface-system/04-feedback.png)和[执行计划](INTERFACE_SYSTEM_PLAN.md)。
源码能力与当前正式v26桌面分开；本轮没有打包、切换VNC或完成视觉验收。

## 职责与呈现

业务模块提交拥有明确语义的请求，Host管理窗口内的呈现与生命周期，共享DSL定义布局，
主题定义颜色、材质与控件状态。WM继续只管理窗口和输入，不读取反馈内容或应用DSL。

| 状态 | 呈现 | 期限 | 业务动作 |
| --- | --- | --- | --- |
| Info | 必要信息与正文 | 默认4秒 | 可选，最多两个 |
| Success | 真实完成结果与正文 | 默认4秒 | 可选，最多两个 |
| Error | 可读原因与恢复操作 | 持续保留 | 可选，最多两个 |

不抢焦点、不遮挡整个owner、不创建第二个前端或平台surface。反馈卡片采用自然紧凑的
标题、滚动正文与固定操作行，恢复操作有文字标签；颜色和图标不能代替原因。
错误不会自动消失，用户可以关闭。显示、关闭与到期不会触发业务确认回调。

当前提供一个窗口内的反馈位置，同一owner新请求替换旧请求；没有全局通知中心或多条
堆叠队列。文件任务与局部模态优先，反馈在其活动期间暂停。会话关机确认、Tooltip、
系统级权限与图05透视翻转是后续独立能力，不从反馈API推导授权。

## 有类型的C ABI

定义见 `prism/contracts/app_feedback.h`。Host可选尾字段为：

- `feedback_capabilities(context)`：当前owner的反馈能力，bit `PRISM_FEEDBACK_CAP_OWNER_V1`。
- `show_feedback(context, request)`：完整验证并复制输入，接受时返回非零关联ID。
- `dismiss_feedback(context, request_id)`：撤下匹配请求；过期或错误ID不撤下新请求。
- 模块可选 `on_feedback_action(context, event)`：只有用户选择具名业务操作时调用。

必须先按 `offsetof + sizeof(field)` 检查Host尾字段，再检查函数指针。调用限所有者
线程，模块create不能提交。没有动作的反馈不要求action回调；声明动作必须有完整
`on_feedback_action`尾字段。旧Host或不适用的根布局查询不到能力，业务保留正文状态。

请求含kind、title、message、actions、duration_ms。title非空最多256 UTF-8字节，正文
最多2048字节，最多两个操作；操作ID非零且不重复，label最多48字节。拒绝无效UTF-8
和控制字符，正文可含LF。Error的duration必须为0；其余0表示默认4000ms，显式期限
为1000—30000ms。输入view只借用到调用返回，action event只借用到回调返回。

```c
/* host已完成完整尾字段与capability检查；模块实现on_feedback_action。 */
PrismFeedbackActionV1 retry = {
    sizeof(retry), 1, {"Retry", 5}
};
PrismFeedbackRequestV1 request = {
    sizeof(request), PRISM_FEEDBACK_ERROR_V1,
    {"Unable to load", 14}, {"Choose another file or retry.", 29},
    &retry, 1, 0
};
uint64_t id = host->show_feedback(host->context, &request);
/* id只表示接受，业务复制并记录id；不代表已经呈现或操作成功。 */
```

动作回调不早于show返回；匹配请求在进入回调前退休，回调可以显示新反馈或启动文件
任务。过期、替换、用户关闭、UI替换和owner退出静默撤销；旧请求的动作不能操作新
请求。完成回调不代表业务工作成功：例如Retry仍须重新执行与检查真正的读写。

## 输入身份与计时

每次内容替换生成新的区域节点身份，旧快照与按下状态不能激活新恢复操作。保留名称
`__prism_feedback_`属于框架，应用DSL、Interface binding/component、区域与业务
set_binding不得覆盖。共享反馈组合在正文之后、模态任务之前；根末尾Popup/Menu的
布局约定继续保持。

期限使用单调时钟的已显示时间，从真实输入快照采用开始；不按帧数累加，不以请求
提交或准备完成当作可见。悬停或键盘焦点位于卡片时暂停，恢复后使用剩余期限。
计时到期只撤下反馈，不周期重绘；通过一次性poll期限唤醒，持久错误不创建计时器。
region替换失败保留已有树；UI替换、关闭与失败撤销反馈身份及其动作。

四材质包新增 `feedbackSuccess` 和 `feedbackError` 的明暗语义颜色。主题总token预算
扩展为256，保留64 KiB payload及其他约束；超出旧128项的新包需要匹配新版Host/WM，
不能只更新正式v26的主题资源。详见[主题编写指南](THEME_AUTHORING.md)。

键盘用户通过常规Tab进入动作，显示反馈不主动切换编辑器焦点。尺寸与祖先clip使
固定操作不可读或不可命中时，能力/请求明确失败，不靠缩小字体裁切按钮完成。

小于320逻辑宽度时，共享DSL收起标题前的状态图标，将宽度留给标题和关闭操作。
正文及超长标题仍可独立滚动；字号、固定按钮高度不缩小，错误原因不依赖图标表达。

## Notepad示例与文件安全

真正的work完成才显示Saved；保存期间产生新编辑时明确说明只保存了提交快照，当前
文档仍Unsaved。取消是Info，不生成持久红色错误；业务状态栏保留摘要作为旧Host回退。

打开失败可Retry或Choose file，仍执行全部读取校验。保存失败保留草稿并提供Change
location，重新进入SaveFile及覆盖确认；当前worker错误没有有类型的“是否已提交”
阶段，不能从错误字符串推断可盲重试，也不能复用失败SaveAs的旧覆盖许可。

恢复操作绑定请求ID、文档生命周期与目标路径。换标签、关闭文档、开始新任务或关闭
流程撤销旧反馈，槽位复用不能复活按钮。保存失败先拒绝原Close请求，后续更换位置
不会恢复已退休的关闭意图。文件版本、独占创建和已有目标替换的限制仍见
[Notepad与Close契约](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)。

## 验证范围

必须覆盖ABI短尾、无效请求、线程/退出、替换与回调重入；单调期限采用、暂停、恢复
和饱和；真实节点替换的旧输入隔离、无焦点抢占、模态共存；四材质与明暗、窄/短窗
的文本与操作；真实Notepad IO结果与恢复操作。测试与probe不进入安装包。
本轮证据在 `dist/validation/interface-system-step6a/`，以最终记录为准。功能与栅格
布局验证不替代用户VNC视觉验收，也不作为FPS结论。

最终相关CTest 48项、隔离Wayland/V3D原生场景18个通过，包含真实Notepad读写与
文件安全恢复。另在真实Saved后，以明确的SDK测试请求验证已有非零seat编辑焦点在
Show和实际采用后保持，未重新点击即继续键入；不将该请求记作额外业务成功。
四张真实字体CPU栅格图检查明暗、窄窗和短窗，不能代替全主题GPU/VNC视觉验收。
详见[6a验证记录](INTERFACE_SYSTEM_PLAN.md#6a验证结果2026-10-07)。
