# Prism 界面系统：控件、层级与任务面板

日期：2026-10-05。状态：设计规范 + 分阶段实施；不是全部已发布的 API。

## 1. 来源与范围

用户确认的《Prism · 文本、层次与间距评审提案》作为本阶段基准。
[Figma 节点 25:107](https://www.figma.com/design/QISSYppHaujXBSqJm1bT3D/Prism?node-id=25-107)。
本次依据用户提供的 18560×18908 PNG；Figma MCP 额度用尽，未取得变量/Auto Layout
及字体度量。仓库保存[控件](design/interface-system/01-controls.png)、
[浮层](design/interface-system/02-popup.png)、[文件任务](design/interface-system/03-file-task.png)、
[反馈](design/interface-system/04-feedback.png)、[动效](design/interface-system/05-motion.png)
五张缩放审阅图，仅作文档参考，不打包为运行时 UI 资产。

此规范补充现有 Concept B 应用布局，不立即替换所有 Demo。设计图蓝色说明、
Slider、跨窗口 Popup、系统文件服务及透视翻转均不能仅凭图示视为已支持。

## 2. 源码能力核对（更新至6a）

以下为工作区源码能力；已安装的正式v26不包含5a—6a，部署记录另行确认。

| 设计要求 | 当前依据 | 差距 / 实施入口 |
| --- | --- | --- |
| Text、Button、IconButton、TextField/TextArea、Progress、Toggle | runtime/dsl_schema.cpp | 已有节点；不等于完整通用控件库 |
| 悬停、按下、捕获、focusVisible、disabled | scene_input.cpp、scene_state.cpp、render_tree.cpp | 已有输入状态、状态规则和主题焦点描边；本轮接 DSL enabled |
| 状态属性与主题 Motion | ANIMATION_RUNTIME_SPEC、MOTION_PRESENTATION_SPEC | 复用已有 .state/.transition，不新增第二套动画驱动 |
| 字号、间距、内边距、截断与窄窗条件 | dsl_schema.cpp、layout_engine.cpp、text_shaper.cpp | 已有 font/spacing/padding、viewport 条件；第二阶段增加 lineHeight 与显式分行，字重/自动折行/字体回退待实现 |
| Checkbox、Radio、Slider、Segment、可滚动列表 | CONTROL_VALUE_CONTRACT、SCROLL_VIEW_CONTRACT | 有类型值与键鼠流程已接入；当前Slider水平，滚动指示条不可拖动 |
| Popup/Menu 与焦点作用域 | POPUP_MENU_CONTRACT、POPUP_TARGET_LIFECYCLE | 锚定、关闭、面板子菜单与Host原生子层已接入；侧向级联待实现 |
| 应用内未保存确认 | OWNER_TASK_PROVIDER_CONTRACT、NOTEPAD_TASK_AND_CLOSE_CONTRACT | 共享Confirmation、owner局部模态及Notepad异步Close已接入 |
| 系统文件任务 | FILE_TASK_PROVIDER_CONTRACT | 共享OpenFile/SaveFile/SelectDirectory单项选择已接入；业务仍负责真正读写，多选待实现 |
| Owner轻量反馈 | OWNER_FEEDBACK_CONTRACT | 6a接入Info/Success/Error、恢复动作与可见期限；一条owner反馈，不是全局通知中心 |
| 会话确认、Tooltip | 图04提案 | 6b独立定义受信入口与非抢焦点说明，不由反馈API取得系统权限 |
| 下缘翻起与 owner 退后 | 有平移/缩放/透明度与 WM 呈现接口 | 透视、裁剪、命中、缓存和中断行为需新增能力；不得伪造 API |

## 3. 责任边界

- DSL/Scene：控件值、局部布局、状态呈现、键盘顺序、局部焦点作用域。
- Host：请求身份、前端生命周期、业务动作/状态、主题安装、owner 生命周期联动。
- WM：可信窗口归属、跨窗口定位/层级、跨 surface 输入约束、呈现变换和命中一致性。
  不解释应用 DSL，不绘制文件列表，不硬编码 Demo。
- 系统文件服务：打开/保存位置/目录任务、过滤/多选、权限错误与覆盖选择、取消结果。
- 业务模块：提供请求参数、消费结果、保存正文、处理业务失败；不凭文本或样式获得系统身份。
- 主题与 Motion：材质、颜色、尺寸 token、状态和开合运动；行为不依赖某个材质名称。

## 4. 基础控件契约

### 4.1 通用状态

`enabled` 是 Boolean，默认 true，可绑定 `$canSave` 等 typed Boolean 状态。
有效启用 = 自身与所有祖先 enabled 均为 true。禁用保留布局和内容；`visible` 管隐藏。
禁用立即撤销按下/捕获/拖动和不再有效的焦点，拒绝后续激活；重新启用不能复活旧按下。
禁用子树不参与命中，不自动吞掉对底层的点击；它不是模态输入屏障。
最新逻辑状态可拒绝旧提交快照中的动作；快照本身保持不可变。

`enabled` 不可动画，也不可在 .state 内赋值，避免输入状态与自身启用形成循环。
控件禁用外观通过现有 `.state(when: "disabled", ...)` 和主题 token 声明，
此阶段不自动强加全局透明度。焦点描边是独立提示，不能以选中背景代替。

C++ `Scene::SetEnabled` 与 DSL 属性更新使用相同状态与失效路径。布局不应因 enabled
变化重新计算；输入快照、状态视觉与取消需要更新。纯重复赋值不应产生新帧。

最小用法（要求更新后的 Host，旧 v22 不支持）：

```prism
VStack(spacing: 8, enabled: $available) {
    Button("保存", action: "document:save", enabled: $canSave, height: 36)
    Button("取消", action: "document:cancel", height: 36)
}
```

`available` 与 `canSave` 都声明为 Boolean binding，由业务发布。应用初始化时明确
写入可用性；未赋值采用默认 true，因此提交入口需要防护时初始 binding 必须是 false。
此例只展示语义，不代表完整按钮的状态样式已统一。

### 4.2 值与激活（后续）

区分语义值（checked/value/selection）、输入状态（hover/pressed/focus）、业务状态
（loading/error）。具体控件定义键盘行为：Button 为 Enter/Space，Radio 为组内方向键，
Slider 为方向键/步长/Home/End；不把一条通用按键规则套给所有控件。
Slider 显式定义最小/最大/步长、预览变化与提交/取消；Progress 只读并反映真实业务值。

### 4.3 文本与密度

沿用设计尺度建议：页面标题 20/18、正文 14、辅助 11，局部间距 4/8/12/16。
这些是逻辑尺寸建议，尚无统一 token 的尺寸不能写成不存在的 @token。
组内距离小于组间距离；按钮按内容和命中区定尺寸；少量行不按 flex 均分整个窗口。
长中文、长路径、空值、错误说明都参与布局预算，主要操作固定可达，列表独立滚动。
图标适合播放/音量/导航；保存/取消/替换等任务决策保留明确文字。

原件中黑底 Segment、低对比文字与不明显的关闭态轨道需校正后才能作为组件标准。
默认/悬停/按下/焦点必须可区分；禁用保留原因；选中不能只靠颜色。
中文字体回退和输入法是独立能力，样稿使用中文不证明运行时支持完整。

## 5. 浮层与阻挡范围（待实现）

| 界面类型 | 归属 | 输入与关闭 |
| --- | --- | --- |
| Popup | 锚点与 owner | 默认非模态；外点关闭并消费该点击；拖动期间捕获；Esc 关闭 |
| Menu | 命令列表或组合面板 | 执行命令关闭，连续调值保留；Esc 逐级返回；关闭归还焦点 |
| 应用确认 | 当前应用窗口 | 只阻挡该窗口；Esc 默认安全取消；保存失败保留正文与确认 |
| 系统文件任务 | 系统服务 + 发起窗口 | 阻挡 owner；其他窗口可操作；切换窗口不取消 |
| 会话确认 | 受信任系统入口 | 仅必要会话操作阻挡桌面；应用不能伪造 |
| 轻量提示 | 应用或系统反馈 | 不抢焦点；错误持续可恢复；可撤销提示支持键盘访问 |

定位按下方→上方→横向避让→限制高度/滚动；窄窗可转贴边面板，子菜单保留返回入口。
异形连接颈只表达锚定关系，裁剪、模糊边界与命中必须一致；Square 主题可使用直角实色。
模态作用域保存返回焦点；目标销毁时回到所属区域稳定入口。避免层层叠加模态。

## 6. 系统文件任务（待实现）

请求包含可信 owner/request 身份、任务种类、初始位置、过滤、多选及建议文件名。
结果区分成功、取消、失败，不用空字符串兼任三种状态。路径、句柄或授权 token 的
选择留给服务协议评审，不能把任意路径字符串当作权限授权。

保存位置选择与正文写入责任分别定义，覆盖授权须绑定所选目标；实际提交仍须处理
目标变化/权限失败。加载不虚构进度，空目录仍可保存。错误保留位置、文件名与正文。
owner 关闭取消未完成请求，迟到结果不能投递给复用窗口。覆盖确认在任务面板内完成。

## 7. 动效与性能（待实现）

草案展开约 260ms（前 100ms 抬起，随后展开）、收回约 180ms，最终时长由主题配置。
倾斜只用于过渡，稳定时文字与按钮平直。真实准备完成才允许业务交互；准备失败显示
可恢复状态。关闭/取消始终有明确处理路径，不以动画完成代替业务完成。

动画使用单调时间与当前时间采样，沿用不可变快照和渲染线程，不增加忙循环。
优先复用内容缓存与合成变换；背景压暗不自动叠加多层模糊。透视能力需先定义变换、
裁剪、逆映射与损伤范围，不能在 Demo 私加 Skia 绘制。
reduced motion 取消翻转与位移，短淡入或立即呈现，保持相同输入和任务语义。

## 8. 执行顺序与验收

1. **基础状态入口（本轮）**：enabled 字面量/绑定/运行时统一，禁用取消与快照测试。
2. **文字与控件呈现**：定义排版尺度和主题状态配方；补齐焦点/禁用外观、长文本布局，
   建立独立控件样例与明暗截图，不把样例装入生产桌面。
3. **值控件与滚动**：Checkbox/Radio/Slider/Segment、键盘、值变更/提交/取消、可滚动容器。
4. **局部浮层与焦点域**：先 Popup/Menu/应用确认，验证外点、嵌套、窗口缩放和返回焦点。
5. **可信跨窗口与文件服务**：owner/request 协议，再打开/保存/目录选择，接 Notepad。
6. **提示与会话确认**：统一恢复动作、撤销与安全取消语义。
7. **主题化任务动效**：稳定交互链通过后接翻起/收回，核对中断、缓存和帧时间。

每阶段交付 schema/契约、最小可运行示例、对应测试与更新后的能力表。
验收覆盖宽/窄/矮窗、长中文/路径、明暗与四种材质、键盘与鼠标、父子禁用、焦点恢复、
快速取消、owner 消失、多窗口并发和主题切换。触屏继续延期。
静态图不能证明键盘可用、对比度合规或动画流畅；这些需要实际运行验证。

## 9. 第一步实施记录（2026-10-05）

已完成：schema 注册 Boolean `enabled`、Blueprint 缓存/读取、SetProperty/SetBinding
到现有 SetEnabled 路径、事务提交携带本地值，以及 C++ 修改值在主题重建中的保留。
属性枚举追加，已有枚举编号不变。未引入 Qt、应用专用渲染或新的 WM 分支。

`dsl_enabled_test` 覆盖字面量、绑定、父子启用关系、指针捕获取消、旧快照不变性、
键盘按下期间原子禁用、主题重建、非法类型与非法状态/动画声明。
结合 scene_interaction_test、scene_state_test、viewport_conditions_test 共 **4/4** 通过。
禁用更新不增加布局次数，重复值不触发新 Build；不据此推断完整桌面帧率。
代码规范检查通过，自有生产文件最大 638 行。证据位于
`dist/validation/interface-system-step1/`。

本轮未提交 git、未打包、未替换 VNC；运行中的 v22 不包含新属性。
下一步为第 2 阶段：文字排版契约与控件主题状态配方，包含独立样例和实际截图验收。

## 10. 第二阶段实施记录（2026-10-05）

已接入 `lineHeight`，Text 显式分行，编辑行框与选位统一；补齐字符串 `\r` 转义，
CRLF 正确作为一个换行。Button 内置标签传递属性、binding 和主题引用。
四主题新增间距/行高/控件状态 token，配方使用原有 InteractionTarget/Visual/state；
测试与截图位于独立 fixtures/probes，不进入生产包。

完整接口、主题 token 与限制见 [文字行高与控件状态配方](INTERFACE_CONTROLS_AND_TYPOGRAPHY.md)。
本轮对照稿核对的是按钮状态、输入框、分区与行框；未将整张评审海报实现为应用。
默认/悬停/按下/焦点/禁用由实际 Scene 事件生成，四材质 × 明暗 × 宽窄共 16 组
Skia raster 截图。样例以英文验证当前字体支持，不声称完成中文字体回退或 GPU 性能验收。

7/7 回归通过：theme_compiler、scene_state、notepad、demo_layout、dsl_enabled、
text_line_height、interface_controls。样例初版按钮文字偏左已修正为居中；
首轮 CRLF 测试暴露 `\r` 被吞成普通 r，已补正确转义并通过测试。
代码规范通过，当前自有生产最大文件 641 行；证据在
`dist/validation/interface-system-step2/`。本轮未提交、打包或替换 VNC。

下一阶段：值控件与滚动的类型化接口，优先定义 Checkbox/Radio/Slider/Segment 的
值、键盘与提交/取消规则，再实现控件；不直接跳到文件任务或透视动效。

## 11. 第三阶段 3a：值交互核心（2026-10-05）

第 3 阶段细分为值会话核心、DSL/Scene/Host 通路、控件呈现及滚动三个子阶段。
类型、提交/取消及键盘规则见 [值控件契约](CONTROL_VALUE_CONTRACT.md)。
本步实现 Boolean/Number/Choice 三类值域及 ControlValueSession；预览只影响局部值，
提交产生业务提议，新修订同步取消旧交互，过期输入不能结束新交互。

当前没有新增可用 DSL 控件，没有改变 Toggle 的已有 action，也未扩展模块 ABI。
接下来按 3b 接入 Scene 和 typed value callback，以 Checkbox 贯通真实业务回写；
再完成组选择、Slider 捕获与键盘规则，最后接主题呈现与滚动。

本步 4/4 回归通过：control_value、dsl_enabled、text_line_height、interface_controls。
格式/行数检查通过；数值测试包含 NaN/Inf 拒绝及极端范围的有限结果检查。
证据位于 `dist/validation/interface-system-step3a/`。本步未提交、打包或替换 VNC。

## 12. 第三阶段 3b 首步：Checkbox 真实值通路（2026-10-05）

新增 Checkbox DSL 语义容器；外观继续使用 Visual/图标/文字和主题 token，WM 及
Skia 没有应用或 Checkbox 专用绘制分支。鼠标释放、Tab/Space、捕获取消与 checked
修订校验接入已有输入快照路径；原 Toggle/action 行为不变。

Scene ControlEdit → SDK OnControlValue → Host → ModuleSession → C ABI 尾回调
on_control_value → set_binding → Scene。Checkbox 是离散提交，不在按压阶段发预览；
按压取消不产生业务事件。业务不确认时 checked 保持旧值。

边界与可复制 DSL/C 示例见 [值控件契约第 8 节](CONTROL_VALUE_CONTRACT.md)。
旧模块没有值回调时安全忽略；新回调不完整时不读取指针。测试模块、DSL fixture 和
视觉 probe 仅放在 tests 下，不进入运行包。接下来实现 Radio/Segment 组选择，再接
Slider 连续预览/提交/取消；滚动和完整四控件视觉验收仍在后续阶段。

正式 prism-app-host 构建通过，11/11 回归通过（checkbox、control_value_abi、
checkbox_visual、control_value、dsl_enabled、text_line_height、interface_controls、
scene_interaction、scene_state、legacy_module、launch_module）。保护页测试验证不完整
on_control_value 尾字段不被读取；值桥覆盖 Bool/Number/String、阶段/原因、借用数据
复制、错误线程及退出后抑制投递。新增 Checkbox 的交互会话按需分配，普通节点不内嵌
完整值会话；没有新增周期任务或 WM 依赖。

四材质 × 明暗 × 320/640 宽度共 16 场景、32 张选中/未选中 Skia raster 截图已生成，
人工查看 Glass 深色宽窗、浅色窄窗与 Square 浅色窄窗。首轮样例使用未支持的
borderColor state 已改为受支持的背景色悬停；编译期间补充接口引起的新旧对象混用
链接失败已通过最终源码重编译解决。原始日志和最终通过记录均保留在
`dist/validation/interface-system-step3b/`。代码规范及 git diff --check 通过。
本轮未提交、打包或替换 VNC；没有将 raster 截图当作真实 Wayland/GPU 性能验证。

## 13. 第三阶段 3b：Radio/Segment 组选择（2026-10-05）

新增 RadioGroup/Radio、SegmentGroup/Segment，组持有 selectedKey/action，选项只声明
固定 key 和 Visual 外观。共享单选值、一次 Tab 停靠、方向键/Home/End、Space/Enter、
禁用/隐藏跳过、重复选择抑制、旧捕获取消与 typed String 业务回写已接入。

只读 selected 状态区分选择与焦点；选项集合在创建后固定，尚不支持动态选项列表。
未知值拒绝整次投影；空值代表等待业务状态、整组暂不可交互。完整规范、DSL 和独立 C
业务示例见 [值控件契约第 9 节](CONTROL_VALUE_CONTRACT.md)。没有新增 WM 绘制逻辑、
ABI 字段或持续刷新任务。下一步为 Slider 连续值预览/提交/取消，再进入滚动容器。

正式 Host 构建及 11/11 回归通过：choice、choice_visual、checkbox、checkbox_visual、
control_value、control_value_abi、scene_interaction、scene_state、dsl_enabled、
text_line_height、interface_controls。共享绑定的域冲突会整体拒绝；测试覆盖真实 C
模块接受/拒绝后的呈现状态、Tab 停靠、方向键循环及禁用/隐藏项跳过。

组容器默认按主轴内容尺寸排列，显式 flex 才拉伸；沿用原容器默认平分剩余空间会造成
过大空隙，本轮已在新组类型上修正，旧容器规则保持。四材质 × 明暗 × 320/640 宽度
生成 16 场景、32 张初始/选择后的 Skia raster 截图，人工查看浅色窄窗、深色宽窗和
Square 窄窗。预览最终使用 384px 高度，减少无内容留白，视觉测试再次通过。

代码规范（352 个生产文件，最大 650 行）及 git diff --check 通过。证据位于
`dist/validation/interface-system-step3b-choices/`。未提交、打包或替换 VNC；没有将
独立 raster 图作为完整桌面 GPU 或远程交互性能验收。

## 14. 第三阶段 3b：连续值事件投递与取消准备（2026-10-05）

在 Slider 输入接入前，先将现有离散 Commit 迁移到统一 `ControlValueDelivery`。每条
事件携带所属 UiLoadId；投递前验证 Scene 节点、action、权威值修订，避免手势或业务
回调换 UI 后，旧事件仍被发送。回调复制、非递归投递及接收者撤销边界同时落实。

连续数值核心的预览可由该链路投递；已被观察的预览失效时通知一次 Cancel，保留原
事务信息。Scene 控件取消自身捕获仍是输入层责任。完整规则及测试边界见
[值控件契约第 10 节](CONTROL_VALUE_CONTRACT.md)。

本步骤不增加 Slider DSL 节点，不变更 demo 布局。下一步继续 Slider 的数值域、
提交快照几何、拖动与键盘规则、滑轨/滑块主题配方；之后进入滚动容器。

正式 `prism-app-host` 构建通过，相关回归 **8/8**：control_value_delivery、control_value、
control_value_abi、checkbox、choice、scene_interaction、scene_state、dsl_enabled。
规范检查通过（355 个生产文件、124 个测试文件；生产最大 650 行），git diff --check
通过。证据位于 `dist/validation/interface-system-step3b-delivery/`。本步未提交、打包或
替换 VNC，也未以核心数值测试代替尚未实现的 Slider 实机交互验收。

## 15. 第三阶段 3b：Slider 水平值控件（2026-10-05）

本轮将 Slider schema、固定 minimum/maximum/step、value 绑定、成功提交轨道快照、
鼠标捕获、键盘按下/重复/释放、Esc/失焦/值替换/不可用取消接入正式值事件通路。
Scene 的 retained stream 独立于渲染，生命周期事件经过统一 SDK 队列；业务模块仅接收
Number 的 Preview/Commit/Cancel，不持有图形或平台对象。

设计以控件评审图 01 为准：低对比 4px 轨道、主题蓝色填充、小尺寸 16px 滑块、44px
独立命中范围、清楚的标签和右侧数值；示例包含禁用状态与无滑块的只读 Progress。
外观为 DSL 的 track/fill/thumb 三个 Visual 和焦点状态配方；四材质复用既有共享
尺寸与明暗颜色 token。沿用普通 DisplayList，不新增 Skia 或 WM 专用控件分支。

值预览只更新绘制快照中的部件几何，保持父布局和输入几何稳定；不添加周期 timer。
业务文本回写仍遵循现有文本布局规则。规范、可复制 DSL 和独立 C 业务样例见
[值控件契约第 11 节](CONTROL_VALUE_CONTRACT.md)。暂仅水平单滑块；滚动容器继续作为
下一步，动态范围、垂直/RTL/双滑块与触控没有混入本轮范围。

正式 Host 构建与最终 **14/14** 回归通过：slider、slider_visual、control_value_delivery、
control_value、control_value_abi、checkbox、choice、scene_interaction、scene_state、
scene_input_snapshot、scene_region_transaction、dsl_enabled、text_line_height、interface_controls。
Slider 回归包括真实 C 模块确认/预览回退、量化去重、端点、迟到键重复/释放、域校验、
禁用/隐藏/失焦/Preflight 取消，以及纯数值预览不增加 layout 次数和输入快照版本。

四材质 × 明暗 × 320/640 宽度共 16 个场景，生成 32 张初始/拖动预览截图。人工查看
明暗窄窗并排、深色宽窗拖动和 Square 浅色窄窗，保持细轨道、小滑块、清楚的名称与
右侧读数、独立分隔线和紧凑间距。没有以 raster 截图宣称真实 GPU/VNC 性能已验收。

首轮新增 Slider token 触发既有主题 128 条目限制，已改为复用相同尺寸/颜色角色的
既有 token，没有放宽协议限制。区域事务回归还发现先前 ShapeLines 对 NaN 度量的
std::max 运算会掩盖非法输入；现已在合并前校验有限且非负的宽高，恢复原子拒绝。
原始失败日志与最终通过日志均保留于 `dist/validation/interface-system-step3b-slider/`。

规范检查通过（358 个生产文件、127 个测试文件，生产最大 668 行），git diff --check
通过。契约与项目 SKILL 已同步；未提交、打包或替换 VNC。下一步为滚动容器。

## 16. 第三阶段 3c：纵向滚动容器（2026-10-05）

本轮新增 ScrollView：单内容自然高度布局、强制绘制/输入/效果裁剪、可选主题 Visual
位置指示条、鼠标滚轮、嵌套剩余滚量传递与 Tab 自动显露。固定标题、分隔线、底栏保持
在视口外，内容行紧凑排列；测试样例沿用评审图 01 的字体、颜色层次与细指示条。
完整结构、前端状态 API、输入和兼容边界见 [ScrollView 契约](SCROLL_VIEW_CONTRACT.md)。

纯位移更新 retained 子树几何和 Paint/Composite，不重做布局；内容/尺寸改变仍走
既有布局，夹紧滚动位置。主题、Preflight 和保留节点区域事务继承位置。旧快照位置
不能激活已移走内容，滚动取消旧按压与 Slider 流；连续滚轮允许沿显示快照续传。

当前为纵向鼠标基础版：位置指示条不支持拖动，无惯性、水平滚动和虚拟化；编辑器
继续自身滚动，不把这些能力写成已完成。本步没有修改应用业务或 WM，也没有打包或
替换 VNC。后续沿主线进入锚定 Popup/Menu 的所有权、输入与主题组合契约。

正式 Host 构建通过；scroll_view、scroll_visual、slider、scene_input_snapshot、
scene_region_transaction、scene_state、scene_interaction、text_line_height、interface_controls
共 **9/9** 回归通过。四材质 × 明暗 × 320/640 宽度（280/420 高度）生成 32 张首尾
截图；检查明暗短窗与 Square 宽窗，修正样例行内垂直居中并使用主题圆角。
规范检查通过（360 个生产文件、129 个测试文件，最大 668 行），git diff --check 通过。
构建、回归与截图证据位于 `dist/validation/interface-system-step3c-scroll/`。
截图验证使用 Skia raster，不代表远程画面或 GPU 性能已验收。

## 17. 第四阶段 4a：Popup/Menu 定位与生命周期核心（2026-10-05）

按评审图 02 开始临时层主线。本轮新增独立 prism_popup_core：下方优先、上方回退、
水平避让、限高及窄窗贴边定位；owner/scene/锚点 generation 与不复用 token 的菜单链。
支持替换分支、逐级 Esc、命令关闭整链、锚点失效关闭和返回焦点候选记录。

接口与后续输入要求见 [Popup/Menu 契约](POPUP_MENU_CONTRACT.md)。本轮尚无 Popup/Menu
DSL、Scene 临时层或生产输入适配，外点消费、实际焦点恢复、菜单键盘导航和材质轮廓
属于后续步骤，不以纯核心测试替代验收。未修改 demo、打包、提交或替换 VNC。

独立核心与 popup_core_test 构建、测试通过。定位测试覆盖 81 组负原点与小数尺寸组合，
另含翻转、限高、贴边和非法输入；生命周期覆盖旧 token 与节点 generation 隔离。
代码规范检查通过（363 个生产文件、130 个测试文件，生产最大 668 行）。验证记录位于
`dist/validation/interface-system-step4a-popup/`；本步没有声称视觉或实机交互已验收。
下一步将该核心接入 Scene 临时层与 DSL，再验证完整快照输入和外点消费。

## 18. 第四阶段 4b：窗口内单层 Popup（2026-10-05）

本轮将 popup core 接入正式 Scene：根末尾声明 `Popup("trigger-action", width, height)`，
触发 action 由前端消费，内部普通命令照常发给业务并关闭，值操作保留浮层。
声明不参与正文布局，随后按锚点独立定位；限高内容使用 ScrollView。主题、Preflight
保留有效 token，锚点失效/滚动移位、无法定位、owner 失焦或关闭结束浮层。

提交输入快照带 popup token；旧帧不能激活已关闭或新一代浮层。外点按下与对应释放
按 source/button 配对消费，Esc 重复/释放消费，Tab 限于浮层作用域。关闭取消捕获并
返回仍可用的触发控件；失效入口清空焦点。透明 owner 内补充输入区域以接收外点。

设计样例按图 02 的声音面板，使用标题/右侧读数/辅助说明、细滑轨、分隔线和设置入口。
四材质 × 明暗 × 320/640 宽度生成 32 张闭合/展开图，人工检查明暗短窗与 Square
宽窗，修正浅色默认按钮文字对比、垂直居中和指示条留白。仅使用普通 DisplayList，
没有私加 WM/Skia 控件分支；没有宣称连接颈、跨 surface Popup 或 Menu 已完成。

Host 构建通过，popup_core、scene_popup、popup_visual、scroll_view、slider、
scene_input_snapshot、scene_region_transaction **7/7** 回归通过。覆盖外点不穿透、旧帧
拒绝、值交互保持展开、拖动到面板外正常提交、Esc、锚点裁剪/滚动、极小窗口收回及
主题/Preflight 保持。首轮测试误用了仅查询 seat 0 的 FocusedAction，改为检查实际
触发节点的焦点状态；日志保留，不把测试修正记为生产输入故障。

契约和项目 SKILL 已同步。代码规范通过（365 个生产文件、132 个测试文件，最大669行），
git diff --check 通过；证据在 `dist/validation/interface-system-step4b-popup/`。未提交、
打包或替换 VNC。下一步为 Menu 导航与子菜单，再处理通用轮廓和跨 surface 定位。

## 19. 第四阶段 4c：Menu 导航与替换面板式子菜单（2026-10-05）

本轮新增 Menu/MenuItem/MenuBack，沿用窗口内 Popup 定位和根末尾声明。按图 02 的窄窗
方案，子菜单替换面板内容并保留明确返回入口；当前不展开横向级联窗口。

命令行支持上下/Home/End 导航、跳过禁用项、右键进入、左键与 Esc 返回；Enter/Space
沿用按下/释放激活。Tab 在命令和值控件间切换，Slider 等值控件保留方向键；普通业务
命令关闭整链，值操作保持展开。子菜单父层保留状态，返回聚焦原入口；每次进入/返回
更新呈现 epoch，旧父层截图同样不能继续激活。owner/祖先锚点失效关闭整链，隐藏父层
的属性更新也检查当前菜单依赖，避免无新输入时悬挂。事件 seat 来自原始输入。

校验拒绝循环锚点、菜单外行、无 action 的 MenuItem 和缺少 MenuBack 的子菜单。
详细语法、实际支持范围和完整配方见 [Popup/Menu 契约第 4c 节](POPUP_MENU_CONTRACT.md)。

Host 构建通过；scene_menu、menu_visual、scene_popup、popup_visual、popup_core、slider、
scroll_view、scene_input_snapshot、scene_region_transaction **9/9** 回归通过。
覆盖键盘与点击返回、旧父帧拒绝、禁用跳过、整链关闭、Checkbox 保留、Slider 方向键、
主题/Preflight 以及暂隐祖先失效。四材质 × 明暗 × 320/640 宽度生成 48 张闭合/父级/
子级截图，检查明暗短窗与 Square 宽窗。首轮样例误用不可状态化的 borderWidth，改为
恒定描边 Visual 加 opacity 状态；同时修正命令行内边距，未放宽生产 schema。

代码规范检查通过（366 个生产文件、134 个测试文件，生产最大 669 行）；git diff --check
通过。证据位于 `dist/validation/interface-system-step4c-menu/`。文档与项目 SKILL 已同步；
未提交、打包或替换 VNC。后续继续通用浮层轮廓/命中/模糊一致性，以及 Host 跨 surface
定位；不以 raster 截图宣称上述能力或 GPU/远程性能已验收。

## 20. 第四阶段 4d：统一矩形/圆角几何（2026-10-05）

本轮先统一现有轮廓，不添加半成品连接颈。新增纯契约 rounded_region.hpp：有限数值
检查、统一半径归一化、点包含、水平跨度及圆角交集的逻辑像素中心栅格化。
Scene 实时/提交快照命中、RenderTree 半径、输入区域及 effect 包含/归一化共用规则。
Wayland region 与 Scene 交集输入掩码共用栅格化，不再分别 ceil 左界/floor 右界而丢掉
应命中的边缘像素；合并相同跨度行，减少区域 add 调用，不额外增加刷新或 timer。

新像素测试发现超尺寸半径在 effect 中已夹紧，而 InputRegions 仍返回原始半径，已
统一修正。保留 v1 模糊协议对非对称圆角交集的拒绝，不用外包矩形扩大效果。
详见 [圆角轮廓契约](ROUNDED_REGION_CONTRACT.md)，其中明确 AA 覆盖与二值命中的差别、
外阴影与外点关闭屏障的不同职责，以及连接颈所需的后续协议工作。

Host/Wayland 适配器构建通过；rounded_region、rounded_scene、visual_scene、scene_popup、
scene_menu、menu_visual、scene_input_snapshot **7/7** 回归通过。96 个单形状/交集场景
逐像素验证；四种半径对照真实 Skia raster 的全覆盖/全透明像素、实时与快照命中、
输入掩码和 effect 几何。已有不支持裁剪与事务回滚回归保持通过。

代码规范通过（367 个生产文件、136 个测试文件，最大 669 行），git diff --check 通过。
原始失败、修正后通过、构建与规范记录保留于 `dist/validation/interface-system-step4d-contours/`。
文档与项目 SKILL 已同步。未提交、打包或替换 VNC；未声称 GPU 背景采样、连接颈或
跨 surface 定位已完成。下一步定义可由绘制/输入/effect 共同消费的通用浮层轮廓扩展。

## 21. 第四阶段 4e：通用轮廓基础契约（2026-10-05）

本轮新增纯契约 `Contour`：单一闭合简单多边形，1/256 逻辑量化、最多 256 顶点，
坐标与外包尺寸受 8192 上限约束。路径准备支持直线与三次曲线，采用有限弦段误差
和有界细分，量化后再验证拓扑。非法自交、自接触、折返、退化或超预算明确拒绝。
纯编解码定义独立 payload v1，严格 little-endian、长度与版本校验，最多 2056 字节。

绘制、点包含与凹多边形多跨度扫描都消费同一份准备结果；二值掩码按像素中心判断，
交集最多八个轮廓，矩形生成最多 65,536 个。DrawList 新增完整轮廓填充、内侧描边、
内外阴影及裁剪，接入 Skia 共享回放、ink bounds、透明度组和损伤比较。
连接颈与面板作为一个轮廓提交，避免叠加透明色和内部边线。详见
[通用轮廓契约](SURFACE_CONTOUR_CONTRACT.md)。

Host 构建通过；contour_contract、contour_render、skia_raster、skia_gles_parity、
skia_damage、visual_scene、rounded_region、rounded_scene、scene_popup、scene_menu、
menu_visual 共 **11/11** 回归通过。覆盖 wire 截断/超范围、曲线误差和顶点预算、
像素中心边界/凹形/反向轮廓及真实矩形预算拒绝；真实 Skia 验证连接处单次 alpha、
内描边、阴影、裁剪和局部修复。明暗轮廓样例仅作为几何证据，不替代生产 Popup 设计。

GLES gate 报告 Broadcom V3D 4.2.14.0。确定区域的填充、裁剪、描边像素与 CPU 对照；
Gaussian 阴影仅对选定样点允许至多 16/255 通道差异，不宣称整图后端一致。
非均匀缩放加透明度组的 GLES 局部修复与同后端完整回放最大通道差异为 0，实际改变
81 个像素，修复范围外 432 个像素保留不变。这些数据验证回放正确性，不是 FPS 结论。

规范检查通过（374 个生产文件、138 个测试文件，生产最大 669 行），git diff --check
通过。构建、回归、GLES 设备信息和明暗截图位于
`dist/validation/interface-system-step4e-contour/`。文档、DrawList 契约与项目 SKILL 同步。

额外的独立 CMake 配置失败：系统
`/usr/share/cmake-3.31/Modules/CMakeParseImplicitIncludeInfo.cmake` 含有损坏文本及五个
NUL 字节，编译器探测阶段即报语法错误。现有缓存构建不受本次配置失败影响，Host
及上述回归均已通过。直接以 g++、标准 C++20 和 Prism 契约头编译四个轮廓单元及
契约测试也通过，没有链接 Skia、Wayland 或 Qt。环境故障日志与直接编译记录分别
保留为 `standalone.log`、`cmake-environment.txt` 和 `standalone-direct.log`；系统模块
尚未修复，不能将新目录 CMake 配置记录成通过。

后续依次实施：

1. Wayland surface effect interface v2：能力协商、轮廓 payload、pending/current 原子
   提交；WM 的通用覆盖遮罩与缓存键。保留 v1，不以外包矩形降级非矩形效果。
2. Scene/DSL：通用路径声明与不可变几何准备，绘制、实时/快照命中、输入 region、
   effect 复用轮廓；结合 Popup 定位生成连接几何，主题只引用通用接口。
3. 参考图 02 的连接颈主题配方与完整交互验证；随后继续 Host 跨 surface 定位主线。

本步尚未接入上述生产路径，未提交、打包或替换 VNC。现有 WM 的背景采样仍只读取
所属 surface 下方内容；同 surface Popup 对正文的局部模糊需要独立客户端合成设计，
不能把通用轮廓协议误写成已经解决该问题。

## 22. 第四阶段 4f：Wayland v2 与 WM 通用轮廓遮罩（2026-10-06）

本轮将上一阶段的纯轮廓接入平台传输和 compositor，保持前端与 WM 的职责边界。
`SurfaceEffectRegion` 可附带已准备的 `Contour`；平台完整校验并编码事务后才发送。
协议保留 `*_v1` 名称和全部旧请求，将 interface version 升至 2，新增独立轮廓能力
事件及 `add_contour`。老客户端仍绑定 v1；新客户端连接 v1 或能力为零的服务端时
省略轮廓 backdrop、保留矩形和客户端着色，不以外包矩形扩大效果。

服务端严格解码有界 payload，复制点数据，矩形与轮廓合计最多八个；clear/add 只
更新 pending，surface commit 才原子发布 current。坏 payload、能力、范围与预算
请求明确拒绝，surface 销毁立即撤销其节点；effect 对象销毁/重建不提前替换已提交
效果。客户端发送缓存比较实际传输的固定点值和完整轮廓，非法事务不能先 clear
再失败。详细协议与责任见 [通用轮廓契约](SURFACE_CONTOUR_CONTRACT.md)。

WM 只消费纯几何，使用通用扫描线生成 alpha 遮罩，四个纵向子采样配合水平区间
面积覆盖；没有引入 Skia、DSL 或 Popup/Menu 分支。单遮罩最多 16 MiB、单边最多
8192，并检查设备纹理上限，超预算在大缓冲分配和背景捕获前拒绝。失败禁用当前
效果节点；随后合法提交可恢复。AA 为有界近似，不宣称与 Skia 边缘逐像素一致。

遮罩和材质分别缓存。背景变化只修复材质并复用遮罩，几何变化才重建并上传；两层
缓存均比较完整顶点，形状仅在相同 bounds 内改变也会失效。呈现变形映射所有点，
保留分数原点，不重新量化；奇数尺寸的一半分辨率采样按实际比例回写。局部轮廓
不会因 bounds 等于窗口而被误判为窗口装饰。新增 typed status 计数
`effects_work.mask_builds/mask_cache_hits/mask_failures`，不额外增加 timer、刷新或
生产像素读回。代码同时修正公开 IPC DTO 所需 JSON 依赖的 CMake 传递可见性。

WM 和 Host 构建通过。相关 CTest **15/15** 通过：surface_effect_contract、
surface_effects_state、surface_effects_protocol、surface_effects_contour_gpu、
surface_effects_coverage、wayland_effects_client、wayland_lifecycle、wayland_submit、
effect_dependencies、json_boundary、contour_contract、contour_render、rounded_scene、
scene_popup、scene_menu。实际客户端通过公开 `WaylandWindow` 与真实 Wayland/XDG/SHM
验证 v1、v2 无轮廓能力及 v2 有能力，覆盖完整发送前校验、清空、缓存和 commit 时机。

真实 GPU 测试验证凹形、非对称连接颈、分数边缘、非均匀缩放、同 bounds 换形状、
背景变化复用遮罩、正 shadow_y 向下、销毁重建和超 16 MiB 失败恢复。读回样点为：
上颈 RGBA `(255,0,0,255)`、凹口 `(0,0,0,0)`、下主体 `(0,0,255,255)`、分数左边缘
`(0,0,191,191)`。独立真实合成调度 probe 的 **54 个场景** 通过，覆盖静止、局部
损伤、90/270 度旋转、子 surface、焦点与鼠标唤醒；设备为 Broadcom V3D 4.2.14.0、
Mesa GLES 3.1。上述数字验证正确性与缓存行为，不作为 FPS 或 VNC 性能结论。

首次 GPU 门槛发现新增遮罩 Y 翻转与 wlroots 离屏纹理行序不符，已统一几何、遮罩
和背景方向。首次客户端测试 fixture 使用完整 renderer display 初始化，在 Pixman
无 DRM 的场景失败，改为 wlroots 的 SHM 初始化后通过；这是测试环境修正。
失败日志和修正后记录均保留，未以放宽门槛消除失败。

另发现系统 `/usr/include/nlohmann/json.hpp` 有 71 个损坏字节，之前的 CMake 模块
仍损坏。使用 apt 缓存中相同版本的 `nlohmann-json3-dev`、`cmake-data` 软件包，经
仓库 metadata SHA256 核对后解包到私有目录
`/home/ss/.cache/prism-deps/contour-system-headers/`，重新配置和构建成功。没有更改
系统文件，也未将隔离依赖路径写入源码；系统文件本身仍需要单独修复。

规范检查通过（378 个生产文件、142 个测试文件，生产最大 669 行），git diff --check
通过。证据位于 `dist/validation/interface-system-step4f-wayland/`，包括初次失败、
软件包及文件哈希、构建、15 项回归、GPU 像素与 54 场景报告。契约、材料文档和项目
SKILL 已同步；未提交、打包或替换 VNC，当前安装版本仍为 v22。

下一步接入 Scene/DSL 的通用路径声明和不可变几何准备，让填充、描边、阴影、裁剪、
实时/快照命中、输入 region 及 effect 共同引用同一轮廓，再按图 02 的 Popup 定位
生成连接颈主题配方。当前生产 Popup/Menu 仍使用矩形/圆角；同 surface 正文局部
模糊和 Host 跨 surface 定位仍是独立后续工作，不能由本轮协议验收代替。

## 23. 第四阶段 4g：Scene/DSL 通用轮廓与不可变输入（2026-10-06）

本轮将路径接入生产前端：`Contour(space: "local")` 是组件内的 typed 几何元声明，
首个 Move 后支持 Line/Cubic 具名数值字面量。准备阶段完成有界展平、量化和拓扑校验，
PreparedComponent、Blueprint、critical 合成和区域安装保留规范点；不增加普通字符串
属性或 layout 子节点，也不改变 Slot 的可视索引和 DslProperty 编号。

Scene 在 detached snapshot 中按整个布局原点量化、平移并验证；实时输入、提交快照、
RenderTree、DrawList 与 effect 使用同一份规范几何。颜色变化和相同几何的主题切换
保留共享存储，不因未使用的 cornerRadius 推进输入快照版本。填充、悬停/焦点、内侧
描边、内外阴影及现有 clip 政策都使用轮廓；旧快照不借用可变节点数组。

输入直接求规范多边形与解析圆角的像素中心交集，保留凹形的多个跨度；每次交集最多
八个去重形状，完整 Scene 的输入矩形最多 65,536 个。结果准备成功后才替换缓存。
背景效果只接受相同或可证明完整保留的已有形状，不能表达的部分交集拒绝候选，
不用 bounds 或重叠区域代替。协议仍沿用 4f 的 v2，WM 没有新增控件或 DSL 分支。

滚动使用只读前瞻放置上下文，先验证新轮廓、effect 交集和完整输入预算，再改变
offset、节点位置和输入状态；成功复用准备好的输入掩码。预检失败保留 Popup、捕获、
快照、版本与缓存；关闭锚点浮层的输入政策也纳入预检。零尺寸节点不在无 Layout
滚动中产生轮廓。该流程不重建 Scene、重新布局或重复展平曲线。

Preflight、主题和区域事务保留静态源轮廓及 provenance；候选不能伪造保留节点的
路径。Contour 覆盖该节点的统一圆角形状语义，局部点可超出 layout bounds；它不提供
固有尺寸或自动缩放。window 材质、生成式 Slider/ScrollView Visual 部件、叶控件、
normalized 空间、路径 binding/state/transition 均拒绝。详细语法和完整静态连接颈
配方见 [通用轮廓契约](SURFACE_CONTOUR_CONTRACT.md)。

WM/Host 构建通过，相关 CTest **16/16** 通过：dsl_contour、contour_mixed_raster、
render_tree、scene_contour、scene_contour_visual、rounded_scene、scene_popup、scene_menu、
scene_input_snapshot、scene_region_transaction、scroll_view、slider、load_plan、
master_load_session、visual_scene、skia_gles_parity。新门槛覆盖局部路径超出 bounds、
小数原点、精确混合裁剪、旧快照冻结、滚动 blur/坐标失败完整回滚与恢复、零尺寸、
区域 provenance、主题输入版本稳定以及 Popup 外点不穿透。

文档配方由独立 probe 直接读取，真实字体整形和 Skia raster 在四材质 × 明暗下
通过；八种均使用 56 个规范点和 24 个输入区域。填充、描边、内外阴影、clip、
InputSnapshot 及 contour effect 点数据一致，材质仍使用真实主题 token。现有 GLES
gate 报告 Broadcom V3D 4.2.14.0，轮廓局部修复与同后端完整回放最大通道差异 0，
改变 81 个像素、保留 432 个修复范围外像素；Gaussian 样点沿用至多 16/255 的
后端差异门槛。上述验证不作为 FPS、VNC、背景模糊或生产 Popup 外观认可。

首轮回归 15/16 通过，旧 visual_scene 的超范围 backdrop 门槛发现共享 validator
将 Scene 的原 runtime_error 诊断改为了 invalid_argument；已以具名适配恢复
`Unsupported backdrop bounds`，保留共享数值校验和范围拒绝，不改测试或放宽门槛。
修正后重新构建及完整 16 项回归通过，首轮日志保留。

代码规范通过（382 个生产文件、146 个测试文件，生产最大为 dsl_frontend.cpp，707 行），
git diff --check 通过。证据位于 `dist/validation/interface-system-step4g-scene/`，包含
`build-verified.log`、`ctest-main.log`、`ctest-final.log`、`style-final.log` 和
`diff-check.log`。继续沿用 4f 的私有依赖隔离，系统损坏文件未修复。文档与项目 SKILL
已同步；本轮未提交、打包或替换 VNC，安装版本仍为 v22。测试与配方 probe 留在
tests，不进入生产应用包。

后续顺序：

1. 按 PopupPlacement 的最终尺寸、方向和锚点偏移设计通用参数化路径准备接口；几何
   在前端所有者线程准备并冻结，渲染线程消费结果，WM 继续只处理规范点。
2. 主题定义连接颈与面板配方，复用材质、明暗和布局 token；按图 02 验证紧凑声音
   面板、上下回退、边缘避让、Square 与旧帧输入。静态路径示例不代替这些能力。
3. 继续 Host 可信跨 surface 定位。若需模糊同 surface 正文，独立设计客户端合成
   边界；本轮没有改变 WM 只采样所属 surface 下方场景的规则。

## 24. 第四阶段 4h：最终定位驱动的主题面板配方（2026-10-06）

本轮按图 02 接入 `attachedPanel`：DSL 保留 owning typed 数值/Number 引用，Scene
在安装/主题候选中解析三个参数；布局保存最终 PopupPlacement 后才生成连接颈。
正文尺寸和 padding 保持原有语义。纯 `panel_contour` 库不依赖 Popup、Theme、DSL
或渲染后端，输出仍为 4g/4f 已有规范点，不新增 WM 控件或 Wayland 字段。

配方明确 `fallback:"detached"`，正常水平 Center 对齐、下方优先、上方回退。
颈部全宽包含两肩，必须在正文圆角之间容纳且中心位于有效锚点水平投影；选定中心
还检查锚点自身/祖先实际 clip。复杂凹形中心被排除时保守回退，不搜索全部其他
可见跨度。EdgePanel、禁用颈部或无安全交集也回退；所有回退保留选定正文位置。
非法参数/量化退化不回退而明确拒绝。gap 增加有效 neckHeight，颈尖距锚点 8，
不把指向形状误写成锚点与面板已物理贴合。Square(radius0) 正文与颈部全直角。

局部准备缓存以尺寸、方向、中心和解析参数为键；颜色更新不重新展平。Above 在
控制点阶段镜像后统一准备，放置仍只量化整个原点。候选 snapshot 全部轮廓通过后
发布，旧输入快照保留旧点；主题/区域事务保护描述 provenance。关闭面板更新配方
参数不单独触发像素/布局，展开时使用最新参数。实时/快照输入、DrawList、clip、
内外阴影、InputRegion 与 effect 继续引用同一 canonical 几何。

测试与图 02 的紧凑声音面板 fixture/probe 保持在 tests，独立于生产应用和安装规则。
WM/Host 构建通过；相关 CTest **21/21** 通过，包括四项新增门槛 panel_contour、
dsl_contour_recipe、scene_attached_panel、attached_panel_visual，以及原有轮廓、
Popup/Menu、控件、加载、区域事务、输入快照和 GLES 回归。纯几何覆盖上下镜像、
非网格尺寸、肩部安全范围、Square、极限预算与量化坍缩拒绝；Scene 覆盖上下回退、
边缘避让、贴边/窄范围 detached、实际祖先 contour clip、旧快照、颜色共享、隐藏
参数/unused radius 不刷新、主题失败回滚与区域 provenance。

真实字体整形与 Skia raster 的 **32 个场景** 通过：四材质 × 明暗 × Below/Above/
right/EdgePanel。圆角附着轮廓 50 点、detached 36 点；Square 分别为 8/4 点。填充、
边框、内外阴影、clip、InputSnapshot 和 effect 比对同一规范几何；InputRegion 检查
覆盖而不要求集合分割相同，owner 的外点关闭屏障仍保留。实际预览另检查文字/读数、
细轨道、分段选择和方形复选标记，移除了与颈部重叠的 owner 说明文字。导出的 PPM/PNG
只呈现字体、着色与轮廓；不是同 surface 正文模糊或运行桌面的截图。

首轮回归 20/21：新 DSL 测试误用了 MenuItem 位置标签，现有契约要求具名 action 与
子 Text。已修正测试 fixture，再构建并完整复跑 21 项通过；未放宽解析或测试门槛。
初次测试调用另发现私有隔离目录没有 ctest 可执行文件，改用系统 ctest（无需损坏的
CMake 模块）；原始日志保留。生产构建继续使用已验证的私有 CMake/JSON 依赖隔离。

代码规范通过（387 个生产文件、150 个测试文件，最长 dsl_frontend.cpp 714 行），
git diff --check 通过。完整证据位于 dist/validation/interface-system-step4h-panels/，
包括 build-initial/build-verified/build-test-fix、ctest-main/ctest-final、style-final、
diff-check 与 visual-preview，以及 previews 下的独立输出。契约、SDK 和项目 SKILL
已同步；本轮未提交、打包或替换已安装的 v22/VNC。

后续顺序：

1. 接入 Host 可信跨 surface 定位，明确 owner 可用区域、逻辑坐标变换及关闭政策。
2. 若需要图 02 的同 surface 正文背景模糊，独立设计客户端合成边界和采样依赖，
   继续复用轮廓/损伤/不可变提交。当前 WM 仍只采样所属 surface 下方场景。
3. 继续文件任务层与轻量反馈的 Host/DSL 服务接口，保持图 03—05 的层次和紧凑尺度。

## 25. 第四阶段 4i：跨 surface 传输基础（2026-10-06）

先核对实际运行边界再接 Host 自动导出。当前 WaylandWindow 独占一个 connection，
ClientRenderOwner 的 WSI/资源/提交与输入快照均为单窗口；WM 只监听 new_toplevel，
不会自动创建 xdg_popup 的 scene tree。另开 ClientApplication 或独占式 EGL owner
不能作为临时浮层实现。

本步的完整检查点为：

1. 无后端依赖的 typed positioner 准备，明确 parent surface、parent window geometry
   和 WM output 三种空间；客户端偏好与最终 configure 分离。
2. 同连接 WaylandPopup 传输，parent 唯一 Pump、configure/ACK/commit 门控、父关闭
   先销毁子对象和 popup_done。保留 renderer 借用口，不引入生产 SHM 绘制。
3. WM 通用 new_popup，实际同 wl_client parent 验证、可用区域约束、独立 scene node
   与输入 owner 解析。Popup 不参与 BSP/launcher/装饰；布局或组几何变动前关闭。
4. 真实 Wayland/wlroots 测试覆盖坐标、配置顺序、父子关闭与被拒绝的输入路由。
   测试 buffer 和服务端 fixture 保持在 tests。

规范以 [Popup/Menu 的 4i](POPUP_MENU_CONTRACT.md#4i跨-surface-的定位与生命周期基础)
为准。本步不把现有 Scene Popup 切为跨 surface，不承诺 GPU child、背景效果、
控件 scope、grab serial 验证、级联菜单或 VNC 实机视觉验收。

本步实现与验证：

- `prism_popup_positioner_contract` 为独立纯契约库，完成锚点裁切、geometry 原点变换、
  整数准备与非法输入拒绝。`WaylandPopup` 借用真实父连接，只在最终 configure/ACK
  后允许提交；父退出、尺寸变化、compositor dismiss 与协议失败均撤销 child。
- WM 新增通用 popup scene 生命周期，正确处理嵌套与非零 window geometry 原点，
  约束实际启用输出。隐藏 owner 和无输出时拒绝新浮层；拒绝操作延后至通知栈返回。
  工作区、布局、fullscreen、组几何和输出变换变化前关闭旧浮层。
- WM 与 Host 生产目标构建通过。新增 `popup_positioner_test`、
  `wayland_popup_client_test`、`wayland_popup_wm_test` 均通过；相关回归最终 **36/36**。
  客户端协议 fixture 检查配置/提交顺序、同连接、输入隔离、重入关闭及迟到事件；
  WM 测试在 Pi 的 headless/Pixman 后端验证真实 Wayland/wlroots 路径。测试 SHM
  不进入生产源码或包，也不构成 GPU child 或 VNC 视觉验证。
- 回归发现两个旧测试的观察时机缺口：应用进程回收早于 WM 处理断开，以及客户端
  本地 pixel commit 早于 WM 实际 map。测试改为等待 owner 从窗口树移除、实际
  scene 命中和键盘焦点就绪；保留原控制柄清除与输入事件断言，未加固定等待。
- 代码规范通过：393 个生产文件、153 个测试文件；最大生产文件为
  dsl_frontend.cpp，714 行。`git diff --check` 通过。构建沿用已验证的私有
  CMake/JSON 依赖隔离，没有修改系统依赖或写入生产路径。

完整证据和初始失败日志在 `dist/validation/interface-system-step4i-transport/`，索引
为该目录的 README.md，最终记录为 build-verified、build-lifecycle-verified、
ctest-final、style-final 和 diff-check。本轮未提交、打包或替换安装中的 v22/VNC。

后续顺序调整为：

1. 拆 connection/context 与每 surface WSI/损伤/呈现，保持一个 render worker，修正
   多 target 下同尺寸 framebuffer 的 Skia 包装缓存和资源生命周期。
2. Host/Scene 将同一逻辑浮层导出为不可变 surface 计划；按 compositor 最终 configure
   重新布局/轮廓，接输入 token、scope、提交基线和安装/关闭屏障。默认能力回退明确。
3. 接 Popup 的通用 WM effect descriptor 与 client 采样边界，验证 parent 正文模糊，
   最后才切生产路径与实机部署；保留图 02 的紧凑风格。
4. 文件任务层与轻量反馈服务接口继续遵循图 03—05。

## 26. 第四阶段 4j：共享 GPU 上下文与独立 target（2026-10-06）

规范见 [多 surface GPU 契约](MULTI_SURFACE_GPU_CONTRACT.md)。执行顺序：

1. WaylandEglContext 与每 surface WSI 分开，同连接代理校验和关闭顺序明确。
2. 有类型的 context/surface lifetime 与 resize generation，修复同尺寸默认 FBO=0
   的 Skia wrapper 缓存，保留一个 Ganesh 和共用图片。
3. ClientRenderOwner 显式持有共享 Context，把 WSI、损伤、准备/提交及输入基线
   放入独立 target 状态；准备计划在 Swap 前校验目标身份。
4. 独立底层多 target GPU/WSI 测试与现有 SDK 回归，记录实际驱动和验证边界。

本步保持 Scene Popup/Menu 同 surface 呈现；下一步才连接 Host/Scene 不可变 surface
计划、最终 configure 驱动布局、子层输入 scope 与呈现反馈，不增加业务启动入口。

本步实现与验证：

- 生产根窗口已迁移到显式共享 Context 和独立 target 状态；同尺寸默认 FBO=0 的
  Skia wrapper 按完整 target 身份区分，图片与 Ganesh 保持共享。真实 resize 更换
  generation，旧 generation 拒绝；缓存限制为 64 个存活 target，可显式释放。
- 排查发现协议失败时旧 Window Pump 会先断开连接，再清理借用该连接的 EGL WSI。
  新增具名、单次、noexcept 后端关闭屏障，在 native proxy/connection 销毁前解除 GPU
  资源；业务 Closed 通知保留原时机。真实协议 fixture 验证正常关闭、dispatch 内关闭、
  server protocol error 与 popup_done，父子资源顺序及重复 Close 均通过。
- WM、Host 与相关生产/测试目标构建通过，相关 CTest **41/41**。新增多 target EGL
  用例验证同尺寸目标、48 次部分修复轮换、共用图片、实际 framebuffer 重绑、70 次
  storage generation 更替、64 target 上限与外来 context 资源隔离。WSI 生命周期
  fixture 单独验证连接校验、失败清理、current 恢复和每 surface age/损伤状态。
- Pi 隔离 Wayland/V3D 验证最终 **7/7**：六项既有 SDK 准备、提交、损伤、动画及
  输入快照检查通过；新增真实父子 WSI 检查报告 `V3D 4.2.14.0`，父 1280×720、
  子 160×112，17 次 pixel 提交、16 次 target 切换、4 次 wrapper 创建/释放、
  13 次缓存命中、共用图片仅上传 1 次。父关闭屏障 1 次、child 屏障 3 次；子关闭
  后父资源仍可用。该检查覆盖显式资源生命周期，不宣称 Host 自动浮层输入或视觉验收。
- 新增 native probe 初次失败是测试 DisplayList 漏填必需的非零 window 标识；补齐
  夹具后只重建该 probe、复跑失败 gate，未放松生产校验、像素或生命周期断言。
  六项既有 SDK 结果保留，最终汇总明确记录来源，不把初始失败报告覆盖成成功。
- 代码规范通过：397 个生产文件、156 个测试文件；最大生产文件仍为
  dsl_frontend.cpp，714 行。`git diff --check` 通过。测试/probe 不进入生产安装规则；
  沿用已有私有构建依赖隔离，本轮未提交、打包或替换运行中的 v22/VNC。

证据在 `dist/validation/interface-system-step4j-gpu/`，索引为 README.md；
最终记录包括 build-verified、build-probe-fix、ctest-main、style-final、diff-check、
native-final-summary.json 与 native-shared-final。初始 native 失败日志保留在 native/。
上述 GPU 功能验证不等价于帧率、零拷贝、功耗或 VNC 视觉测量。

下一步为 4k：在 Host/Scene 层建立不可变 surface 计划，将同一逻辑 Popup/Menu 的
内容和输入归属导出到 child target。先接 compositor 最终 configure 的布局/轮廓与
各 target 的提交基线、安装/关闭屏障，再接可信 input token/scope 和独立 frame
callback/呈现反馈；按能力选择同 surface 回退。跨 surface effect descriptor 和
parent 正文采样边界随后单独接入，保持图 02 的紧凑层次与应用业务 ABI 边界。

## 27. 第四阶段 4k1：不可变 Popup surface 计划（2026-10-06）

规范见 [Popup surface 计划](POPUP_SURFACE_PLAN_CONTRACT.md)。先完成可独立验证的
导出与最终配置准备，再进入生产生命周期/输入/调度；执行顺序为：

1. 从同一 Scene 捕获当前活动面板与可信锚点，Host FramePacket 携带不可变请求。
2. 最终 configure 反馈到 UI 所有者，在拥有的快照值中局部布局、生成轮廓/DrawList
   和子树输入描述；拒绝失效请求，保持已提交 root 几何和状态。
3. 原生 adapter 区分含正文/功能连接颈的 window geometry 与含阴影的 buffer，绑定
   configure 代次。
4. 独立 Scene/协议回归与真实 V3D probe 验证导出的计划，而非重新手写测试绘制列表。

Menu 子页继续替换同一个逻辑面板，原生 parent 为 root。4k1 不自动切换生产 Popup；
4k2 完成 target 输入 provenance、安装/关闭屏障、独立 callback/feedback 与提交基线
后，再连接实际生产生命周期。跨 surface 背景效果为 4k3，视觉仍遵循图 02。

本步实现与验证：

- Scene 捕获可信锚点、活动节点/token、父配置与 Scene/theme/pixels 代次，并共享当前
  已解析快照。Host 的 FramePacket 携带该请求，保持一个 Scene、一个 render worker
  和现有业务 ABI。仅主题代次变化的 metadata packet 可刷新解析值，不增加根布局或
  绘制次数，也不要求调用 Build。
- 最终 configure 驱动局部 Measure/Place、连接颈/Contour、文字及 Slider/Scroll
  Visual；不修改 root 几何、输入快照或滚动位置。区分整数功能 window geometry、
  分数正文与含阴影的 buffer；功能连接颈在 window geometry 内，阴影不扩大输入。
  编辑器、无法保持的祖先裁剪/变换明确诊断并保留原窗口呈现。局部 Input.scene=0，
  现有 root 输入入口拒绝它；独立 child adoption 与每帧几何复用仍为 4k2。
- WaylandPopup buffer layout 绑定当前已 ACK 的 configure 代次；非法布局不产生
  协议请求或覆盖当前布局。Set/Reset 不 commit，新 configure 撤销旧布局；原有
  AttachBuffer 路径与显式 EGL layout→WSI→Swap 顺序均有独立验证。
- WM、Host 与相关目标构建通过。相关 CTest 最终 **42/42**：初轮其余 41 项通过，
  新测试的 TextField fixture 缺少 action，在 Scene 创建阶段被拒绝。补齐合法夹具后
  仅重建/复跑该项，确认不支持编辑器在导出阶段正确诊断，未放松生产校验。
- Pi 隔离原生 GPU 检查 **8/8**，实际驱动 `V3D 4.2.14.0`。新增 Scene/DSL 导出
  probe 覆盖四种材质×明暗配色×下方/上方/边缘定位共 **24** 个组合；72 次 GPU
  提交、240 次 CPU/GPU 像素对照、25 次 wrapper 创建、24 次 child 释放/关闭屏障。
  绑定更新实际改变 Checkbox 的 GPU 像素；子关闭后 parent/Ganesh/Context 仍可用。
  隔离 WM 已回收。该检查不代表生产 child 输入、毛玻璃或帧率验收。
- 构建发现 Scene 调用 positioner 契约后漏写 CMake 依赖；补齐 PUBLIC 契约库依赖。
  初始链接失败、测试失败和构建检查中止日志均保留，最终通过记录另存。
- 代码规范通过：402 个生产文件、158 个测试文件；最大生产文件仍为
  dsl_frontend.cpp，714 行。测试/probe 保持在 tests，未进入生产安装规则；
  沿用私有构建依赖隔离，未修改系统依赖。`git diff --check` 通过。

证据在 `dist/validation/interface-system-step4k1-plans/`，索引为 README.md。
当前生产 Popup/Menu 仍由所属窗口呈现，本轮未提交、打包或替换安装中的 v22/VNC。
下一步 4k2 接生产 target 生命周期、可信鼠标/键盘输入归属、首次提交切换屏障、
独立资源/损伤/内容代次以及 callback/feedback；4k3 再接跨 surface 背景效果与验收。

## 28. 第四阶段 4k2：生产子层生命周期、输入与调度（2026-10-06）

规范见 [Popup 子层生命周期与已提交输入](POPUP_TARGET_LIFECYCLE.md)。本步将 4k1 的
请求与最终配置计划接到真实 Host/Scene/RenderOwner；执行顺序为：

1. 同连接 child 创建、最终 configure 回 UI、实际提交与关闭事件统一排队/ACK。
2. 首次成功 child Pixels 后验证 prepared provenance、采用局部输入并撤 root 面板；
   后续 State/checked None 更新已提交输入，pixel ID 与 adoption sequence 分开。
3. 实际 surface 路由鼠标/键盘、内部焦点转移、Slider/Scroll 与旧 scope 取消；
   菜单替换、UI 安装、父配置变化和资源撤销有明确屏障。
4. 每 target 保持独立 callback/feedback、损伤与内容基线；一个 Scene 的聚合动画
   许可分别确认 root/child 完成，最终局部布局按 configure/布局签名复用。
5. 独立协议/Scene 回归和真实 SDK/V3D 集成验证，更新契约与应用 SKILL。

当前自动 native 能力为 scale=1、无 backdrop 需求且支持局部导出的 Popup/Menu。
毛玻璃、编辑器与不支持的变换/裁剪保留 root 呈现；主题或定位/epoch 改变可重新尝试。
普通命令执行后关闭浮层，值操作保留浮层，业务 ABI/DSL 不新增原生窗口入口。
各 surface 独立提交，本步不提供跨 surface 原子呈现。

本步实现与验证：

- 生产 WM/Host 构建通过。相关 CTest 最终 **43/43**：初轮 41 项通过；新的能力
  测试错误要求 Composite-only 更新产生 DrawList，修正夹具后通过。另一个测试的
  可执行 ELF 在 main 前出现 35 个越界重定位偏移，保存失败文件/诊断、仅重编译链接
  该目标后通过；没有据此修改 Scene 逻辑，也未推断硬件根因。初轮日志保留。
- Pi 隔离 Wayland/GPU 检查 **9/9**，实际驱动 `V3D 4.2.14.0`。新增 SDK probe
  验证首次交接、metadata 禁用/恢复、原生局部 Checkbox/Slider、命令关闭与外点消费、
  Glass 回退/Square 恢复、UI 替换和重复 Close。child 的输入身份/lifetime 稳定，
  root UI 里程碑不被 child-only 更新推进。
- 最终 SDK 动画记录：child-only **16 child / 0 root** 次像素提交；root-only
  **17 root / 0 child**；mixed **14 root / 15 child**。这些是功能隔离与调度证据，
  不是 FPS 或实际呈现时间测量。既有动画、输入快照、部分损伤、准备/资源、多 target
  和 24 组合导出对照均通过，隔离 WM 已回收。
- 审查修复了 Root metadata 提前消费整帧许可、无候选时误关 child、UI 未比较
  window geometry、同尺寸父 configure 的像素门槛过严，以及较新图片先到造成旧
  候选永久降级的边界。资源释放同时检查候选与成功基线；旧 root 描述不能激活 child。
- 原生 probe 初轮包含非法 Card/Button background transition 和会关闭浮层的普通
  action；按现有 Visual/值控件契约修正，并补齐根输入覆盖范围。未扩展生产 DSL
  白名单或改变菜单命令语义来适配测试；原始失败记录保留。
- clang-format 19、行数/goto 门槛、SKILL 校验和 `git diff --check` 通过：
  **412** 个生产文件、**160** 个测试文件；最大生产文件 dsl_frontend.cpp 为 **714** 行。
  测试/probe 不进入安装规则，私有构建依赖隔离保持。

证据在 `dist/validation/interface-system-step4k2-owner/`，索引 README.md 与
summary.json 明确引用初轮 41 项、修复复测 2 项及最终完整 9 项 native 报告。
本轮为源码与隔离 GPU 验证，未提交、打包或替换运行中的 v22/VNC。

下一步 **4k3**：为 Popup 接通用 surface effect descriptor，明确 parent 正文与
下层内容的采样边界、排除自身/阴影递归采样；统一坐标、轮廓/裁剪和 damage 依赖，
完成四种材质/明暗切换、主题回退与真实 GPU 对照后再进入生产视觉部署。继续遵循
图 02 的紧凑层次，不增加应用特判或第二套 UI。随后推进图 03—05 的文件任务与反馈。

## 29. 第四阶段 4k3：原生浮层背景效果与父正文采样（2026-10-06）

规范见 [POPUP_BACKDROP_CONTRACT](POPUP_BACKDROP_CONTRACT.md)。沿用图 02 的
Popup/Menu 紧凑层次；本步增加通用采样与提交能力，不改变应用布局或增加应用特判。

- `PopupSurfacePlan.effect_regions` 在最终 configure 的同一次布局、轮廓准备与局部
  平移后导出，和 DisplayList/input 共用几何。root/native 共用严格交集算法，
  不可表示的裁剪或最终超过 8 regions 拒绝整份 native plan；shadow 不扩大 mask。
  blur-only 变化不进入布局签名，元数据纳入 prepared provenance。
- Wayland effect manager/object v3 独立声明 popup backdrop；保留 v1/v2 兼容。
  Window/Popup 提供 typed capability 查询；子层 descriptor 在全量校验、定点转换后
  stage，下一次 Pixels/State 采用。旧 target / configure 拒绝，不静默丢 Contour。
- WM 用真实 surface leaf 找 global 原点，paint 作为 popup tree 的同级前驱，位置
  转为 parent tree-local。按真实 scene 顺序采父正文和下层，排除当前子树、自己的
  effect paints 与上层；不重复 window geometry 或 output transform。
  复用现有 damage/mapping epoch/cache，父正文内外 damage 分别重算或复用。
  paint tree destroy listener 撤销指针，覆盖父树先于 wl_surface 销毁的寿命差。
- Host 首 child Pixels 暂无 effects，采用后生成携 excluded identity 的干净 root
  packet。真正 root pixels 或与实际像素匹配的成功 metadata 建立采样屏障，之后
  child State 开启 blur。旧身份/UI/resource 不得建立屏障；首次 ready 发一次 MPSC
  wake，避免 checked None 后阻塞到下一次输入。adoption sequence 允许合法前进。
- SDK 自动 native 根据独立能力判断；无需应用自行创建 surface。材质、明暗与
  blur-only 更新保留 native lifetime，像素、input、callback/feedback 及动画许可仍
  分目标记账。编辑器、非 scale=1 与不可导出的裁剪/变换继续 root fallback。

本步验证：

- WM/Host 与相关 57 个目标构建通过；最终相关 CTest **49/49**。
  初轮发现 native paint 原点误用 buffer iterator 的相对坐标，改为查询真实 leaf 的
  global coords，真实 GLES readback 通过。两处平台夹具分别混入尚未 flush 的合法
  configure ACK，以及父连接直接断开时的资源清理；修正同步边界后保留严格状态/
  销毁顺序断言，不改变生产 Close 行为。
- Pi 隔离 GPU 集成最终 **9/9**，驱动 `V3D 4.2.14.0`。四种材质 × 两种明暗组合
  保持同一 native lifetime；blur 12→24→0→12 不增加 root/child 像素提交。
  最终动画记录为 child-only **16 child / 0 root**，root-only **16 root / 0 child**，
  mixed **15 root / 16 child**。这些是提交隔离证据，不代表 FPS 或跨 surface 原子呈现。
  既有 24 组合、72 次子提交与 240 次像素采样对照通过，隔离 WM 均已回收。
- 初轮构建有旧 `tree_snapshot.cpp.o` 的非法 debug relocation；保存对象、哈希及
  readelf 诊断，仅重新生成该构建产物后链接通过，没有据此推断硬件根因。
  SDK 冷启动夹具增加真正 root Pixels 门槛，避免把零像素的安静状态误判成 ready。
- 初轮另有一次八主题后的快速 Command 未关闭，以及旧手势探针在断言退出附近
  abort。修复测试侧局部 callback 生命周期，避免退出时访问已释放 observer；
  单项、最终九项集成与独立 GDB 复测通过，但没有复现或证明两次时序失败的根因。
  失败日志与诊断保留，部署前仍需关注快速输入链，不能把复测通过描述成已消除偶发问题。
- clang-format 19、行数/goto、SKILL 校验和 `git diff --check` 通过：**419** 个生产
  文件、**162** 个测试文件，最大生产文件 dsl_frontend.cpp 为 **714** 行。
  测试/probe 没有进入安装规则，原有私有构建依赖隔离保持。

证据在 `dist/validation/interface-system-step4k3-backdrop/`，README.md 和 summary.json
分别列出最终门槛与初始失败记录。源码实现和自动 GPU 验证不等于生产视觉验收；
本轮未提交、打包或替换运行中的 v22/VNC。

下一步先补快速输入失败的轻量诊断，再部署本轮原生浮层，按图 02 检查透明、连接颈、
阴影与窗口边缘的实际呈现；随后推进图 03—05 的文件任务服务、owner 下缘任务面板与轻量反馈。

## 30. 4k3 部署前：续帧与输入修复、真实菜单入口（2026-10-06）

按用户要求先处理失败，再替换生产 VNC。以下修复保持单一 Scene、render owner、
EGL Context 和 Ganesh，不增加应用绘制分支。

- 低干扰协议环形记录在成功路径不写日志，失败抛出前停止进程再取状态。原生失败
  现场显示 worker 在无限 poll：sampling=true，approved/opportunity 为空，两个
  callback 均空，root update=false，输入 issued=processed=14。root 候选与成功
  基线 list 相同，child 候选也已消费，证实聚合消费后缺少下一次采样许可。
- 续帧依据两个 target 的真实 callback，取消用 root list 相等来选择时钟的规则。
  Root prepare 仍要求像素许可/反馈容量；child ready 与 advance 同时检查 root
  callback。State/checked None 可以完成自身消费；两个目标消费后只发一个新机会，
  不重绘静态 target，不增加 Timer、空提交或固定忙循环。
- 同 Scene 的 root↔popup 内部键盘焦点转移不取消鼠标捕获；真正失焦/关闭保持取消。
  原回归只覆盖 root 失焦，现在增加 child 方向。旧实现明确失败，修复后通过。
- 输入 probe 先等实际 action 或 Begin/Update/End/Cancel，再检查提交计数；严格
  phase、数量和身份校验不放宽。相同 650ms 传输延迟下，旧安静计数提前结束并失败，
  新 receipt fence 等输入到达后通过。异常退出 RAII 撤销局部 gesture handler。
- Preferences 头部新增 View 入口，wide/compact 各自唯一锚点；主题 panel、连接颈、
  字号14/11、16留白、36行高、8间距、分隔线，短窗口使用 ScrollView。菜单复用
  Overview/Appearance/Monitoring/Refresh 业务动作。普通命令执行关闭，外点消费，
  Host 自动选择 native/root；测试/probe 不作为生产视觉入口。

新增真实 owner 回归在旧实现失败、修复后通过。12 次隔离原生测试保持快速 Command、
八种材质/配色、root/child/mixed 动画及关闭重开门槛，全部通过。最初八主题后 Command
偶发失败没有单独再次捕获，不把它自动归因于续帧问题；保留原失败与诊断记录。
证据：`dist/validation/interface-system-step4k3-input-fix/`。

相关 CTest 最终 **52/52**、当前代码的隔离 GPU 门槛 **9/9** 通过；上述12次完整
原生复测在续帧修复后、内部焦点修复前完成，九项最终门槛包含焦点修复。代码规范
通过：419 个生产文件、163 个测试文件，最大714行。测试/probe 不进入生产安装。

v23 实际部署检查发现 Preferences 根 clip 包含 root-last Menu，原生导出因不能
保持祖先裁剪而回退。保留根 window 材质，将 clip 移到普通内容容器，菜单作为其
兄弟，SDK 安全规则保持。v24 的真实菜单 native plan 回归覆盖八主题×四视口，
相关 **4/4** 检查通过；实际 VNC 检查了连接颈、命令关闭、外点、Esc、Glass 明暗
与 Square。用户指出颈部顶部过扁平，未认可该视觉，按下一节继续修订。
图 03—05 的文件服务、任务面板和反馈仍为后续主线。

## 31. 图 02 实机修订：圆润三角连接颈（2026-10-06）

实机像素及绘制路径确认：v24 颈部未被裁掉，9.33px 平顶和8px高度来自旧配方。
用户明确修改参考设计，不保留平顶，改为圆润三角。此处通过通用配方修订，不增加
Preferences/WM 绘制分支，也不把右侧输出边缘的阴影裁切误作顶部截断。

- `Contour(recipe: "attachedPanel", ..., neckShape: "roundedTriangle")` 选择圆润
  三角；省略保持旧 softTab，显式 softTab 也兼容。形状仅接受具名字符串字面量。
- 主题 `panel_neck_width/height` 控制尺寸；v25 使用40×14，C方案调整为64×20。
  radius 控制柔度。连续肩部、收尖曲线和圆润顶点生成一个闭合轮廓，顶端无水平
  Line。Square 是直线三角。
- 上下方向在控制点阶段镜像，随后统一量化；退化、未知值和预算越界拒绝。
  形状参与 typed 配方/参数、缓存与 provenance。绘制、命中、内外阴影和背景遮罩
  继续共用最终 Contour，协议只传规范顶点。普通内容与浮层的裁剪分离保持。

详细字段与示例见 [轮廓契约](SURFACE_CONTOUR_CONTRACT.md#圆润三角颈v25)。
相关11项检查通过：初轮10项通过，一处几何测试把规范展平多边形外的点误作内部，
保留原点为 outside 并增加真实 inside 点后，该项通过；生产曲线和单顶点规则未放宽。
v25已正式安装，包和SDK/四主题匹配，实际V3D首帧、Menu命令/外点/Esc与主题检查
通过；WM commit/effect/mask failures为0。证据：`dist/validation/prism-v25-deploy/`。

**用户仍未认可v25颈部**：当前斜边与小圆角过于尖锐。最新意图为保留图02的宽肩
和柔和衔接，将平顶换成圆形弧度，整段圆润；方正主题才使用直线三角。
用户已在三种提案中选定[对照稿 C：圆润收尖](design/interface-system/neck-study-v1.png)。
按此修订通用配方：每侧两段 cubic 直接相接，宽肩到顶部全程连续曲线，没有长直
斜边；顶部仍有饱满圆弧和明确指向。64×20 是四主题默认尺寸，Square 继续使用
直线三角。形状接口、旧 softTab 默认与全部消费边界保持。
v25提供的接口与自动检查不构成默认视觉基线已验收；C提案选定与v26实机验收分别记录。

v26部署（2026-10-07）：上述C曲线与四主题64×20参数已安装到原远程会话。相关
CTest **11/11**、代码规范、Skill校验和生产包审计通过；测试/probe不进入安装包。
旧进程与socket已回收，两个remote服务active，Shell、Music和Preferences的V3D
首帧已确认。实际VNC检查了Glass明暗、Square直线三角、Appearance命令关闭、
外点及Esc，WM commit/effect/mask失败计数均为0。最终留Glass Dark和展开菜单。
记录位于 `dist/validation/prism-v26-deploy/`；实际颈部视觉等待用户确认。

## 32. 第五阶段5a：Owner任务核心与局部模态（2026-10-07）

第四阶段源码、文档、主题和应用改动已提交为`0418359`。继续图03—05前，先落实
[Owner任务契约](OWNER_TASK_AND_MODAL_SCOPE.md)，把业务会话、输入门禁和面板运动
分开。5a提供运行时基础，不声明系统文件选择器或图05透视运动已经完成。

- 纯`TaskSession`使用不同的owner/request strong type；owner只在最终已打开的
  前端首次请求时分配。每个owner一个活动任务或待取终态，不同owner互不阻塞。
  Preparing/Ready/Working和Success/Cancelled/Failed有明确合法转换，首个终态
  接受后拒绝重复及迟到结果；终态先取走再调用业务，支持同步发起下一请求。
- Scene普通子树提供局部模态作用域，所有seat的命中、滚轮、文本、值控件和焦点
  都受门禁。打开取消正文捕获、撤销旧Popup采用；外点保留，Esc一次终结，失焦
  取消捕获而保留任务。隐藏/禁用/成功区域替换使作用域失效，失败候选保持旧状态。
- 不可变输入快照新增独立单调`owner_modal_epoch`，打开、关闭及失效均推进。
  root/child命中与采用检查当前epoch，旧按下的释放不能激活新任务或结束后正文。
  纯取消与生命周期清理继续执行，不通过旧或外来快照恢复坐标命中。
- SDK以已挂载region接入TaskSession和Scene。Preparing只在worker真实采用匹配
  当前UI/Scene/epoch的输入后进入Ready；像素是否变化与输入采用分别判断。
  候选UI准备/取消/失败保留任务，成功UI替换取消request；Host退出先永久退休
  owner，再停止work并销毁业务。输入作用域不修改DSL的visible绑定。
- 第一版拒绝模态内所有Popup及嵌套作用域；没有任务C ABI、跨进程授权、文件结果
  payload或新DSL任务节点。owner数字只作本进程关联，不能作为系统服务权限。
  既有同步bool关闭接口保留，异步关闭续接随后设计。

后续顺序为5b业务任务ABI/Host-provider/共享DSL面板，5c异步目录与文件任务，5d
Notepad打开/另存为/未保存确认及关闭续接，然后推进图04反馈与图05主题运动。
面板布局继续参考图03：owner下缘展开、保留工作上下文、紧凑自然密度和清晰文字
层次；运动引用独立主题接口，不以动画完成代替业务Ready或终态。

本步相关CTest累计 **15/15** 通过：最终生产代码的整组复测14项通过，唯一剩余
Scene用例修正焦点前置状态后单项复跑通过。首轮12/15保留在日志中：未采用的
`scene == 0`原生描述符仍应完全拒绝根输入，已恢复该屏障；两个新夹具分别使用了
非法Column region及误把默认true空操作当作SetBinding失败，按已有契约修正。
随后一项夹具误以为PointerCancel重置键盘焦点，改为验证焦点保留，再真实点击
指定按钮继续验证Close/释放；没有改变生产焦点规则或放松有效断言。

三个新增测试覆盖精确身份、一次终态、退休/重入、Scene多seat与旧/null/foreign
快照、真实gesture/slider取消、原生Popup撤销与重开，以及SDK候选UI失败/成功
替换、无像素变化的输入采用、命令队列满回滚和Cancel回调同步发起下一请求。
SDK测试不开原生连接、worker或GPU，成功metadata为隔离夹具模拟，不作为实际
compositor采用/呈现或帧率证据。

正式Host/WM及相关目标构建通过。代码规范通过：**425**个生产文件、**166**个
测试文件，最大生产文件仍为714行；Skill校验和`git diff --check`通过。WM符号与
动态依赖审计未发现Scene、ClientApplication、TaskSession、Skia或Qt依赖。
最终增量曾因生成的Ninja规则出现异常字节而失败，重新生成后构建恢复；失败日志
独立保留，不将其当作功能通过。本步未打包部署，正式VNC仍为v26。
证据在`dist/validation/interface-system-step5a/`，README与summary区分整组复测、
单项修复复跑和自动检查范围；下一步按5b推进任务ABI与共享面板。

## 33. 第五阶段5b：Confirmation Provider与共享任务面板（2026-10-07）

继续[Provider与业务ABI契约](OWNER_TASK_PROVIDER_CONTRACT.md)，先建立真实
Confirmation，不提前实现文件列表、演示路径或Notepad异步关闭续接。任务含义由
业务决定，Host掌握实际owner归属，UI由受信共享DSL呈现；WM保持通用窗口职责。

- C ABI v1只尾追加能力查询、typed request/cancel及完成回调。业务不指定owner或
  Scene，ModuleSession复制UTF-8和choice数组、发放独立单调关联ID；一个pending，
  终态先退休再callback。Host sink只stage，完成回调不得发生在提交返回之前。
- Confirmation含1—2个业务选项，Primary/Destructive有明确角色；系统始终追加
  Cancel。Success只表示已选择已知choice，取消和失败有独立typed payload，不
  代表文件保存或业务后台操作已经成功。非法结构、UTF-8、角色及结果原子拒绝。
- Host在Preview之后、Master安装之前准备共享panel；普通非region Box/Card根
  才进行composition。Preview无panel，不能保留composition的根按原树运行并
  拒绝能力。`__prism_task_`是框架保留空间，业务binding/action不能修改或伪造它。
- owner Pump推进staged请求，准备当前Master的可见投影与局部scope。只有真实
  输入快照采用身份才能使Preparing进入Ready，provider输入只在精确任务/epoch
  上完成。结果取走、scope撤销与面板收回后交付；退出先退休owner且不再回调。
- 下缘面板沿图03/04保留工作上下文，文字说明清楚、动作位置稳定、边距紧凑。
  复用当前主题材质、状态、字号/行框及control.feedback；宽窄短视口分别布局，
  正文按真实宽度折行，溢出高度滚动。图05翻起/收回及owner退后运动随后接入。
- 标题/正文使用真实内容宽度折行和独立ScrollView；choice按实际字体与标签盒
  校验，不靠clip或缩字号掩盖不可读布局。主题、resize和后续Layout变化重新准备，
  Paint动画不重复折行。私有任务投影不进入业务binding表，deferred区域安装保持
  当前投影；包括未使用Interface声明在内的应用源始终检查保留空间。
- Preferences的View菜单接入真实Reset monitoring确认：一个Primary选择，系统
  提供Cancel，确认才恢复监控和1s间隔，取消保留业务值。完成结果一次交付，回调
  可同步发起下一请求；已接受的业务取消优先于随后失败，owner退出不再回调。

正式Host/WM、Preferences模块与相关目标构建通过。最终同一轮CTest **17/17**
通过，包含ABI兼容、任务策略、Scene/SDK、共享面板及已有DSL组合/菜单/值控件。
纯布局检查覆盖八种材质/配色×四视口×一/二选择，共64组合；SDK夹具使用真实
FreeType/HarfBuzz、布局与帧包，但其提交metadata为模拟，不是GPU呈现证明。

独立临时Wayland环境中的原生V3D门槛 **7/7** 通过：真实确认、系统Cancel、Esc、
业务取消与失败先后顺序、callback同步下一请求、owner关闭及两个owner同时持有
任务。Ready记录实际worker采用的owner/request/epoch、帧与输入版本；原生鼠标/
键盘验证正文屏障、任务结束后恢复及双窗隔离。该探针使用Square Light，其他主题
的64组合是纯布局检查；不将它们称为全主题GPU视觉验收或帧率测试。

验证中修正共享composer在Visual下放入测量region的问题：标签改用普通内容子树，
Visual只负责装饰，Scene限制保持。窄窗放大字体导致真实标签宽度不足时应返回
PreparationFailed，正向主题夹具改用可读宽度。另修复CriticalComposer漏拷贝
gesture的实际问题，保留真实拖动、捕获取消及异常回调中新请求的断言。
原生夹具补齐真实backend_ready声明，没有放松实际呈现/采用/V3D门槛。
构建探针显式链接已有nlohmann_json目标，避开系统异常头文件，系统文件未修改。

代码规范通过：**436**个生产文件、**174**个测试文件，最大生产文件仍为714行；
Skill、diff、WM符号/Qt依赖及生成安装规则检查通过。共享DSL资源进入安装规则，
测试/probe不进入生产安装。首轮测试、窄窗/手势失败及原生失败日志独立保留。
证据与各检查范围见`dist/validation/interface-system-step5b/README.md`和summary。
本阶段未提交新commit、未打包部署，正式VNC仍保留v26，面板实机视觉待后续部署确认。

后续为5c文件请求/异步目录模型、5d Notepad真实打开/另存为/未保存确认与关闭
续接，再推进图04反馈及图05主题运动。5a基础与本协议保持同一任务身份和输入门禁。

## 34. 第五阶段5c：异步文件Provider与共享文件面板（2026-10-07）

规范先落在[文件任务契约](FILE_TASK_PROVIDER_CONTRACT.md)，沿图03的下缘任务面板
继续同一个owner生命周期。三种文件请求通过typed ABI进入Host，实际读写留给业务。
本节源码已接入；正式远程会话仍为v26，自动验证与部署分别记录。

- Request/Result只尾追加文件options、路径及覆盖标志；旧Confirmation前缀、旧完成
  回调保持。业务借用内容在边界复制，文件Success校验类型、路径、后缀及独立payload。
- `FileTaskModel`使用共享scheduler的独立channel；规范路径、目录枚举、候选验证
  和stamp均在worker，主线程只处理拥有的结果。代次过滤、取消、容量FD重试保持。
  输出4096条/4MiB，扫描16384项；超限明确失败，特殊/无法表达的名称排除并计数。
- 共享`owner-file-panel.prism`与Confirmation共同组成一个普通作用域。保持原Master
  正文几何和根末尾Menu，引用系统材质、颜色及字号；八个投影槽位配合分页，不把
  数千个文件变成Scene节点。完整选中名可滚动阅读，主操作保留明确文字。
- Host路由Up/Home、分页、目录进入、文件选择、Save名称和提交；空目录、读取失败、
  非法名称与类型不匹配有恢复状态。文件名回送保留焦点与Ready，语义变化推进epoch、
  撤销旧流并等待新快照实际采用，不在每次按键中等待渲染。
- Save存在目标时同面板显示Replace/Back，保持一个TaskIdentity。再次worker验证
  canonical路径、parent identity及完整target stamp，对象改变要求重新审阅；不创建
  文件、不截断、不执行实际保存。选择结果不代替业务读写时的对象与错误检查。
- scope取消/替换/关闭撤销工作与投影，迟到结果不能写入下一请求。Host在没有活动
  文件任务时仍排空该channel通知，避免取消后空闲反复唤醒。测试/probe独立于安装。

实施顺序为ABI→工作模型→输入刷新→共享面板→Host路由→相关测试与隔离V3D。
下一步5d接Notepad的Open/Save As、实际文件工作、未保存确认和异步关闭续接；图04
反馈与图05独立主题运动随后按原顺序实现，不以面板动画完成替代任务Ready或终态。

验证期间修复刷新后的文本焦点跟踪：取消输入时临时移出focus记录会删除active项，
恢复focus记录时同步恢复TrackInputTarget。连续刷新保留面板焦点，结束后仍恢复最初
owner焦点。还修复空目录/Loading的状态文字被自然布局压到11像素的问题，共享DSL
给状态和紧凑路径详情明确的滚动视口；真实文字宽高校验保持，不以裁切冒充可读。

测试夹具使用实际slider中心、真实字体，以及原生输入源对应seat的焦点回执；原断言
保留，并补充命中、全选及逐字输入检查。初轮模板启动失败原因没有被记录，补充
StartDiagnostic后复测通过；不把未捕获原因自动归为I/O或预算问题。失败日志保留。

后轮模板回归明确捕获冷模块加载超过默认20ms入口预算。该结构/schema测试使用
独立5s夹具入口预算，保留Start、实际binding和全部布局断言；生产20ms及专用
module_work_test的严格预算/超时检查保持，不将模板测试当作启动性能证明。

原生逐字输入还揭示TextInputEvent缺少来源，文字错误进入最先创建的seat0焦点。
补充尾source并由Wayland携带KeyboardSource，Scene将按键和文字分别路由到各自
seat；新增双seat与旧epoch回归。C业务文本回调保持，旧合成事件默认seat0。

固定主操作按实际viewport、直属容器及祖先rect/rounded/contour clip校验。可读resize
保留目录、名称、身份和scope；空间不足明确PreparationFailed并收回。补正常resize、
越界一次终态、过大padding、圆角clip和凹Contour穿过按钮中段的回归。

最终同一轮CTest **24/24**通过；在原21项之外增加现有Notepad、文字行高和原生虚拟
输入回归，UTF-8来源须非零且与同次按键一致。纯面板为八种材质/配色×四视口×三种
文件请求，共96组合；SDK使用真实字体及模拟metadata，不连接worker/GPU。

隔离Wayland/V3D原生门槛 **14/14**通过：7个确认场景和7个文件场景，包含真实键盘
改名、导航选择、同任务覆盖及对象变动后的重审、取消/旧快照、owner关闭；核对规范
路径、覆盖意图、一次完成及provider没有写文件。实际采用记录owner/request/epoch、
帧与input_version/pixel commits。Square Light用于原生流程，不称全主题视觉或FPS验收。

统一构建的链接阶段发现SDK静态库一个成员与有效独立对象有65字节差异，readelf确认
异常重定位；重建该派生产物后恢复，25个成员逐一与独立对象一致。损坏原因未确定，
原日志与成员保留，不改源代码或把失败写成通过。最后统一构建包含全部24个测试目标。

规范门槛通过：**448**个生产文件、**178**个测试文件，最大生产文件714行；Skill、
diff、WM客户端符号/Qt依赖及52条生成安装规则审计通过。测试/probe无生产安装规则。
证据与范围见`dist/validation/interface-system-step5c/README.md`及summary；本阶段
未创建commit、deb或替换正式v26会话，用户视觉验收仍留后续部署。

## 第五阶段5d：Notepad业务与异步关闭（2026-10-07）

本轮将5c的选择结果接入真实Notepad业务，契约见
[文件业务与Close](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)。图03/04工作上下文和共享
任务面板保持；任务运动不在本轮伪造，下一步接图04轻量反馈及图05命名主题运动。

1. Open/Save As与未保存确认复用Host typed文件/Confirmation请求；移除Notepad
   自带路径页、确认页及相关binding，不保留两条选择器路径。
2. 业务复制结果后提交异步文件work；实际读写完成才更新文档。固定父目录FD，
   完整mtime/ctime stamp核对，批准覆盖仍需业务检查，新目标不意外覆盖。
3. Close ABI以尾字段追加request ID、REJECT/ACCEPT/DEFER与complete_close，
   保持旧bool回调兼容；owner Pump在回调结束后消费决策，SDK单次有序接受平台关闭。
4. 多文档逐个询问，全部完成自动关窗；中途取消/失败保留全部草稿。保存前准备
   快照，保存期间的新编辑仍未保存并再次确认；已接受task/work的异常恢复保留关联。
5. 业务、真实文件竞争、ABI/SDK、主题布局及隔离原生Wayland/V3D分别验证。
   测试/probe不安装，正式VNC与自动验证分别记录。

源码、验证与部署的最终状态在本节后续结果和
`dist/validation/interface-system-step5d/`记录。正式v26不包含5a—5d，
本轮不自动替换用户VNC会话。

### 5d验证结果

最终同一轮相关CTest **28/28**通过：原24项任务/输入/布局回归，加Notepad真实文件
竞争、主题布局及ModuleSession/SDK异步关闭门槛。Notepad布局覆盖四材质×明暗×
244×420、320×480、482×204、900×640视口，共32组合；光栅和纯布局不代替壁纸
合成的实机视觉认可。测试assert在Release下显式启用。

隔离真实Wayland/V3D **18/18**通过：原provider14场景，加真实Notepad模块的4流程。
原生键鼠完成打开、重复激活、另存、批准覆盖和外部修改后拒绝保存；真实WM关闭
事件验证重复请求合并、多草稿中途取消完整保留、全部Discard自动退出、Save后
实际写入才自动退出。Close成功同时核对渲染terminal无故障，临时WM与文件均已回收。
原生使用Square Light，不是FPS、全主题视觉或触控屏验收。

代码规范检查通过：450个生产文件、185个测试文件，最大生产文件714行；Skill、
diff、WM客户端边界、模块纯依赖和安装规则检查通过。已移除两套自定义内联面板。
首次测试link旧规则和新增Save as焦点过渡缺失已修复，失败与最终日志保留在
`dist/validation/interface-system-step5d/README.md`。

本轮源码完成，正式VNC仍为v26，未生成新deb或提交commit。下一步接图04轻量
业务反馈，再接图05主题可定义的任务开合、owner退后/恢复与中断/reduced motion；
继续复用现有任务身份、输入epoch及时间驱动动画，不新增Timer或WM应用分支。


## 6a：Owner轻量业务反馈

5d文件业务及异步Close之后，按图04先接Info/Success/Error与恢复操作。
详细规范见[反馈契约](OWNER_FEEDBACK_CONTRACT.md)。实现顺序：

1. 纯反馈值与C ABI完整尾字段、ModuleSession复制/身份边界。
2. SDK区域原子替换、共享DSL与主题语义颜色；不抢焦点的输入身份。
3. 实际采用后开始的单调可见时间期限、悬停/聚焦/模态暂停，无固定tick。
4. Host统一接入，Notepad真实成功/失败/取消及文件安全恢复。
5. ABI、业务、布局、native输入与打包边界验证。

本步后继续图04的说明/会话确认接口，再进入图05由主题定义的任务呈现运动；
系统会话确认须有受信入口，不以普通反馈API代理。源码完成不代表正式v26已部署，
不把现有Motion声明或功能检查写成透视翻转/用户视觉验收已完成。


### 6a验证结果（2026-10-07）

本轮Info/Success/Error已沿typed ABI、ModuleSession、Host与SDK接入共享
`owner-feedback-panel.prism`。只有业务恢复操作产生一次动作事件；到期、关闭、替换、
UI更换及owner退出静默撤销。共享反馈位于正文之后、局部任务之前，保留根末尾
Popup/Menu；显示不抢编辑焦点，不改变owner modal epoch。四主题的明暗配色新增
成功/错误语义色，实际129个Number/Color token需要匹配本轮扩展到256项的新协议
预算，64 KiB payload约束保持，不能只更新正式v26的主题资源。

最终选定的相关CTest **48/48**通过，覆盖反馈值/ABI短尾与复制、回调重入及退出，
SDK真实DSL/字体/布局与模拟metadata采用，非模态焦点、悬停及模态共存，期限暂停/
恢复/到期收回、旧输入与动态action伪造隔离。1000次区域替换检查节点槽位有界，
复用递增generation，UINT32_MAX槽位永久退休；拒绝请求保留旧投影，提交后发布失败
清退前端。通用区域安装、输入、动画、控件、Popup/Menu、主题协议、Confirmation、
文件Provider、Notepad业务与Close回归一并通过。这是48项相关选择，不宣称全仓库
CTest已全部运行；SDK夹具的模拟采用不代替原生呈现。

反馈纯布局覆盖四材质×明暗×240×180/640×420×零/一/二业务动作，共**48组合**。
独立CPU探针使用真实DejaVu Sans、共享DSL和Skia raster，生成**4张**PNG：640×420
明暗、240×180窄窗和640×204短窗。实际字形、标签宽高、32像素操作与DrawGlyphRun
均有断言；窄窗收起状态图标，将宽度留给标题和关闭操作，长内容通过独立ScrollView
阅读。这些图片是Notes风格上下文中的共享卡片，不是真实Notepad或GPU/VNC截图，
不构成全主题GPU视觉、中文字体回退、用户外观认可或FPS验收。

隔离headless Wayland/V3D原生门槛 **18/18**通过：**14**个既有确认/文件Provider
场景，加**4**个真实Notepad模块文件/关闭流程。Notepad核对实际读写后Saved采用，
继续原生键盘编辑、持久Error、真实Change location操作、取消后草稿与外部文件保留，
以及失败Close先退休后再恢复。旧Show前后采样为空的证据保留；追加一条明确的
SDK Info测试请求：真实Saved和原生编辑之后，显示前存在非零seat及有效编辑节点，
Show和实际采用后两者完全相同，不再点击即继续键入，草稿更新而磁盘快照不变。
最终4个Notepad流程再次通过，非空焦点样本为1；该专项不宣称多一次业务成功。
Provider继续核对真实owner/request/epoch及快照采用、导航/改名、覆盖重审、
旧描述拒绝、一次结果、FD排空和无业务写文件。临时WM均已回收；原生
采用Square Light，持久Error检查其期限为空，自动到期由独立策略/SDK测试覆盖，
没有将其写成真实原生墙钟到期、触控屏或性能测试。

规范检查通过：**462**个生产文件、**193**个测试文件，最大生产文件714行；Skill、
diff及WM客户端符号/Qt依赖、Notepad模块前端依赖边界检查通过。安装边界只读审计
核对21份生成安装脚本、62个安装来源及65个build share文件，三份共享Provider DSL
的构建副本与最终源码SHA一致，反馈安装到`share/prism/ui`；测试、probe与fixture
不进入生产安装。该审计没有执行install、CPack或验证新deb。

构建中再次发现派生`.o`/`.a`内容损坏及archive成员与当前独立对象不一致。恢复相关
派生产物后，最终审计的**39**个有效静态archive、**248**个成员全部与当前对象一致，
六个先前受影响对象检查通过，再完成最终构建与上述回归。损坏原因仍未确定，原失败、
差异和恢复记录保留，不归因于编译器、存储或硬件，也不以当前产物通过宣称环境问题
已根治。证据以`dist/validation/interface-system-step6a/`中的`ctest-final.log`、
`visual-final.log`、`native-notepad-focus/native-gates.json`、
`native-provider/native-gates.json`及最终artifact/package/boundary审计为准。

本阶段源码已接入，当前通过范围如上；未提交新commit、生成deb、部署或替换正式VNC。
正式版本仍为**0.1.0-26**。下一步6b先定义Tooltip与受信系统会话确认入口，再进入
图05主题可定义的任务开合、owner退后/恢复和中断/reduced motion，继续复用现有
时间驱动动画、任务身份与输入epoch，普通反馈不能代理系统会话授权。
