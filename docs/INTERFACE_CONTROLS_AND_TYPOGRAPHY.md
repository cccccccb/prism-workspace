# 界面系统：文字行高与控件状态配方

2026-10-05，第二阶段。依赖 v22 之后源码；当前已安装 v22 不支持本轮新增属性/token。
上层目标及后续阶段见 [执行计划](INTERFACE_SYSTEM_PLAN.md)。

## 1. 排版接口

新增 `lineHeight`（Number，0..512 逻辑像素），支持 Text、Button 的内置标签、
TextField、TextArea。支持数字、Number token 和 typed number binding；不支持
State 或 Transition，避免动画逐帧改变段落布局与编辑命中。

- `0` 使用默认行为：Text 跟随字体度量；编辑控件使用至少 1.5×字号的行框。
- 正值是**最小行框高度**。字体实际度量大于指定值时扩大行框，避免压叠；不是缩放字形。
- 增加的行距均分到字形上下。编辑光标、选区、滚动和指针命中采用相同的行框。
- Text 支持显式 LF、CRLF、CR 换行。CRLF 为一次换行，空行与末尾换行保留行框。
- Text 不因此获得自动折行、ellipsis、字重或字体回退；窄窗用内容分支/裁剪，
  不能宣称长段落已自动重排。TextArea 仍按原有显式换行与水平滚动模型工作。
- `height` 是布局分配高度，与 lineHeight 分离；过小 height 或 clip 仍可能裁剪文本。
- Button 的字号、行高、前景色同步到内置标签，包括 binding 与主题引用。

```prism
VStack(spacing: "@space_sm") {
    Text("文件设置", font: "@font_title", lineHeight: "@line_title", foreground: "@text")
    Text("更换位置后重试\n正文和文件名会保留", font: "@font_body",
         lineHeight: "@line_body", foreground: "@mutedText")
    Button("保存", action: "save", enabled: $canSave,
           font: "@font_body", lineHeight: "@line_body", height: "@control_height")
}
```

此例中文需要应用选择覆盖中文的字体；当前默认 DejaVu Sans 与单字体 shaping 不保证
中文字形。本轮独立渲染样例使用英文检验行框与状态，不冒充中文字体回退验收。

## 2. 四套主题共享 token

本轮增加到 glass/translucent/transparent/square（界面标签 Glass/Tint/Clear/Square），应用按需引用。现有 Demo accent 不被自动改写。

| 类别 | token | 默认逻辑尺寸 |
| --- | --- | --- |
| 局部间距 | space_xs / space_sm / space_md / space_lg / space_section | 4 / 8 / 12 / 16 / 24 |
| 行框 | line_caption / line_body / line_title / line_heading | 18 / 22 / 26 / 30 |
| 控件 | control_height / control_hit_height / control_focus_inset | 36 / 44 / 3 |
| 轮廓 | control_state_radius | 8；Square 为 0 |

颜色：controlPrimary、controlPrimaryHover、controlPrimaryPressed、controlPrimaryText、
controlDisabled、controlDisabledText、controlSecondary、controlOutline、controlFocus、
controlError、controlField。明暗各有值；深色亮蓝按钮用深色文字，浅色蓝按钮用白字。
禁用保留文字与布局，错误附说明，不仅靠颜色。材质和控件状态颜色各自负责自己的层次。
第三方旧主题没有这些 token 时会被正常校验拒绝；发布应用前明确所需主题能力。

## 3. 组合配方与状态优先级

复用 InteractionTarget + Visual + Text。外层负责稳定命中和 action，Visual 负责绘制。
不添加应用专用 Skia 分支，不增加第二套按钮输入或动画驱动。正式 SDK 组件复用机制
尚未提供时，不把文档配方假装成一个可直接调用的新组件名。

- 按钮底色响应 hovered / pressed / disabled，沿用现有规则优先级。
- disabled 文字单独切换到 controlDisabledText；输入必须同时用 enabled 禁用。
- 键盘焦点采用独立外层 Visual 的 opacity，内部面板留 inset，形成外侧轮廓；
  焦点和背景的状态属性不互相抢写。焦点必须由真实键盘事件形成，不能只画“焦点按钮”。
- 焦点轮廓不改变布局和命中尺寸。透明 Visual 从不获得窗口权限。
- 此阶段样例无动画，先验收稳定状态；后续可引用主题 Motion，不能对 enabled/lineHeight 动画。

可执行配方见 [controls.prism](../tests/fixtures/interface-system/controls.prism)。
样例包含五种按钮状态、普通/错误输入框、文字分区和显式双行段落。按钮与输入真实走
Scene 输入链；示例错误说明是固定展示，尚未接业务校验。380px 窄版省去次要说明列，
保留状态名和主要按钮；不将稀疏内容按 flex 均分成大卡片。

## 4. 验证与边界

`text_line_height_test` 验证显式换行、最小行框、基线、Button 标签 binding、
非法值/动画拒绝、编辑行高与鼠标选位一致性。
`interface_controls_probe` 使用四套真实主题 × 明暗 × 680/380 宽，共 16 种组合，
经真实鼠标/键盘事件制造悬停、按下、焦点和禁用，再用 Skia raster 输出 PPM。
该 probe 不安装；截图验证控件外观与布局，不测 GPU 帧率、不证明背景毛玻璃效果。

尚未完成：中文字体回退/输入法、自动段落折行、字重/字间距、独立 Checkbox/Radio/
Slider/Segment、通用焦点作用域和 Popup/Menu。它们按执行计划继续，不由静态配方冒充。
