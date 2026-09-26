# 主题包编写指南

主题运行时责任与切换协议以 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 为准。本页说明 schema v1 已实现的作者接口；0.1.0-6 已构建、安装到 Pi，四主题切换及主要点击通过现场确认。

## 包结构和入口

每个包只有一份样式源：`share/prism/themes/<id>/theme.prism`。文件必须以 `Theme("<id>", name: "显示名称", schemaVersion: 1)` 为根，ID 与目录一致。ID、token 和材料名使用字母、数字、下划线或连字符，长度 1..64；文件最多 65536 字节。

`prism::theme::CompileTheme(source, generation)` 编译源码；`LoadTheme(root, id, generation)` 读取并编译包，拒绝路径越界与 ID 不符。`DefaultThemeRoot()` 取运行程序前缀下的 `share/prism/themes`，构建树和安装树使用相同目录结构。编译器只依赖共享 DSL 语法与纯值契约。

现成完整范例为 `resources/themes/glass/theme.prism`。首批包的区别：

| ID | 窗口/面板背景 | 模糊 | 圆角 |
| --- | --- | --- | --- |
| `glass` | 半透明 tint | 开启 | 开启 |
| `translucent` | 半透明 tint | 关闭 | 开启 |
| `transparent` | 全透明 | 关闭 | 开启 |
| `square` | 不透明 | 关闭 | 关闭 |

四包保持相同 Shell 保留带和 BSP 间距。当前热切换明确拒绝改变 `Layout` 的候选包。

## 声明原语

- `Number("font_body", value: 14)`、`Color("text", value: #17212FFF)` 定义有类型 token。颜色为 `#RRGGBB` 或 `#RRGGBBAA`，alpha 不会降低整个控件树的透明度。
- token 可用 `value: "@other_token"` 引用同类型 token，支持前向引用；循环、未知引用和类型不匹配拒绝。
- `Material("window", ...)` 定义 tint、radius、blur、borderWidth/border、shadowBlur/shadowY/shadow、innerShadowBlur/innerShadowY/innerShadow、inputShape。所有字段必填；必须提供 `window`、`panel`、`card`、`control` 四种语义材料。
- `Layout(...)` 唯一声明 topbarSurfaceHeight、dockSurfaceHeight、dockMaxWidth、outerGap、innerGap。
- `Decoration("normal"|"focused"|"fullscreen", ...)` 三种状态都必填，字段为 shape、enabled、borderWidth/border、shadowBlur/shadowY/shadow。v1 `shape` 必须是 `"window"`，装饰圆角取同名材料，不能另设半径。
- `Controls(...)` 唯一声明 hover、focus、toggleKnob、focusWidth、toggleInset、toggleKnobRadius、toggleTrackRadius、innerShadowY。

材料和装饰数值/颜色字段接受字面量或 `"@token"`；`inputShape` 为 `"bounds"` 或 `"visible"`。未知字段、重复字段/定义、错误类型、非有限数字及越界值都拒绝，不会静默使用默认值。圆角范围 0..256，背景模糊 0..48，边框宽度 0..32，阴影模糊 0..128，阴影 Y 偏移 -128..128；其余范围由契约校验。

## 应用、主题和装饰的责任

应用 DSL 保留内容结构、约束和业务动作，使用 `material: "window"` 或 `material: "panel"` 取得通用材料，并用 `foreground: "@text"` 等引用主题 token。主题不定义 Music 或 Settings 专用绘制分支，也不改变业务状态。

标准普通窗口根使用共享 `window` 材料，不显式覆写根圆角。WM 绘制窗口外框/外阴影，客户端根材料不重复声明这些效果；局部卡片、控件的阴影由 SDK 执行。通用节点仍允许显式属性覆盖材料；应用主动覆写窗口根形状会脱离全局装饰一致性，当前接口不保证任意自定义根轮廓与外框匹配。

`blur` 请求真实 compositor 背景采样；客户端只绘制 tint，不能预先模糊壁纸冒充背景效果。`inputShape: "bounds"` 使全透明材料仍可交互，透明度本身不代表点击穿透。Shell 面板外的透明留白由输入轮廓保留穿透。

Settings 通过可选 `select_theme` 请求包 ID，以 `on_theme_event` 的 Current/Applied/Rejected 更新显示；提交请求不代表成功。没有会话主题接口时显示不可用，不自行维护一份局部配色。

## 新增与切换主题

在源码中复制 `resources/themes/glass/` 为 `resources/themes/<新ID>/`，同步修改根 `Theme` 的 ID/名称，再按需要修改 token、材料和装饰。应用引用的 token 必须保留且类型一致；颜色、透明度、圆角、模糊和阴影的组合由包控制。CMake 自动收集各目录的 `theme.prism`；重新构建/打包即可进入 `share/prism/themes`，无需新增 WM/Skia 中的主题分支。

安装后的包路径为 `/usr/share/prism/themes/<id>/theme.prism`。会话使用已配置的主题根；开发时可通过 `prism-session-runtime --themes-root <目录> --theme <初始ID>` 选择资源目录与初始包。运行中可从 Preferences 选择，或请求：

```sh
XDG_RUNTIME_DIR=/run/user/1000 prism-msg set_theme transparent
XDG_RUNTIME_DIR=/run/user/1000 prism-msg get_theme
```

主题选择目前属于会话状态；重启按初始配置加载，不自动持久化本次选择。修改当前包文件后仍需显式请求该 ID，运行时不监视文件变化。
