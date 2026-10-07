# Tooltip：只读控件说明（6b1）

依据[图04](design/interface-system/04-feedback.png)的“Tooltip / 输入说明”：只解释控件，
不放必需操作；鼠标悬停和键盘聚焦都能触发，不抢焦点，必要说明直接留在界面里。
本步提供独立的客户端 DSL/Scene 能力，与业务反馈、owner 确认及受信会话确认分开。
源码接口需要匹配本步 Host/SDK；已安装的 `0.1.0-26` 尚不提供该能力。

## 1. 归属与边界

- 应用 DSL 声明锚点、说明文字和主题样式，框架管理时序、布局和呈现。
- Tooltip 使用当前窗口的 Scene 与渲染管线，不新建进程、Wayland surface 或原生 Popup。
- WM 不解析说明文字，不管理应用内部的 Tooltip，也不新增应用专用分支。
- 提示不取得焦点，不进入 Tab 顺序，不消费原控件的点击，也不建立输入屏障。
- 保存失败、覆盖决定、权限与会话操作必须使用对应业务/受信接口；Tooltip 只作说明。
- 这不是无障碍名称、ARIA、自动本地化或字体回退接口。陌生、有风险的操作仍保留短标签。

## 2. DSL 声明

```prism
Card(material: "window", padding: "@window_padding") {
    VStack {
        IconButton("folder", "open-file", width: 32, height: 32,
                   material: "control", foreground: "@text")
        Text("Choose a document to edit", font: "@font_body", foreground: "@mutedText")
    }
    Tooltip("open-file", width: 224, height: 44, tooltipDelayMs: 500, padding: 8,
            background: "@surfaceRaised", borderWidth: 1, borderColor: "@controlOutline",
            cornerRadius: "@control_state_radius", justify: "center") {
        Text("Open a UTF-8 text file", font: "@font_body", foreground: "@text")
    }
}
```

`Tooltip` 是独立 Kind，不是带特殊样式的 Popup。它与 Popup/Menu 同处 Scene 根的末尾
浮动声明段；普通内容必须在该段之前。Interface v2 的布局文件负责根声明，不能把
Tooltip 塞进 Interface 的 Binding/Component 列表，也不能在组件文件中追加第二个根。
锚点可以来自已链接的 Slot，完成 Scene 组合后统一校验。

| 字段 | 语义与限制 |
| --- | --- |
| 第一个位置参数 / `tooltipFor` | 非空固定 action 字符串；不能用 binding 或主题值改变锚点身份 |
| `width`、`height` | 必须显式提供正尺寸；描述完整可读说明的目标尺寸 |
| `tooltipDelayMs` | 悬停等待时间，Number，范围 0—10000ms；默认 500ms |
| `min/maxViewportWidth/Height` | 与现有尺寸条件一致；隐藏的提示不能因锚点存在而显示 |
| 文字、间距、边线、圆角、颜色 | 使用现有 DSL 属性与主题 token；`font`、`cornerRadius` 不是新别名 |

首版要求每个 Tooltip 对应的 action **在整棵已组合 Scene 中恰好出现一次**，每个 action
最多声明一个 Tooltip。即使重复控件按宽窄条件互斥也不放宽这个规则；给不同布局使用
不同固定 action，或先选择具有唯一 action 的控件。不能用 tooltipFor 引用另一个浮层、
Visual 的装饰内容或动态 action，也不能用说明替换控件自身的业务 action。

### 只读内容

允许 HStack/VStack/Card/Visual、Text、Icon、Image 和 Separator。只读布局不因此获得
输入能力；图片仍遵循正常资源加载与所有者生命周期。说明的所有 Text 合计最多
256 UTF-8 字节，必须有效；允许显式 LF，拒绝控制字符和非法编码。

拒绝交互控件、编辑器、ScrollView、Popup/Menu、嵌套 Tooltip、Gesture、region、action、
Contour 与 window 材质。说明不提供可点击的“更多”、恢复按钮或滚动入口。需要这些
操作时应设计正文、Popup/Menu 或业务反馈。

当前不自动折行。文字可以用显式 LF 分行，并为完整内容提供足够宽高；实际字体 shaping
或当前 viewport/祖先 clip 无法完整容纳时，提示保持隐藏。不能缩小文字、裁切关键说明
或转成可操作的贴边面板来伪造可用。图稿中的文字尺寸不是渲染参数。

## 3. 触发、关闭与调度

鼠标使用实际提交/采用的锚点几何，等待单调时间的悬停期限。控件内部持续移动不重置
同一次等待；退出、锚点失效或作用域变化撤销等待。等待只设置一次性 poll deadline，
期限到达和真正显示变化才失效对应呈现，不新增固定 tick 或持续重绘。

触发模式由最近的有效输入决定，不同时竞争鼠标位置与历史键盘焦点：

| 输入 | 当前模式与候选 |
| --- | --- |
| 非重复 KeyDown | 键盘模式，只从现有 `focusVisible` 目标选择说明 |
| 真实 PointerEnter / PointerMotion | 悬停模式，只从当前有效 hovered 目标选择说明 |
| PointerLeave / PointerCancel | 撤下说明；离开后不回退到旧键盘焦点 |
| 框架 `FocusNext()` | 显式键盘模式，保持现有程序化导航语义 |

正常 Tab 导航产生 `focusVisible` 后不等待鼠标期限；仍等待几何与说明对应的帧真正提交
和采用，才把它记为已显示。鼠标点击产生的普通 focused 状态不冒充键盘触发，也不会
让说明因按下动作持续停留。显示前后的原 seat 与焦点 Node 保持不变。

布局失效先关闭当前呈现，并以真实 metadata 采用作为几何门槛；准备期间不使用旧坐标
打开说明。若采用后的候选身份、锚点 bounds 和等待时间都未改变，保留原来的**绝对
deadline**，不从每次布局更新重新等待。例如 Preferences 每500ms刷新监控文字，不能
使500ms悬停提示一直等不到期限。锚点真正移动、尺寸改变或候选/等待时间改变后才重新
建立等待；已到期的相同候选在几何可用后继续呈现。

按下、Esc、离开、窗口失焦、锚点隐藏/禁用/销毁、UI 替换、Popup/Menu 或 owner 任务
启动时撤下说明。撤下不执行任何业务 action；原事件继续按其正常作用域处理。Esc/
按下抑制当前候选的立即重开。布局准备、尺寸更新和重新采用不会删除这份被抑制身份；
需要真实离开后再次进入、进入新的悬停目标或实际重新聚焦，才能产生新的有效候选。

Tooltip 不改变 Popup token、OwnerModal token，也不保存一份待恢复焦点。其生命周期
跟随实际 Scene、锚点 Node generation 和 owner；旧候选与旧期限不能操作替换后的节点。
禁用原因与必须阅读的信息留在界面中，不依赖用户能够悬停到禁用控件。

## 4. 定位和输入快照

位置按当前有效锚点计算，优先下方、必要时上方并横向避让，完整尺寸限制在当前逻辑
viewport/有效裁剪内。空间不足时隐藏，不应用 Popup 的 EdgePanel/限高交互回退。
提示不会绕过窗口根与祖先的裁剪，也不会向 WM 申请更大的输入区。

Tooltip 与其整个只读子树从 InputSnapshot 节点树、命中、Tab 目标以及 surface input
regions 排除。根快照只附带被动的 `tooltip_node` 与 `tooltip_anchor` 身份戳，用于关联
当前实际呈现状态；`snapshot.Find(tooltip_node)` 返回空是预期结果。这两个 ID 不是
输入目标或应用可提交的权限凭证。

框架内部可通过 `TooltipNode()`、`TooltipAnchor()`、`NextTooltipDeadlineNs()` 观察当前
状态。应用业务模块无需读取 Scene、定时器或这些 NodeId，不要把内部状态变成应用
binding/action 来复制框架生命周期。

## 5. 应用例子

| 应用 | 锚点 | 说明 | 布局位置 |
| --- | --- | --- | --- |
| Notepad | `new` | New document | `layout.prism` 根末尾，锚点来自 toolbar Slot |
| Notepad | `open-panel` | Open a UTF-8 text file | 同上，不虚构快捷键 |
| Preferences | `prefs:menu:wide` | Pages and monitoring | `master.prism` 根末尾，宽窗条件 |
| Preferences | `prefs:menu:compact` | Pages and monitoring | 同上，紧凑条件 |

Notepad `save-as` 和 Preferences 的主题卡/监控操作在互斥布局中复用 action，首版例子
不对这些重复目标声明 Tooltip。必要的文件名、状态、错误和设置标题保持正文表达。
新增提示没有改变业务模块 ABI、文件读写、关闭流程或系统授权。

## 6. 验证与范围

确定性测试负责期限前后、取消、饱和、输入身份、无效 DSL、只读校验、文本/布局边界和
旧快照隔离；测试使用可控时间，不能把 native 调度耗时当成精确的 hover 延迟证明。

`tests/probes/tooltip_native_probe.cpp/.py` 沿用隔离 Wayland/V3D 和真实 Notepad DSO，
声明五个连续场景：

1. 实际鼠标悬停，提示文字进入真实采用帧；非空原生 seat＋编辑器 Node 保持，直接续写。
2. Esc 撤下，并超过声明的悬停时间检查同一候选不会复活。
3. 原生 Tab 聚焦即时说明；Enter 仍只执行一次 New document。
4. 提示存在时原生点击锚点，仍只执行一次 New document。
5. Open 启动真实共享文件任务，任务优先于提示；取消后草稿与文档数保持。

采用验证同时检查被动身份戳、实际帧中的匹配 glyph run、整个只读子树不在输入节点树，
不以内部 active 标志或请求接受代替实际呈现。焦点验证必须有非空原生样本，不能把
空集合比较写成“保留已有焦点”。

原生工具仅覆盖 Square Light、鼠标和键盘，不代表全部主题、触控、用户视觉验收、文件
IO 或 FPS 性能结论；不会安装软件包或切换正式 VNC。测试与 probe 不进入生产包。
构建与验证结果以本步执行计划及验证目录中的实际记录为准。
