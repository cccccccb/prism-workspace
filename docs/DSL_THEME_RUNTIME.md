# DSL 主题与运行时装饰接口

日期：2026-09-26。本规范修订 DESIGN_SPEC 第 10 节及视觉主线的静态主题方案。

## 1. 目标和责任

主题是可替换的数据包。磨砂、透明、半透明、直角是首批材料主题，不是 WM 或 SDK 中的分支。light/dark 是独立配色维度，由同一包的 Palette 声明；0.1.0-8 最终优化版的八种组合已通过实机自动验证，用户已确认本轮布局、配色及主要操作。

| 层 | 输入 | 责任 |
| --- | --- | --- |
| 主题包 | `theme.prism` | 声明颜色、尺寸、材料、普通/聚焦/全屏装饰、控件状态 |
| 统一会话 launcher | 包 ID、colorScheme | 使用共享 DSL 语法解析器编译主题与配色，校验、版本分发与失败恢复 |
| SDK Scene | `ThemeSnapshot`、应用 DSL 中的引用 | 更新现有节点样式、客户端裁剪/命中/背景效果，保留业务状态 |
| WM | `ThemeSnapshot` 中的几何和装饰 | 平铺间距、Shell 保留区、外框/外阴影、跨 surface 背景合成 |
| Skia / compositor 后端 | 已解析的绘制/效果参数 | 执行算法，不选择具体主题 |

WM 不读取主题 DSL，不链接 Scene、客户端 parser 或 Skia。ThemeSnapshot 是 `prism/contracts/theme.hpp` 中的纯值契约，不包含 AST、控件、业务动作或图形句柄。主题编译器只依赖通用语法与契约；应用 DSL 与主题 DSL 使用同一个词法/语法解析实现。

## 2. 唯一主题源与通用能力

包安装在 `share/prism/themes/<id>/theme.prism`。`glass` 为默认；首批 `translucent`、`transparent`、`square` 保持相同 Shell/BSP 几何，方便单独检查外观。当前会话热切换要求候选 ThemeLayout 与现有值相同；改变 Shell 保留带或 BSP gap 的包会明确拒绝，待配置/提交感知事务实现后开放。新会话可选不同初始布局，但其大小应匹配输出。删除旧 JSON、Python 常量生成器和生成的 `prism/contracts/theme_tokens.hpp` 静态样式路径；`prism/runtime/theme_tokens.hpp` 保留为通用引用解析接口。主题不会通过重建或重启窗口生效。

schema v2 增加 `Palette("light")` / 可选 `Palette("dark")`，只允许覆盖根部已声明的同类型 Color/Number token。根定义为 dark 基础，light 必须有对应 Palette；schema v1 包与快照兼容 dark，不支持没有声明的 light。先合并所选 token，再解析材料、装饰、控件和别名，WM/Skia 收到完全解析的数值与颜色，不解释配色名称或维护第二份调色板。主题 ID 与 colorScheme 分别保存在快照和结果中：选择主题保留当前配色，选择配色保留当前主题。

SDK 提供独立能力：tint RGBA、backdrop blur、圆角、边框、内/外阴影、控件悬停/焦点。`material` 属性按语义名称选择材料，例如普通窗口 `window`、Shell `panel`；应用名不参与解析。应用内容结构与动作仍由应用 DSL/业务模块控制。

`@token` 保留为有类型的主题引用，不能在 UI 编译时永久变成字面量。替换主题时重新解析引用，更新样式并触发布局、绘制与合成；不包含节点增量布局或分块缓存优化。业务 `$binding` 和节点 ID 保留。`material` 是静态样式引用，其字段可被显式字面量、`@token` 或业务 binding 覆盖；直接 `SetProperty` 修改某项会解除该项的 token 引用。主题 generation 与 DisplayList generation 分开；`ApplyTheme` 返回是否接受，包括无变化的合法更新。普通窗口根使用 `window` 材料；装饰的 `shape: "window"` 在主题编译时取得同一圆角参数，避免客户端轮廓与 WM 外框不一致。v1 普通装饰使用 `window` 形状；标准应用根不覆写该材料圆角。应用若主动覆写根形状，不能声称仍与全局外框一致，后续应通过 surface 装饰形状接口声明。

主题影响交互轮廓，但透明度不代表点击穿透。材料显式定义 `inputShape`：`bounds` 保持整个圆角材料可交互，`visible` 根据可见内容决定输入范围。全透明、无模糊窗口同样能点击；Shell 材料外的透明边距保持穿透。

外框和窗口外阴影由 WM 绘制；根材料不能重复声明同一外框阴影。局部卡片/控件的阴影由 SDK 绘制。全屏装饰控制 WM 外框；客户端材料仍按自己的 DSL 实现。

## 3. 运行时接口与版本

- SDK：`Scene::ApplyTheme(snapshot, diagnostic)`；ClientApplication/host 将快照传给既有 Scene。
- 应用 ABI：可选尾部 `select_theme`、`select_color_scheme` 和 `on_theme_event`。模块请求包 ID/配色名称，事件携带实际 color_scheme，不自行维护颜色或模糊参数。
- 公开控制：`prism-msg set_theme <id>`、`prism-msg set_color_scheme <light|dark>`、`prism-msg get_theme`，连接 launcher 公共端点；默认使用 `$XDG_RUNTIME_DIR/prism/launcher.sock`。初始参数为 `--theme <id>`、`--color-scheme <dark|light>`；默认 glass/dark。
- PRL1 添加主题请求/结果；PRW1 添加快照/ACK/请求/结果；PWC1 添加快照/ACK。使用有界二进制编码，不能复用 Shell permit 字段传递样式。
- 私有 PWC 仍受 supervisor 同 UID/父进程和 session 身份约束；公开请求不能给自己授予 Shell 身份。

launcher 是当前主题与配色的唯一会话所有者。初始化先编译选定包/配色；WM Ready 后安装快照并 ACK，随后启动 Shell。预热 worker 在前端资源准备完毕后接收主题，ACK 当前版本后才可分配应用。新窗口始终继承当前主题与配色。两种选择共用同一 ACK/恢复事务，不能由 Settings 先改自身 Palette 再异步修改其他窗口。

切换步骤：

1. 按请求的包 ID/配色与需保留的另一维度解析并校验候选主题，不触碰当前运行状态。
2. 分配单调递增 generation，将同一快照发给 WM 和已准备的 active/idle hosts。
3. 各端校验后更新现有对象并返回 ACK；host 保留模块、窗口、业务绑定、资源与节点。
4. 所有参与者成功 ACK 后报告 Applied，通知业务模块当前 ID/名称/配色。
5. 任一端拒绝：用新的 generation 分发原主题与配色内容，恢复成功后报告 Rejected。原主题恢复也失败/超时属于会话控制失效，走已有 supervisor 会话清理，不留下已知的混合主题状态。

同时仅处理一个切换事务；切换中暂停新应用的分配。准备中的 worker 在 Ready 时加入当前事务。退出的窗口从参与集合移除。未响应切换有 5 秒 watchdog；恢复同样有 5 秒 watchdog。编译错误、缺少 token、类型不匹配及当前材料/clip 能力不支持均返回失败，不重启正常窗口。

ACK 表示参数已经安装，不声称跨进程同一显示帧原子切换。客户端参数随后在自己的 surface commit 中生效；几何配置继续按 Wayland configure/commit 处理。

## 4. 校验与边界

主题 schema v1/v2。名字、数量、字符串、文件/帧大小有界；数字必须有限。token 唯一，引用必须存在且类型匹配；未知属性/状态/重复定义拒绝。Palette 只接受已声明同类型 token 的覆盖，拒绝未知配色、未知 token、重复覆盖和错误子块；合并后继续执行完整范围与引用检查。语义范围遵循后端能力：backdrop blur 0..48、圆角 0..256；阴影等另有契约限值。SDK 在应用候选主题前验证当前窗口全部引用与效果边界，失败保持旧主题。

首版主题是一组有类型的声明式原语，不能通过主题向 WM 注入任意程序或任意客户端 widget 树。新增效果先扩展通用契约、后端与 schema，再由主题使用。Gaussian 采样权重等算法常量属于后端实现；用户可配置样式不能藏在算法常量里。

第三方原生 Wayland 客户端的内部内容仍由它负责；本版本不能替未使用 SDK 的客户端自动修改内部配色或保证其内容圆角。主题也不改变业务动作、播放器播放状态和 BSP 拓扑。

## 5. 执行与验收

1. 纯值契约、单一 DSL 编译器、四主题包。
2. Scene 保留引用、通用材料/控件默认、事务更新与输入轮廓。
3. WM 装饰/几何快照，效果缓存纳入主题版本及下层材料变化。
4. launcher 分发、预热继承、ACK/恢复；host 生命周期和 Settings 选择接口。
5. 隔离测试：编译错误与协议边界；Scene 切换保留业务状态；真实统一会话切换四主题、不更换现有 PID、旧/新窗口版本一致。
6. deb 包只装生产代码/资源，再做 Pi 外观与点击验收。

测试与验证程序只放 `tests/`，捕获工具不安装。当前壁纸、动画延期、节点增量布局/分块缓存/零拷贝专项延期安排继续有效。实际完成与验证结果另行记录，不能用规范替代验收。

## 6. 0.1.0-6 完成与验收记录

2026-09-26，第 5 节六步已实施。改造前的 BSP/玻璃视觉版本先提交为 `cf8d9ad`；后续主题改造与规范在该版本基础上实施。

- CTest 28/28 通过；主题编译/边界、协议编解码、Scene 引用与材料更新、业务状态/输入保留及 WM/BSP 快照均有独立检查。最终透明主题调色后，相关编译器/模板测试再次 2/2 通过。
- 诊断与 Release 的真实 V3D 统一会话均完成四主题切换；既有六个 worker PID 保持，预热 worker 创建的新窗口继承最新主题。未知包不改变当前版本，缺少应用必需 token 被 host 拒绝并完成恢复；冲突请求被拒绝。结果分别在 `dist/validation/prism-v6-theme/` 与 `dist/validation/prism-v6-release/`，探针仅位于 `tests/probes/`。
- 0.1.0-6 arm64 deb 已安装到 Pi。物理 DRM 会话完成四主题切换、协议门槛与截图检查；用户现场答复“四种主题切换及点击都正常”，确认全局效果及主要交互。测试/捕获工具未进入安装包，WM 链接边界检查通过。

发布校验、现场证据与升级回收记录见 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)。后续开放热切换 `Layout` 必须先实现覆盖 Shell configure/commit 的事务，不能取消当前拒绝检查来宣称支持几何热切换。

## 7. 0.1.0-8 配色验证状态

2026-09-26，schema v2 与独立 light/dark 配色已完成构建，0.1.0-8 最终优化版已重新安装到 Pi。四材料主题 × 两种配色的八种组合均通过实机自动切换验证，最终结果见 `dist/validation/prism-v8-preferences/appearance-optimized/results.json`；首版结果保留在同目录的 `appearance/results.json`。最终版真实 UI 的 Light/Dark 截图检查通过。

用户现场确认“布局与配色满意，操作正常”，本轮最终版布局、配色及主要操作验收完成。第 6 节和 0.1.0-7 的现场答复仍属于此前版本历史验收；动画、节点增量布局、分块缓存、零拷贝专项以及改变 Layout 的热切换仍按本文边界延期，不属于本轮验收范围。
