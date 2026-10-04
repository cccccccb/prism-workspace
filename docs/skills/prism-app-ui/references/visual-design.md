# Prism 应用视觉设计参考

更新：2026-10-04。依据当前主题包和客户端 DSL 契约编写。
设计尺度见 [字号、间距与页面配方](design-system.md)，动效与版本差异见
[动效与坑点](motion-and-pitfalls.md)；普通设计任务无需先阅读旧应用源码。
本文供应用作者与 AI 开发者设计界面使用，重点是视觉、布局与交互反馈；启动、模块
ABI、异步任务及包结构请使用本 skill 的主指南和仓库对应规范。

文中尺寸均为逻辑像素。标为“建议”或“当前主题值”的数字不是控件 API 的硬性尺寸。
示例中的 `@token` 来自现有主题包；带 `$binding` 的片段需要在 Interface v2 声明
相应类型及初值，并由业务模块提供更新。示例 action 是业务约定，DSL 不会自动实现
同名功能。视觉组件文件只有一个根节点；下文各代码块分别使用，不应串接为多个根。

## 1. 视觉目标与层次

Prism 的默认方向是图标为主、文字为辅的简洁桌面。参考 mac-like 悬浮面板的层次，
使用烟灰玻璃、轻边线和有限阴影，保持内容清晰；普通应用仍适应 BSP 平铺窗口。

| 层次 | 表达方式 | 设计理由 |
| --- | --- | --- |
| 普通窗口 | `material: "window"`、统一根裁剪 | 跟随会话主题，与 WM 外框共享轮廓 |
| 内容分组 | `material: "card"`、小间距、短标题 | 分清业务区域，不把每行都做成独立窗口 |
| 可操作控件 | `IconButton` / `Button` / `Toggle`，`material: "control"` | 形状与 hover/focus 提示可操作性 |
| 主操作 | 同样的控件，少量 `@accent` 强调 | 避免一排按钮都抢占注意力 |
| 次要信息 | `@mutedText`、`@font_caption` | 降低视觉权重，但保留必要说明 |
| 状态与选择 | `check` / `error` / `refresh`、短文字或细指示条 | 状态明确，不依赖颜色单独传达 |

优先使用能解释动作的内建图标。例如 `play` 表示播放，`refresh` 表示刷新/重试，
`heart` 表示收藏。陌生或有歧义的动作应附简短文字；纯图标不能替代操作含义。
当前 DSL 没有 tooltip、ARIA、无障碍名称或自动本地化属性，不要声称相邻 Text 已
自动成为 IconButton 的语义标签。

静态图标、清楚的选中标记与及时的结果反馈已足够表达首版交互。不要为了“精致”
持续轮询、重复提交未变化内容或伪造 spinner 动画。

### 1.1 桌面视觉来源

现有 Shell 是普通应用的视觉参考：

- Topbar 只保留 Prism 品牌和时间文字，连接、电源等状态用图标表达；不展示 Wi-Fi
  名称或 AC 等附加文字。顶部短横线已接入鼠标组沉浸/恢复和局部反馈；
  普通应用不能复制该受信任系统控制接口。
- Dock 分为应用中心、固定常用应用、已打开应用三段，使用分割线区分。运行应用
  下方使用清楚的短横线；固定与运行区域出现同一应用不代表创建了两个实例。
- 横线色彩与厚度来自主题，间距让图标、分割线和运行指示各自清晰。玻璃以烟灰
  tint 保持对比，避免多层偏白底色覆盖背景、削弱内容。

第三方普通应用参考其图标、间距和层次即可；不复制 Topbar/Dock 到窗口中，不申请
Shell 角色。应用自己的选中横线表达实际页面/选项状态，运行指示由平台实例状态决定。
控制横线的固定感应区、视觉子树、状态规则和系统权限见
[交互与呈现规范](../../../INTERACTION_AND_PRESENTATION_SPEC.md)。InteractionTarget、Visual
与受限 `.state`、鼠标组控制已进入 v19；新的触屏系统操作仍延期。普通应用
不能凭 action 名称取得系统布局控制权限。

## 2. 会话主题与独立明暗配色

主题源位于 [resources/themes](../../../../resources/themes)，每包的 `theme.prism`
声明颜色/尺寸 token、Material、Layout、Decoration 与 Controls。应用视觉 DSL
引用这些数据，不在业务模块中维护另一套全局颜色表。

| 主题 ID | 当前视觉特点 | 当前 `blur_radius` | 普通窗口圆角 |
| --- | --- | ---: | ---: |
| `glass` | 烟灰半透明 tint、真实背景模糊、内外层次 | 16 | 12 |
| `translucent` | 更实的半透明 tint，没有背景模糊 | 0 | 12 |
| `transparent` | window/panel tint 全透明，内容卡片仍保持可读底色 | 0 | 12 |
| `square` | 实色、直角，阴影/背景模糊关闭 | 0 | 0 |

`light` / `dark` 是独立配色维度：选主题保留配色，选配色保留主题。应用不应把
“Glass”写成“Dark”的同义词，也不应通过一个窗口自行改色来假装全局主题已切换。
选择指示应由实际主题结果更新；请求发出时只表达处理中，拒绝时保留实际选中项。

当前主题 schema 3（兼容 schema 2 配色语义）的根 token 是 dark 基础，`Palette("light")` 覆盖已声明的同
类型 token；材料及其引用随后重新解析。以下是主题文件中的**摘录**，不是完整主题包：

```prism
Color("text", value: #F4F7FCFF)
Color("mutedText", value: #BBC7D8FF)
Color("accent", value: #83B9FFFF)

Palette("light") {
    Color("text", value: #152435FF)
    Color("mutedText", value: #45566AFF)
    Color("accent", value: #2F6EC3FF)
}
```

颜色字面量为 `#RRGGBB` 或 `#RRGGBBAA`；八位的最后两位是 alpha。用于 theme
引用时必须写带引号的字符串，如 `foreground: "@text"`；`$track_title` 是业务
绑定，不是主题引用。数字和颜色属性可用 `@token`；material 名称是静态字符串，
不能写成 `$material`。未声明 token、类型不符和未知材料会被拒绝。

### 2.1 常用语义 token

下表为当前四个主题均提供的 token；示例色值仅来自 glass/dark，不能复制成应用的
固定调色板。明亮、透明及直角主题会提供不同的解析值。

| token | 用途 | glass/dark 当前值 |
| --- | --- | --- |
| `text` | 正文、标题、通用图标 | `#F4F7FCFF` |
| `mutedText` | 次要标题、说明、时间 | `#BBC7D8FF` |
| `accent` / `accentSoft` | 通用强调 / 轻强调底色 | `#83B9FFFF` / `#84B9FF20` |
| `onAccent` | 强调背景上的内容 | `#FFFFFFFF` |
| `windowTint` / `panelTint` | 普通窗口 / Shell 面板 tint | `#1E293BB4` / `#25314478` |
| `cardTint` / `controlTint` | 局部卡片 / 控件 tint | `#111E3160` / `#ECF3FA18` |
| `border` / `divider` | 局部轮廓 / 分割线 | `#FFFFFF40` / `#E2EAF07A` |
| `shadow` / `innerShadow` | 局部外阴影 / 内阴影 | `#03081170` / `#02081248` |
| `progressTrack` | 进度轨道、Toggle 背景 | `#DBE7F130` |
| `indicator` | 运行或选中指示 | `#E9F1FFFF` |
| `none` | 完全透明 | `#00000000` |
| `hover` / `focus` / `toggleKnob` | 控件状态，由主题 Controls 引用 | 使用主题默认，通常不逐控件覆盖 |

`musicAccent`、`settingsAccent`、`albumTint`、`dockTileTint`、`dockTileBorder`、
`appCenter` 是当前 demo/Shell 的专用语义，不宜成为第三方应用的默认颜色依赖。
第三方应用通常优先使用 `accent`；需要专用材料/token 时先确定所需主题包契约，
不要假设任意新增名字在所有会话主题中都存在。

### 2.2 尺寸与排版

| token | glass 当前值 | 推荐用途 |
| --- | ---: | --- |
| `window_padding` | 14 | 普通应用根内边距 |
| `window_radius` / `panel_radius` | 12 / 12 | 从材料继承，不在普通窗口根硬写 |
| `card_radius` / `control_radius` | 10 / 10 | 局部卡片、控件 |
| `font_heading` / `font_title` | 20 / 18 | 页面标题 / 紧凑内容标题 |
| `font_body` / `font_caption` | 14 / 11 | 正文 / 辅助信息 |
| `icon_size` | 24 | 通用内容图标 |
| `button_size` / `button_padding` | 32 / 7 | 常用图标按钮 |
| `primary_button_width` / `primary_button_height` | 48 / 44 | 允许空间下的主操作 |
| `indicator_height` / `indicator_radius` | 4 / 2 | 运行/选中细条 |
| `progress_radius` | 3 | 进度条 |

建议同一局部区域采用 4–6 的细间距、8–12 的组间距；根内边距优先使用主题值。
这些建议没有对应的 CSS spacing scale 或自动网格。需要 6 或 8 时直接写合法数字，
不引用不存在的 `@spacing_sm`。

`font` 是正数字号或 numeric token，例如 `font: 14` / `font: "@font_body"`，
不是字体族名称。`HostConfig.font_path` 属于平台配置，当前默认指向 DejaVuSans.ttf；
UTF-8 字符串语法合法不代表该字体覆盖全部中文字形，也不代表已有完整字体 fallback。
示例保留英文标题/状态以适配现默认字体；应用需要中文时应实际检查配置字体与字形，
不能通过虚构 `fontFamily` 或 CSS 字体列表解决。

当前 glass 的 Shell/BSP token 包括 topbar 高 30、Shell 顶保留区 52、Dock 面板高
76、Shell 底保留区 100、outer_gap 14、inner_gap 12。它们是当前共享主题的几何，
由 Shell/WM 使用；普通应用不需要扣除一遍 Topbar/Dock 高度或在根添加 BSP gap。
现有主题热切换要求 Layout 保持兼容，不把改变保留区/间距当作已支持的在线响应式配置。

## 3. 材料、玻璃与窗口装饰的分工

### 3.1 普通窗口根

推荐根写法：

```prism
Card(material: "window", padding: "@window_padding", clip: true) {
    VStack(spacing: 8) {
        HStack(height: 32, spacing: 6, align: "center") {
            Icon("folder", width: 20, height: 20, foreground: "@accent")
            Text("Library", flex: 1, font: "@font_body",
                 foreground: "@text", overflow: "clip")
            IconButton("refresh", action: "library:refresh",
                       width: "@button_size", height: "@button_size",
                       padding: "@button_padding", material: "control",
                       foreground: "@text")
        }
        Card(flex: 1, material: "card", padding: 8, overflow: "clip") {
            Text("Choose an item", font: "@font_body", foreground: "@mutedText")
        }
    }
}
```

这里窗口根跟随 `window` 材料，`clip: true` 裁住整个子树。普通窗口外边框、焦点
外框及跨窗口外阴影由 WM 绘制；根材料当前没有重复的外边框/外阴影。不要额外在
根写 `shadowBlur: "@shadow_radius"` 或复制 WM Decoration 的边框。

普通根也不要硬写 `cornerRadius: 12`：square 的根应变成直角，其他主题的客户端
轮廓应与 WM `shape: "window"` 一致。局部按钮、封面装饰可以有自己的圆形/圆角，
普通根覆写圆角则不能声称仍匹配全局窗口外框。

### 3.2 局部材料

| 当前材料名 | 使用位置 | 特点 |
| --- | --- | --- |
| `window` | 普通应用根 | tint、圆角、背景效果、内阴影、`inputShape: "bounds"` |
| `panel` | Topbar/Dock 等 Shell 浮动面板 | 自身边框与局部内外阴影；通常有背景效果 |
| `card` | 应用内分组、状态卡片 | 本地边框/内外阴影，不请求背景模糊 |
| `control` | 图标按钮、文本按钮等 | 小控件的统一外观，不请求背景模糊 |
| `dockTile` | 当前 Dock 图标卡片 | Shell 特定底色与边线 |
| `album` | 当前 Music 封面区域 | demo 特定底色与局部阴影 |

普通应用以 window/card/control 为主要依赖。不要将 `panel` 无差别加到每个内容行：
glass 下它会请求独立背景效果，造成过多区域与多层视觉噪声。

材料字段可以被该节点显式属性覆盖。例如主操作保持 control 的形状/阴影，只覆盖
语义背景色与图标色：

```prism
IconButton("play", action: "player:toggle", width: 44, height: 32, padding: 7,
           material: "control", background: "@accent", foreground: "@onAccent")
```

显式覆盖不是 CSS cascade；每个属性的 token、字面量或绑定有类型。尽量只覆盖确实
不同的语义项，避免把材料的全部字段抄进每个节点后破坏统一主题。

### 3.3 真实背景效果与本地阴影

应用只绘制自己的半透明 tint、内容和局部效果。`backdropBlur` 请求已布局的背景
区域，由 compositor 采样该 surface 下层场景进行模糊。不要截取壁纸、读回屏幕或
给静态背景图片做滤镜来冒充真实背景玻璃。扩展不可用时仍应保持正常透明/tint 呈现，
不把“客户端能请求 blur”写成任意 Wayland compositor 都提供该效果。

客户端视觉属性的真实拼写如下，通常优先从 material 继承：

| 效果 | 客户端 DSL 属性 |
| --- | --- |
| 底色/前景 | `background` / `foreground` |
| 圆角/裁剪 | `cornerRadius`、`clip: true`、`overflow: "clip"` |
| 局部边线 | `borderWidth`、`borderColor` |
| 局部外阴影 | `shadowBlur`、`shadowY`、`shadowColor` |
| 局部内阴影 | `innerShadowBlur`、`innerShadowY`、`innerShadowColor` |
| 背景效果 | `backdropBlur` |
| 输入轮廓政策 | `inputShape: "bounds"` 或 `"visible"` |

主题 Material 的 `tint/radius/blur/border/shadow` 等字段和应用上述属性不是同一
拼写，不要把 `blur: 16` 或 `border: "@border"` 直接写到 Card 中。

当前 `backdropBlur` 为 0–48，cornerRadius 为 0–256，每 surface 最多 8 个背景效果
区域。结果裁到 viewport 和祖先 clip；无法由协议精确表示的偏移圆角交集会诊断失败，
不会扩成大矩形。优先在根或一个完整面板使用玻璃；局部 Card/Control 用 tint 即可。
不要依赖复杂嵌套圆角模糊蒙版。输入交集支持逻辑像素轮廓裁剪，不意味着背景效果
协议也支持所有相同形状。

透明度与点击穿透独立：window 的 bounds 输入政策保留整个圆角内容区域，即使
transparent 主题下 tint 和 blur 都为零。Shell 外部透明留白应没有 material/action，
保持穿透；不要把整张大透明 surface 设置成 bounds 后遮住背后的应用。

## 4. BSP 下的可缩放内容

BSP tile 的实际 configure 尺寸是布局输入，manifest 尺寸只是初始请求。窗口可能
变窄、变矮，应用不应假设固定 640×480 或用绝对屏幕坐标定位所有控件。

推荐结构是短 header + 可伸缩内容区 + 受控高度的操作区。用 `flex: 1` 分配剩余
空间，文本保留弹性宽度，小图标/关键按钮保持明确尺寸，次要说明需要明确 overflow。

### 4.1 当前布局语义

| 结构/属性 | 实际含义 | 使用建议 |
| --- | --- | --- |
| `HStack` | 横向流布局 | 图标、短标题、操作按钮一行 |
| `VStack` | 纵向流布局 | header、内容、transport 等区域 |
| `Card` | Box；子节点可叠放，默认填充可用区域 | 分组、封面、叠放指示、左右中心锚定 |
| `width` / `height` | 正数指定尺寸，0/省略为自动 | 不支持百分比、`auto` 字符串或 CSS 单位 |
| `flex` | 流布局主轴的剩余空间权重 | 显式 flex 会参与剩余空间分配，不是最小宽高 |
| `align` | Stack 交叉轴 start/center/end/stretch | HStack 常用 center；正文区域常用 stretch |
| `justify` | Stack 主轴 start/center/end/spaceBetween | 时间两端可用 spaceBetween |
| `padding` | 节点内边距 | 可用 paddingX/paddingY 覆盖对应轴 |
| `inset` | 父布局分配中的外侧留白 | 与 padding、BSP gap 不混用 |
| `spacing` | 可见流式子节点之间的间距 | 不会给 hidden 节点留下额外 gap |
| `anchor` | Card 子节点的 fill/left/center/right 水平锚定 | 不把它当任意绝对定位或 Stack 主轴布局 |
| `overflow` | visible 或 clip | clip 是裁剪，不是滚动容器 |

未指定主轴尺寸的容器在 Stack 中会自动分配剩余空间；叶节点通常按内容度量。
不要照搬 Web flexbox 的 `min-width`、`flex-shrink`、wrap 或自动换行假设。固定内容
超出可用空间时，没有通用自动缩字/隐藏/滚动补救；必须主动删减次要区域或选择
紧凑布局。`overflow: "clip"` 可以保护边界，但不是让重要按钮“可用”的办法。

Text 使用真实 shaping 度量，不按“字符数×字号”估算。当前没有声明式 ellipsis
或多行布局属性；长标题可以约束宽度并 clip，重要完整信息应通过应用信息区提供。

### 4.2 全栏居中与侧区

时钟式中心内容应是 Card 的独立 center 子节点，左右侧区独立锚定，而不是在一条
HStack 中靠相同左右宽度碰巧居中。当前 Topbar 使用相同模式：

```prism
Card(height: 30, paddingX: 12, paddingY: 5, justify: "center") {
    HStack(width: 90, anchor: "left", spacing: 6, align: "center", overflow: "clip") {
        Icon("folder", width: 18, height: 18, foreground: "@accent")
        Text("Library", font: "@font_caption", foreground: "@mutedText")
    }
    Text("Overview", anchor: "center", font: "@font_body", foreground: "@text")
    HStack(width: 32, anchor: "right", align: "center", overflow: "clip") {
        IconButton("refresh", action: "library:refresh", width: 24, height: 20,
                   padding: 3, material: "control", foreground: "@text")
    }
}
```

center 子节点按自然宽度居中；不要给中心 Text 一个大固定宽度后期待文字自动水平
居中。存在中心组时，Card 会限制左右组可用宽度；侧区也应显式 clip。极窄宽度下
不能保证三个区域都可读，需要减少侧区内容，而不是叠在中心文字之上。

### 4.3 小窗口检查

当前 Music/Settings 模板回归覆盖 482×420、482×204、244×420 等 BSP 内容尺寸。
建议第三方应用至少以这些形状检查核心动作可见/可点击，再按自己的最低可用尺寸
补充检查；它们是参考验收尺寸，不是窗口系统承诺的最小宽高。

- 宽且矮：减少标题层数，正文与装饰不能挤走底部主操作。
- 窄且高：让标题/数据弹性变窄，缩短辅助标签，避免横排很多固定宽按钮。
- 双页互斥：使用 `visible` 切换主体，保持页面结构和已挂载区域身份。
- 隐藏补充说明应由业务提供 bool 状态；DSL 没有 media query 或表达式断点。
- 所有可见 action 的完整可操作范围应在祖先 clip 内；只看截图不足以证明命中。

当前业务 C ABI 也没有 viewport/configure 回调。应用可以采用统一紧凑布局、弹性
内容和用户切页；不能凭空读取窗口宽度来设置 compact binding。按尺寸自动切换
布局需要先补通用 metrics/断点契约，不用屏幕分辨率代替实际窗口内容尺寸。

`visible: false` 隐藏整棵子树：不参与布局/间距、绘制、命中、输入区域或背景效果；
节点和绑定仍保留，重新显示时使用当前数据与主题。它不是 `opacity: 0`，也不是
销毁重建。初值默认 true，状态页和选择指示需要显式提供正确的 bool 初值。

## 5. 控件与合法图标

当前视觉组件如下，不存在通用 CSS/HTML 标签或可随意起名的 widget：

| 组件 | 支持的视觉用途 | 注意事项 |
| --- | --- | --- |
| HStack / VStack / Card | 布局、背景、材料、边线/阴影、裁剪 | 可以包含子节点；Card 本身没有 action 属性 |
| Text | text、font、foreground、background、clip/overflow | 没有 padding、material、fontFamily、fontWeight 属性 |
| Button | 文字按钮、action、材料/效果 | 自动生成文字 label；不接受嵌套子节点 |
| Icon | 内建向量图标、foreground | 无 action、material、border 或独立 padding |
| IconButton | 图标按钮、action、padding、材料/效果 | 不接受子节点；图标内容由 icon / 首位置参数指定 |
| Image | source、fit、cornerRadius、clip | 不是任意 Skia/SVG 绘图容器，不接受 material/边框效果 |
| Separator | 固定粗细分割线，background | 不是通用 RoundedRect；无 cornerRadius 属性 |
| Progress | value、foreground、background、圆角/材料 | value 必须 0–1；只显示进度，无拖动/action |
| Toggle | checked、action、前景/背景/材料 | checked 是 bool；点击返回 action，不自行翻转业务状态 |
| InteractionTarget | 固定命中容器、可选 action、局部状态 | 采用 Card 布局；无 action 只反馈，不自行实现手势或系统控制 |
| Visual | 纯装饰子树、平移/缩放/整体 opacity、显式状态规则 | 整棵子树不接受输入；不能作根或包含 action/material/backdrop/Slot |

所有这些组件接受 `width/height/flex/inset/anchor/visible`；具体属性仍以 schema 为准。
不要因为 Card 支持 shadow 就假定 Text、Icon、Image 支持所有同名属性。

内建图标是通用向量绘制命令，保持方形比例并在可用区域居中。IconButton 的图标
可用边长来自按钮较短边减两倍 padding；例如 32×32、padding 7，图标内容约 18×18。
不要把 padding 设得大于半个短边后期待仍显示图标。

### 5.1 合法名称列表

v19 可用的图标名单如下，大小写与连字符必须一致。后续源码图标另外列出，不能
因文档存在该名字就假定旧 Host 可用：

```text
grid music settings folder terminal
play pause previous next volume
wifi wifi-off battery search sun moon power
check chevron refresh error
cpu memory heart layers rectangle drop
fullscreen restore split-horizontal split-vertical
```

工作区新增图标：`document`、`document-add`、`save`、`close`、`arrow-left`、
`arrow-right`、`trash`、`info`，需匹配新版源码 Host，v19 不包含。
`favorites`、`spinner`、`warning`、`sliders`、`wifi_off` 等仍不是合法名。
需要新图标时扩展通用 schema/向量实现及其验证，不在应用中传未知字符串期待兜底。
`Icon($playback_icon, ...)` / `IconButton($playback_icon, ...)` 可以动态更新，但绑定
值仍须属于合法名单。不要用 emoji、Unicode 字符或平台字体 glyph 假装统一图标。

### 5.2 进度、开关与选择反馈

下面的视觉片段需要 number `progress`（初值 0）与 bool `enabled`（初值 false）：

```prism
VStack(spacing: 6) {
    Progress(value: $progress, height: "@indicator_height",
             cornerRadius: "@progress_radius",
             foreground: "@accent", background: "@progressTrack")
    HStack(height: 28, spacing: 8, align: "center") {
        Icon("cpu", width: 20, height: 20, foreground: "@accent")
        Text("Monitor", flex: 1, font: "@font_body", foreground: "@text")
        Toggle(checked: $enabled, action: "monitor:toggle", width: 34, height: 18,
               foreground: "@accent", background: "@progressTrack")
    }
}
```

业务收到 monitor:toggle 后决定新状态并回写 enabled；不能只改变视觉后假装业务
已成功。Progress 不会启动定时器，也不能以它实现可拖动的音量/播放位置 Slider。

在叠放 Card 中放一个短 Progress(value:1) 可表达选中状态。建议保留指示槽高度，
隐藏内部条而不是整个槽，避免选中切换导致相邻控件跳动；Settings 当前采用此方式。
这类指示只是绑定渲染，不会自动跟随主题请求或应用运行状态。

### 5.3 固定交互目标与局部状态反馈

以下是交互第二阶段源码支持的自定义图标控制区，需匹配版本的 Host/SDK。静态材料留在
目标上；只有内部图标子树缩放，点击范围保持 40×40。应用仍负责 `data:refresh` 的业务。

下面的控件片段放入已有父容器；Scene 根占满 viewport，固定命中尺寸在子节点布局中生效。

```prism
InteractionTarget(width: 40, height: 40, action: "data:refresh",
                  material: "control", align: "center", justify: "center") {
    Visual(width: 24, height: 24) {
        Icon("refresh", foreground: "@text")
            .state(when: "focusVisible", scope: "target", foreground: "@accent")
            .transition(property: "foreground", durationMs: 120, easing: "easeOutCubic")
    }
        .state(when: "hovered", scope: "target", scaleX: 1.08, scaleY: 1.08)
        .state(when: "pressed", scope: "target", scaleX: 0.94, scaleY: 0.94)
        .transition(property: "scaleX", durationMs: 140, easing: "easeOutCubic")
        .transition(property: "scaleY", durationMs: 140, easing: "easeOutCubic")
}
```

状态规则要求 `when`、`scope: "target"` 与至少一个合法呈现属性；scope 指向结构上最近的
InteractionTarget，声明必须位于其 Visual 子树。规则只接受数字/颜色字面量或主题 token，
不接受业务 binding、表达式或回调。基础属性可继续使用 binding；状态退出恢复最新基础值。
使用有 children 的 Visual 时，修饰符位于右花括号之后，沿用现有 DSL 语法。

支持 hovered、pressed、captured、disabled、focused、focusVisible。普通条件的同属性
优先级为 disabled > pressed > captured > hovered；焦点条件写入的属性不能再被其他条件
写入，重复条件/属性也拒绝。示例将键盘焦点颜色与按下缩放分开。disabled 来自通用输入
禁用接口，当前没有 DSL `enabled` 属性；不要伪造 `$disabled` 状态规则来代替输入禁用。

只有 Visual 可声明 translateX/Y（−8192..8192）、scaleX/Y（0.01..8）、originX/Y（0..1）
及 opacity（0..1）；原点默认 0.5，其余默认平移 0、缩放/opacity 1。原点不能 Transition。
Visual.background 与子树内直接绘制节点 foreground 可响应状态；状态不能改布局、资源、
action、材料、模糊或阴影。推荐引用命名 motion，旧 durationMs/easing 字面量仍可用；
两种形式互斥。instant 提供零时长参数，但不是自动读取系统减弱动效偏好的接口。

Visual 不能作为组件根，其整个子树不接受输入，也不能放 InteractionTarget、Slot、action、
material、backdropBlur 或 inputShape；零值也禁止。普通 Card 的材料可放在 Visual 外部。
opacity 对整组像素合成一次，不能用于禁用目标或宣称透明点击穿透。当前动画仍有 Paint、
列表与 Skia 回放成本；不使用永久业务 tick，不声称已经获得保留层或组控制能力。
完整规则及本阶段验证记录见[交互规范第 13 节](../../../INTERACTION_AND_PRESENTATION_SPEC.md)。

## 6. pending、empty、error、ready 的视觉与交互

状态页应共享同一主体位置，避免加载前后整个窗口跳动。准备/加载中可以先显示短
header 与稳定占位；deferred 区域使用 Slot 占位，挂载时不另外弹出窗口。

| 状态 | 视觉内容 | 动作规则 |
| --- | --- | --- |
| pending | 静态 refresh 图标、短状态文字、稳定占位 | 不虚构百分比；需要已就绪数据的动作由业务拒绝或暂时隐藏 |
| empty | folder/grid 等相关图标、明确“没有内容”、可行的添加/刷新动作 | 空结果可是合法 ready，不能当作加载失败 |
| error | error 图标、简短可解释消息、refresh 重试按钮 | 保存可用旧内容；重试不直接显示成功，不隐去真实错误 |
| ready | 真实内容、适量 check/选中指示、可用动作 | 成功状态由实际完成结果驱动，不由延迟猜测 |

下例是一个可作为内容区组件的状态结构。请在 Interface v2 声明以下 binding；各
bool 的互斥关系由业务计算，DSL 不支持 `!pending`、`ready && empty` 之类表达式：

| binding | 类型 | 示例初值 |
| --- | --- | --- |
| pending / empty / failed / content_visible | bool | true / false / false / false |
| status_text / error_text / item_title | string | "Loading" / "" / "" |

```prism
Card(material: "card", padding: 8, overflow: "clip") {
    VStack(spacing: 8, align: "center", justify: "center", visible: $pending) {
        Icon("refresh", width: 24, height: 24, foreground: "@accent")
        Text($status_text, font: "@font_caption", foreground: "@mutedText", overflow: "clip")
    }
    VStack(spacing: 8, justify: "center", visible: $empty) {
        Icon("folder", width: 24, height: 24, foreground: "@mutedText")
        Text("No items yet", font: "@font_body", foreground: "@text")
        Button("Refresh", action: "library:refresh", height: 28, padding: 4,
               material: "control", font: "@font_caption", foreground: "@text")
    }
    VStack(spacing: 8, justify: "center", visible: $failed) {
        HStack(height: 28, spacing: 8, align: "center") {
            Icon("error", width: 20, height: 20, foreground: "@accent")
            Text($error_text, flex: 1, font: "@font_caption", foreground: "@text", overflow: "clip")
            IconButton("refresh", action: "library:retry", width: 28, height: 28,
                       padding: 6, material: "control", foreground: "@accent")
        }
    }
    VStack(spacing: 6, visible: $content_visible) {
        Text($item_title, font: "@font_title", foreground: "@text", overflow: "clip")
        Text("Ready", font: "@font_caption", foreground: "@mutedText")
    }
}
```

这是 UI 状态建议，不改变 Host 的启动/失败期限。应用的 BackendReady 必须继续
遵循实际业务契约；deferred 占位、视觉 check、frame callback 都不能冒充业务成功
或内容已真实呈现。错误重试也应遵循启动期限与任务取消规则，详见主指南。

在 pending 阶段显示原操作按钮时，业务 handler 必须检查数据是否可用。当前 DSL
没有通用 `disabled` / `enabled` 控件属性；不要造一个 `disabled: true` 来解决问题。
如果不应可操作，隐藏操作节点，或使用无 action 的 Icon 并附状态说明。

## 7. 图片、裁剪与内容资源

包内图片使用 Image source，当前生产解码路径支持 PNG；内建 Icon 不等于任意 SVG
文件解码支持。生产 Host 的 assets_root 已指向包的资产目录，Image URI 相对该目录，
而不是相对组件源文件；`cover.png` 对应包内 `assets/cover.png`。资源路径和前端加载
由包/SDK 管理，不写屏幕截图读回或 renderer 调用。

```prism
Card(height: 120, material: "card", padding: 8, clip: true) {
    Image("cover.png", fit: "cover", clip: true, cornerRadius: "@card_radius")
}
```

此片段要求 manifest 的资产目录中实际提供 cover.png。Image 在 Card 内有明确的目标区域，
`cover` 保持比例并裁掉超出部分；`contain` 保持比例完整显示；`fill` 拉伸填满。
在流式 Stack 中自动 Image 会保留解码尺寸，不能仅设置 fit 就假定得到理想高度；
使用明确的目标尺寸或 Card 填充槽位。

不要预先给 source 填假的 ResourceId，或通过高频改 source 触发动画。图片准备与
真实 GPU 上传有预算和取消生命周期；pending 视觉应允许资源尚未就绪。大量装饰
图片需要实测，不把“小 demo 快”当作任意资源规模的保证。

## 8. 已支持与未实现边界

| 能力 | 当前状态 |
| --- | --- |
| 主题材料、明暗配色、token 热更新 | 已有；按实际事务结果表达选中状态 |
| 透明 tint、统一圆角、局部边线/内外阴影 | 已有；普通窗口外围装饰由 WM 管理 |
| 真实 backdrop blur | Prism compositor 已有有界圆角矩形协议与 GPU 合成；不是客户端假 blur |
| H/V 流布局、Card 锚定、flex、padding/inset、visible | 已有；不是完整 CSS flexbox |
| 图标按钮、文字按钮、Progress、Toggle、Separator | 已有；业务状态与动作仍由 module 维护 |
| hover/pressed/capture/focus、Tab/Shift+Tab、Enter/Space | 源码已有统一状态；主按钮与键盘释放时激活，取消不触发 action；仍非完整无障碍系统 |
| Interface v2 critical/deferred 与稳定 Slot | 已有；声明式加载顺序不是动画/滚动布局能力 |
| Slider 拖动、通用滚动容器、虚拟列表 | 当前视觉 schema 未提供，不能写伪 API |
| TextField / TextArea | 工作区源码已补基础编辑；需新版 Host，v19 不包含；非完整 IME/系统剪贴板 |
| CSS gradient/box-shadow、百分比尺寸、media query | 当前 DSL 未提供；使用现有类型化属性，Visual.opacity 也不是任意 CSS 样式 |
| 自动换行/省略号/自动缩字、任意自定义字体属性 | 未提供对应声明式属性，不作隐式假设 |
| Paint Transition 与单调帧时间 | 已有 Progress.value、指定 foreground，并扩展 Visual 的背景、平移/缩放/opacity；见[动画规范](../../../ANIMATION_RUNTIME_SPEC.md)，不代表旧部署包已有 |
| 固定目标与局部状态样式 | 第二阶段源码已有 InteractionTarget/Visual/.state；边界见下方示例及[规范](../../../INTERACTION_AND_PRESENTATION_SPEC.md) |
| 鼠标组/窗口手势与 WM 空间运动 | v19 已支持受信任系统控制及限定范围过渡；第三方不能借此申请布局权限 |
| 触屏系统操作、客户端交互子树整体变换、GPU 保留层动画 | 不作为当前第三方可用能力；装饰动画走 Paint，不能以永久 loop/tick 模拟 |
| 节点增量布局、DisplayList 分块缓存 | 延后设计；现有 render tree 复用不是这些功能已完成 |
| 安全局部像素修复 | 已有 damage/history/buffer-age 与保守完整回退；不改变布局功能边界 |
| 任意第三方 Wayland 客户端内部主题统一 | 未使用 Prism SDK 的客户端内部内容仍由它维护 |

鼠标拖拽 BSP 比例已由私有 Shell 接入；完整应用抽屉、真实音频等不能靠绘制图标就宣称实现。
Music 当前播放状态/进度仍是业务 demo；UI 设计参考不扩大这些功能的承诺。

## 9. 交付前检查与源码索引

- 根材料随四个主题及 light/dark 变化，未重复 WM 装饰、未硬写普通根圆角。
- 主要动作可由图标理解；必要说明用短文字；状态不只靠颜色。
- 各可见 action 在最窄/最矮目标尺寸中可命中，不能被 clip 或叠放节点挡住。
- 初始 bool/string/number 已声明，pending/empty/error/ready 不发生意外重叠。
- Toggle/Progress/选择指示依赖真实业务结果，没有 UI 自行假成功。
- 不支持的组件、属性、图标或 token 没有进入示例和生产 DSL。
- 背景效果区域数量/形状有界，局部卡片不滥用多层 blur。
- 样式常量来自主题或明确的局部布局设计；没有应用名渲染分支、ImGui 或 Qt widget。

权威入口与可复用示例：

- [DSL 属性与组件 schema](../../../../prism/runtime/dsl_schema.cpp)
- [布局语义](../../../../prism/runtime/layout_engine.cpp)
- [图标绘制实现](../../../../prism/render_skia/vector_icons.hpp)
- [glass 主题](../../../../resources/themes/glass/theme.prism)
- [主题与装饰契约](../../../DSL_THEME_RUNTIME.md)
- [客户端 Scene 规范](../../../CLIENT_SCENE_RUNTIME.md)
- [视觉/BSP 主线](../../../VISUAL_TILING_REFINEMENT_PLAN.md)
- [渲染调度与局部修复边界](../../../RENDER_SCHEDULING_AND_INVALIDATION.md)
- [Music 布局](../../../../demos/demo_player/layout.prism)与[状态/导航 header](../../../../demos/demo_player/ui/header.prism)
- [Music 进度与主操作](../../../../demos/demo_player/ui/transport.prism)、[紧凑曲库](../../../../demos/demo_player/ui/library.prism)
- [Settings 页面与主题/配色指示](../../../../demos/demo_settings/master.prism)
- [Topbar 中心锚定](../../../../prism-topbar/ui/topbar.prism)与[Dock 可见运行组](../../../../prism-dock/ui/dock.prism)

现有 demo 是实现参考，包含自己的业务动作与专用 token；复制外观片段前，应改成
应用实际的 action/binding，并检查所依赖主题包。历史设计文档中的规划不等于当前
schema 能力；新增能力以源代码与相应测试/发布记录为准。

### 窗口控制图标补充（2026-10-04 源码）

通用向量图标新增 `fullscreen`、`restore`、`split-horizontal`、`split-vertical`。
前两者表示全屏/恢复，后两者分别表示左右/上下排列；图标本身不会获取 WM 权限。
平台窗口控制面板用 `controlTint`、`text`、`hover` 等共享 token，禁用项保留图标，
降低 opacity，并隐藏实际 gesture target。固定外层 InteractionTarget 保持输入区域，
防止禁用区域的点击透传。当前 DSL 没有 `enabled` 属性；`Scene::SetEnabled` 是 typed
运行时接口，不应凭该接口名称臆造 DSL 属性。

截至本阶段，Topbar 鼠标手势已实现组沉浸/恢复；私有 LayoutControls 已实现分隔线
拖动及窗口控制，新的触屏操作延期。参见 [布局控制计划](../../../LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md)。

## 独立 Motion 引用（2026-10-04）

主题 schema 3 通过根参数 `motion: "prism"` 引用 `motions/prism/motion.prism`。
视觉材料与运动参数独立；同一套界面可选 prism、subtle 或 instant 参数包。
当前选择在主题文件中配置，Preferences 尚无独立动效选择器。

```prism
InteractionTarget(width: 48, height: 40) {
    Visual(width: 48, height: 40, background: "@controlTint")
        .state(when: "hovered", scope: "target", background: "@accent")
        .transition(property: "background", motion: "control.feedback")
}
```

命名 motion 与 `durationMs/easing` 二选一。Host 在 Scene 准备/主题安装时校验引用；
缺失必须报告失败并保留原主题，不自行选默认曲线。业务模块仍只改 typed binding，
不读取 motion 文件、不自建逐帧 timer。`instant` 的零时长用于立即完成反馈。
窗口/面板整体呈现由 WM 适配层执行，应用不能借此变更 BSP 或输入权限。
当前能力与限制详见 [Motion 规范](../../../MOTION_PRESENTATION_SPEC.md)。

## 文本编辑补充（2026-10-04，Notepad 源码）

本次源码新增正式 `TextField` / `TextArea`，先前章节所述文本输入缺口已由此补齐基础能力。
使用方式、typed edit/close ABI 与现有限制见 [通用文本编辑规范](../../../TEXT_EDITING.md)。
它们提供控件内滚动，不代表通用 Scroll/List、IME 或系统剪贴板已经实现。
新应用参考 [Notepad](../../../../prism-notepad/README.md)；必须搭配本次源码 Host。

### 通用文件操作图标与 Notepad 动效（2026-10-04）

新增合法图标：`document`、`document-add`、`save`、`close`、`arrow-left`、`arrow-right`、
`trash`、`info`。这些是公共向量资源，不依赖字体，也没有应用专用绘制分支；此前列表中
`close` 不可用的说明不再适用于当前源码。

Notepad 使用固定 InteractionTarget + Visual 呈现子树：hover 轻微上移、press 缩放、
focusVisible 独立描边，均引用 `control.feedback`。列表与确认标题的进入效果引用
`panel.visibility`，工作指示只淡入淡出而不虚构进度。确认提示叠放在正文上方，正文
几何不改变。关闭区域立即撤销输入，不用 opacity 代替 visible；没有新增逐帧业务 tick。

`@surfaceRaised` 是四套主题共同提供的实色内容/浮层底色（dark/light 分别定义），适合
阅读纸面与需要遮住底层文字的确认卡片。它不引入额外 backdrop 区域，也不改变其他
材料的透明度；与 `@cardTint` 的半透明分组底色按用途区分。
