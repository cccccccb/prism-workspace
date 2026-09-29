# 客户端 Scene：布局、绘制与运行时主题

日期：2026-09-26。本文保留早期检查点的实现背景；当前主题接口以 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 为准，新主题功能的构建、测试与部署结果另行记录。

`prism_client_scene` 链接纯值契约 `prism_contracts`、主题值校验/编码库 `prism_theme_contract` 和资源工作线程依赖，不链接 WM、Wayland、Skia 或 DSL parser。`prism_client_dsl` 使用共享的 `prism_dsl_syntax`，将语法树转换为 Blueprint；运行期 Scene 不持有 AST 或后端对象。应用 UI 和主题包使用同一语法解析实现，但分别进行组件和主题语义校验。

Scene 支持 `HStack`、`VStack`、`Card`、`Text`、`Button`、`Image`、`Icon`、`IconButton`、`Separator`、`Progress`、`Toggle`。布局、样式与 binding 按 schema 检查；未支持的组件和修饰符抛出错误。当前输入命中返回节点身份，统一状态机确认激活后才派发 action，业务状态仍由 module 更新。Slider 拖动语义尚未实现。

`SetSlot` 是带类型的 `SetBinding` 的字符串便捷入口；Scene 的属性存储依据 schema 元数据决定 Layout/Paint/Composite dirty，背景色变化标记 Paint，viewport 的实际尺寸变化标记 Layout/Paint。`Build` 在无 Paint/Layout 变化时返回空；纯 Composite 更新由平台提交状态，不生成 DisplayList。输出是进程内 DisplayList。文字 glyph id 与位置由调用方提供的 shaping 接口生成；Skia 后端现提供 HarfBuzz + FreeType 实现。固定尺寸与均分的 Row/Column、基础裁剪、圆角和文字命令已通过单元测试。

构建链路现为 Scene snapshot → `LayoutEngine` → `RenderTreeBuilder` → `DisplayListBuilder`。Layout dirty 才重新计算几何与文字；Render Tree 保存视觉图元、裁剪、来源版本，并复用未变节点记录。需要新 DisplayList 时仍完整生成；纯状态提交不走这条链路，平台强制像素提交也可复用已构建列表。增量布局和分块缓存尚未实现。

此文件记录 Scene 首个检查点。后续已加入 Skia CPU 诊断后端、HarfBuzz 字体排版和真实 Wayland 测试窗口，见 [SKIA_BACKEND_PI.md](SKIA_BACKEND_PI.md)。PNG 图片资源的首个异步检查点见 [IMAGE_RESOURCES.md](IMAGE_RESOURCES.md)；GLES GPU 后端及复用客户端入口见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md) 和 [CLIENT_APP_SDK.md](CLIENT_APP_SDK.md)。

验证：

```sh
cmake --build build -j4
ctest --test-dir build -R '^client_scene_test$' --output-on-failure
```

## 视觉与通用布局升级（2026-09-26）

生产 DSL 新增 `Image`、`Icon`、`IconButton`、`Separator`、`Progress`、`Toggle`。所有控件继续使用通用 action 和带类型 binding；业务状态与应用行为保留在 module。`IconButton("play", "toggle_play", ...)` 的图标可使用 `$playback_icon` 动态绑定；`Progress(value: $progress)` 接受 0–1；`Toggle(checked: $enabled, action: "toggle")` 接受 bool。Toggle 和 Progress 无内置业务计时或状态翻转。Slider 拖动语义尚未加入。

主题唯一源码为 `resources/themes/<id>/theme.prism`，由主题编译器输出 `contracts::ThemeSnapshot`，不再生成静态样式头。UI 的 `"@panelTint"` / `"@font_body"` 在 Blueprint 中保留为 `ThemeRef(name, target)`；Scene 使用当前快照解析并验证目标属性的类型、范围和组件约束。未知 token 或类型错配在创建 Scene / 应用主题时明确拒绝，不回退默认主题。语法与包格式见 [THEME_AUTHORING.md](THEME_AUTHORING.md)。

布局先测量再放置：Text 使用真实 shaping 的宽高，图片使用解码尺寸，Row/Column 的固有尺寸累加主轴与 gap。`width/height > 0` 指定尺寸；容器未指定主轴尺寸时填充剩余空间，`flex` 明确指定权重；文本和叶控件默认采用固有主轴尺寸；自动图片在流式容器中保持解码固有宽高，指定尺寸或 Card 的 fill 槽位才提供缩放目标。`align` 为交叉轴 start/center/end/stretch，`justify` 为主轴 start/center/end/spaceBetween；`paddingX/Y` 可覆盖统一 padding，`inset` 是节点四周留白。

Card 叠放子项支持 `anchor: "left"/"center"/"right"`。center 子项依据整个 Card 内容区居中；左右 anchor 的可用宽度向 center 让位，配合 `overflow: "clip"` 限制侧区内容。未指定 anchor 的 Card 子项沿用 fill。固定宽子项在 Row/Column 不逐个缩短；`overflow: "clip"` 明确裁剪越界内容，`visible` 允许越界绘制。窄屏应用因此必须选择自身的布局与溢出策略。

`borderWidth/borderColor`、`shadowBlur/shadowY/shadowColor`、`innerShadowBlur/innerShadowY/innerShadowColor` 进入 RenderTree 视觉记录，统一转成 DisplayList。`clip: true` 或 `overflow: "clip"` 在存在 cornerRadius 时使用圆角 clip。应用窗口本身的外框和跨窗口阴影由 compositor 负责；客户端属性用于局部卡片/控制面板。

`Scene::InputRegions()` 根据 `inputShape` 和可见材料/控件生成输入轮廓，并裁到祖先 clip。`bounds` 保持材料整个圆角区域可交互，即使 tint alpha 和 blur 都为零；`visible` 沿可见材料、图片与 action 节点收集区域。任意嵌套圆角 clip 的交集按逻辑像素行求交，避免透明角吞点击。输入轮廓仅在布局或影响输入形状的属性变化后重新计算，hover/键盘焦点等 Paint 更新复用缓存；透明 Shell 外部留白不吞点击。输入 hit 同时尊重矩形/圆角 clip。Tab 遍历 action，Enter/Space 调用当前 action，点击只处理主按键。键盘行为沿用业务 action，不直接改业务 binding。

`backdropBlur` 范围 0–48，圆角 0–256，每 surface 最多提交 8 个背景效果区域。前端计算显式 blur、动态 binding 和 token 引用的潜在区域数量；主题材料解析后还须检查实际效果区域。`Scene::SurfaceEffects()` 先与 viewport、祖先 clip 求交，完全隐藏的区域不提交。矩形或能精确保留的统一圆角矩形交集正常提交；偏移圆角相交、截断曲线等不能用 v1 表示时明确诊断，不扩大区域。最终 x/y 为 ±8192、宽高 ≤8192，非法请求不发送。主题预检遇到这些问题会拒绝候选并保留旧主题；普通呈现遇到未预见的非法效果仍会失败。无 AST/控件树跨进程；节点增量布局与 DisplayList 分块缓存仍未加入。

新增 `visual_scene_test` 覆盖全栏居中、窄栏让位、flex 比例、控件 typed binding、圆角命中和祖先 clip 输入区；raster/GLES 测试检查透明预乘与圆角裁剪。本段描述实现契约，现场外观验收由实机记录独立给出。

## 运行时主题接口

`Scene(Blueprint, ShapeText, ResourceId font = {}, optional<ThemeSnapshot> theme = {})` 接收可选初始快照。只有字面量的 Blueprint 可以不提供主题；包含 token 或 `material` 的 Blueprint 必须提供合法快照。`material: "window"` 等静态语义引用提供 tint、圆角、blur、边线、内/外阴影与输入政策，显式字面量、token 或业务 binding 覆盖同一属性。直接 `SetProperty` 修改属性会解除其 token 引用。

`ApplyTheme(snapshot, diagnostic)` 先在独立候选 Scene 中验证全部引用、布局、资源尺寸、效果和输入轮廓，再将准备好的样式值替换到现有节点。成功返回 true，包括合法的无变化更新；失败返回 false 并保留旧状态。现有 NodeId、业务 binding、动作、文本、勾选/进度值、图片 ID 与解码状态保持。主题 generation 与 DisplayList generation 分开。

RenderTree 的 hover/focus 颜色和焦点线宽、Toggle 轨道/滑块圆角、滑块颜色/间距、默认内阴影偏移来自快照 Controls，后端不判断具体主题名称。主题安装比较可见节点解析后的属性，按 schema 标记需要的 Layout/Paint/Composite；只有身份或 generation 改变且解析样式相同，不推进像素版本。隐藏节点仍保存新样式，重新显示时读取最新值。Controls 的任一变化目前保守标记 Paint，尚未判断哪些可见控件实际使用了该值，因此不能把所有隐藏主题变化都视为零绘制。圆角 clip 与客户端图元随像素提交更新；纯背景效果或输入政策可独立提交 surface 状态。完整会话分发、ACK 与失败恢复规则见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。本节描述已接入的接口；验证结果由本轮测试记录给出。

相同快照可直接接受；身份或 generation 不同的候选仍执行隔离预检，可能重新布局和整形文字。候选的工作不计入已提交 Scene 的渲染计数，零 GPU/Swap 或零实际 Scene Build 不表示主题安装没有 CPU 工作。

## 像素版本与状态确认（第三阶段）

`PixelsRevision()` 从 1 开始，可见 Paint/Layout 失效推进该版本；它与 DisplayList generation、surface commit 和帧回调独立。`Build` 消耗 Paint/Layout，保留 Composite，不确认像素已经提交。SDK 只在像素提交成功后记录对应版本；失败终止当前前端连接，不能因列表已构建而确认 Swap 成功。

`AcknowledgeComposite()` 只清除 Composite，不清除待处理 Paint/Layout。对应 metadata 随 State/Pixels 成功提交后才能确认；若准备阶段已检查 metadata，协议请求与上次相同，或可选效果扩展不可用而没有请求可发送，checked-identical `None` 也可确认该状态。因像素回调节流而推迟准备的 `None` 不属于这个例外，仍保留待处理状态和像素需求。四种提交结果及等待契约见 [RENDER_SCHEDULING_AND_INVALIDATION.md](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 9 节。

第四阶段继续由 Scene 生成完整快照、布局/Render Tree 和 DisplayList；SDK 在 renderer 的通用接口上比较成功列表，按实际绘制范围产生内容损伤，平台 buffer age 历史决定修复区域。局部像素绘制不等于节点增量布局或显示列表分块缓存；WM 只接收原生 surface 与损伤，不接收 Scene/DSL。契约、全量回退和像素验收见上述规范第 11 节。


## 通用可见性与图标（0.1.0-7）

所有组件支持 `visible: true|false` 或 `visible: $布尔绑定`，默认 true。隐藏节点及其子树不参加测量、flex 分配、间距、绘制、命中、键盘焦点、输入区域和背景效果；隐藏立即撤销旧 hover/focus，下一次布局将子树 bounds 清零。节点、资源及 binding 保留，重新显示后按当前状态测量/绘制。`Scene::IsVisible(NodeId)` 查询包含祖先的实际可见性；它与材料的 `inputShape: "visible"`（收集可见内容输入区域）是不同接口。

`SetProperty`（包括通过它执行的 `SetBinding`）按修改前后的实际可见性决定是否失效。若节点在修改前后均因自身或祖先而隐藏，属性仍更新到存储、缓存样式和显式覆盖记录，节点 revision 递增；主题引用遵循正常覆盖规则，直接修改某属性会解除该属性的 token 引用，其余引用保留。这些修改不新增 Scene 的 Layout/Paint/Composite dirty 或输入轮廓失效标记。隐藏父节点下的子节点将自己的 `visible` 从 false 改为 true，也属于此情况；它的实际可见性仍为 false。

可见节点的属性变化继续按 schema 失效。实际可见性从可见变隐藏或从隐藏变可见时，仍触发完整布局、绘制、合成及输入轮廓更新；隐藏时清除旧输入状态。当前交互实现会在重新显示后按指针位置和新几何重新命中 hover，不恢复旧捕获、按下或键盘焦点。同一个 binding 同时命中隐藏与可见节点时，隐藏目标保存状态，可见目标正常失效，不能因存在隐藏目标而跳过整条 binding 的更新。

重新显示父节点后，下一次完整 snapshot/布局会读取隐藏期间的最新文字、进度、样式、尺寸、显隐值与主题覆盖，并据此更新绘制、命中和输入轮廓。若本次只有仍然隐藏的属性更新，且没有其他待处理 dirty，`Build` 返回空，不生成 DisplayList，也不推进 DisplayList generation；其他可见更新产生的 dirty 保留。本规则完善通用可见性的失效语义，布局仍按既有全量流程执行，未引入节点增量布局或 DisplayList 分块缓存。`visual_scene_test` 覆盖隐藏更新不提交、共享 binding 的可见目标失效、重新显示后的最新几何/图元/输入，以及静止指针重新命中而旧焦点/捕获不复活。

通用矢量图标新增 `layers`、`rectangle`、`drop`、`wifi-off`、`error`，在契约枚举尾部追加，不改变已有图标编号。符号几何属于后端资源实现，颜色、尺寸与布局仍由 DSL/主题决定；没有 Theme ID 或应用名绘制分支。

## 通用交互状态第一阶段（2026-09-29）

`Scene::HitTest(point)` 返回 `HitResult { NodeId node, LogicalPoint localPosition }`，
身份限定在当前 Scene；SDK 另用 UiLoadId 隔离加载代数。相同 action 的多个节点独立
命中，不按字符串重新搜索。命中遵守绘制顺序、圆角与祖先 clip，并拒绝非有限坐标。

`HandleInput(WindowEvent)` 返回 `InteractionResult`：`changed` 表示局部状态变化，
可选 `Activation { node, action }` 表示已经结束并确认的动作。捕获按 InputSource
维护，键盘焦点按 seat 维护；释放必须匹配原节点、来源和原 action。该捕获只作用于
已经获得的 surface 输入，不是跨应用全局抓取。取消和动作确认均先于业务回调。

`State(NodeId)` 提供 hovered、pressed、captured、focused、focusVisible、enabled。
pressed 表示仍在原目标内的有效指针按下或有效键盘按下；离开时可保留 captured 而撤销
pressed。鼠标聚焦不强制显示焦点环，键盘导航明确显示。旧 hover/focus 图元现在从同一
状态快照取值，主题 Controls 继续提供颜色和线宽，尚未增加声明式 StateRule。

`SetEnabled(NodeId, bool)` 是 Scene 的 C++ 输入接口，父节点禁用影响整棵子树，立即
取消相关输入，不改写业务 binding。它不是新增 DSL `enabled` 属性。`CancelInput()`
清理当前 Scene 的输入；隐藏、区域替换与 action 改变按目标身份取消，存活且未改变的
区域保留自己的捕获。主题/区域事务的提交后清理不分配新的输入状态存储。

局部状态只产生 Paint 失效；同一目标内没有状态变化的 motion 不推进像素版本。
布局或命中形状变化后按当前坐标重新命中静止指针，普通颜色/进度动画不重复执行该过程。
重新显示时的 hover 来自当前几何重新命中，不恢复旧捕获、按下或键盘焦点。
`ActionAt`、`SetPointer`、`FocusNext`、`FocusedAction` 保留为 Scene 级兼容查询/便利接口；
SDK 动作分派统一走 HandleInput，没有第二套按下即激活路径。

状态作用域 DSL、独立感应区、拖动识别、触摸、transform/opacity 与成功提交对应的命中
快照仍待后续。接口、实施顺序与验证记录见[交互与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)。
