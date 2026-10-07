# 客户端 Scene：布局、绘制与运行时主题

更新日期：2026-10-06。本文保留早期检查点的实现背景；当前主题接口以 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 为准，Popup 子层当前能力以 [4k2 生命周期](POPUP_TARGET_LIFECYCLE.md) 为准，各阶段构建、测试与部署结果另行记录。

`prism_client_scene` 链接纯值契约 `prism_contracts`、主题值校验/编码库 `prism_theme_contract`、轮廓/effect 校验契约和资源工作线程依赖，不链接 WM、Wayland、Skia 或 DSL parser。`prism_client_dsl` 使用共享的 `prism_dsl_syntax`，将语法树转换为 Blueprint；运行期 Scene 不持有 AST 或后端对象。应用 UI 和主题包使用同一语法解析实现，但分别进行组件和主题语义校验。

Scene 支持 `HStack`、`VStack`、`Card`、`Text`、`Button`、`Image`、`Icon`、`IconButton`、`Separator`、`Progress`、`Toggle`，交互第二阶段另提供 `InteractionTarget` 与 `Visual`，后续已加入值控件、ScrollView 及 Popup/Menu。布局、样式与 binding 按 schema 检查；未支持的组件和修饰符抛出错误。当前输入命中返回节点身份，统一状态机确认激活后才派发 action，业务状态仍由 module 更新。Slider 当前已支持 typed Number 鼠标拖动/键盘会话，见本文末尾及 [值控件契约](CONTROL_VALUE_CONTRACT.md)。

`SetSlot` 是带类型的 `SetBinding` 的字符串便捷入口；Scene 的属性存储依据 schema 元数据决定 Layout/Paint/Composite dirty，背景色变化标记 Paint，viewport 的实际尺寸变化标记 Layout/Paint。`Build` 在无 Paint/Layout 变化时返回空；纯 Composite 更新由平台提交状态，不生成 DisplayList。输出是进程内 DisplayList。文字 glyph id 与位置由调用方提供的 shaping 接口生成；Skia 后端现提供 HarfBuzz + FreeType 实现。固定尺寸与均分的 Row/Column、基础裁剪、圆角和文字命令已通过单元测试。

构建链路现为 Scene snapshot → `LayoutEngine` → `RenderTreeBuilder` → `DisplayListBuilder`。Layout dirty 才重新计算几何与文字；Render Tree 保存视觉图元、裁剪、来源版本，并复用未变节点记录。需要新 DisplayList 时仍完整生成；纯状态提交不走这条链路，平台强制像素提交也可复用已构建列表。增量布局和分块缓存尚未实现。

本段保留 Scene 首个检查点的背景。后续已加入 Skia CPU 诊断后端、HarfBuzz 字体排版和真实 Wayland 测试窗口，见 [SKIA_BACKEND_PI.md](SKIA_BACKEND_PI.md)。PNG 图片资源的首个异步检查点见 [IMAGE_RESOURCES.md](IMAGE_RESOURCES.md)；GLES GPU 后端及复用客户端入口见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md) 和 [CLIENT_APP_SDK.md](CLIENT_APP_SDK.md)。

## 通用轮廓接入（2026-10-06，4g / 4h）

局部容器和 Popup/Menu 可在 DSL 中声明 typed `Contour` 元数据，路径在语义准备阶段
展平并验证一次；它不成为布局节点。Blueprint 保存局部规范点，Scene 的 detached
snapshot 按布局原点整体量化、平移并验证，再发布不可变共享几何。RenderTree、
绘制/裁剪、实时命中、输入快照和 effect 消费相同的最终点；点未改变则复用存储。
轮廓替代 cornerRadius 的形状语义，布局尺寸和命中返回的局部坐标仍以节点 bounds 为准。

混合输入交集直接使用多边形与解析圆角的像素中心判断，保留凹形的多个跨度；当前
上限为八个去重形状和 65,536 个输入矩形。外阴影与 Popup 外点关闭屏障继续有独立职责。
背景效果只保留能够证明为完整既有形状的交集；部分相交或无法证明的交集拒绝候选，
不扩大到 bounds。窗口外围装饰未改动，Contour 与 window 材质不可组合。
完整语法、量化误差、滚动/事务行为和后续能力见
[通用轮廓契约](SURFACE_CONTOUR_CONTRACT.md)。

验证：

```sh
cmake --build build -j4
ctest --test-dir build -R '^client_scene_test$' --output-on-failure
```

4h 的 `AttachedPanelRecipe` 保留 typed 数值/主题引用；Snapshot 只消费解析后的
`PanelContourSpec` 和最终 PopupPlacement。正文 bounds 与局部准备结果分开，颈部
允许超出正文；几何键包含尺寸、方向、中心和参数。PrepareSceneContours 在 detached
snapshot 中先放置静态路径，再准备面板配方，全部校验成功才发布共享轮廓与定位。
颜色更新复用缓存；参数变化走主题候选与 Layout/Paint/Composite，旧快照不变。

历史 4i 只打通跨 surface 定位/传输与 WM 生命周期，当时仍保留 Scene 同窗口布局输出。
此阶段确定后续导出必须根据 xdg_popup 最终 configure 重新确定局部 viewport/轮廓，
并将资源、提交与输入 scope 关联到实际 surface；不可复制业务模块或直接读取渲染线程代理。
具体阶段与坐标见 [Popup/Menu 契约](POPUP_MENU_CONTRACT.md)。
历史 4j 的[共享 GPU/独立 target](MULTI_SURFACE_GPU_CONTRACT.md)拆分保持上述 Scene 输出；
当时仅底层支持多个 WSI，并未自动导出 Scene。4i/4j 的验证记录保留在各阶段文档。

4k1 新增 [Popup surface 计划](POPUP_SURFACE_PLAN_CONTRACT.md)：活动面板 Build 保存
已准备的不可变快照；Capture 生成当前请求，无像素变化的状态更新可刷新已解析值，
不调用 Build 或布局。Prepare 根据最终 configure 在快照值中重新布局子树并导出
DrawList/局部输入描述。没有另建 Scene 或重解析 DSL。局部
输入描述 `scene=0`，根输入入口继续拒绝它；真实身份在 request 中。4k1 当时生产
仍使用根 surface 的原有布局与快照；其历史验证不能替代下面 4k2 的提交与输入验证。

## Popup 子层采用与局部布局复用（2026-10-06，4k2）

当前 Host 在 scale=1、无 backdrop 采样需求且可准确准备局部布局时自动创建同连接
native child。`PopupSurfaceRequest::requires_backdrop` 来自当前可见子树的解析值，
不按主题名称猜测。毛玻璃、编辑器和不支持的裁剪/变换保留 root 路径；跨 surface
effect descriptor 和 parent 正文采样为 4k3。这里描述当前源码接入，未替换安装中的
桌面或 VNC 版本。

UI 所有者根据原生最终 configure 准备 owning `PopupSurfacePlan`，保留不可改造的
prepared provenance；render worker 仅消费不可变 `PopupFramePacket`，共享既有
Context/Ganesh、图片和业务模块。Scene 不持 Wayland/EGL 代理，worker 不读 Scene
或执行布局、文字整形、属性动画。没有第二个 Scene 或应用渲染线程。

首次实际 child Pixels 成功后，Host 才调用 `AdoptPopupSurface(plan, identity)`。
返回值表示 accepted，不等同于本次 root 像素变化。Scene 核对 prepared 记录、
Scene/popup epoch/parent configure 和 worker/target/lifetime/configure/adoption
sequence，生成私有可信输入副本。首次采用使 root Paint 和输入版本失效：后续
root 列表、输入及 effect 区域剔除同一面板，完整源快照仍保留供 child 导出。
root 和 child 提交独立，不承诺跨 surface 原子呈现。

后续 State 或核对过当前真实 pixel 基线的 None 也可采用新输入。adoption sequence
与 pixel ID 分开，metadata 更新不虚构新像素；child 提交/呈现反馈不推进 root
Preview/Master 里程碑。root `InputGeometry()`/`CaptureInputSnapshot()` 返回过滤后
的几何，版本保持严格单调；root `ApplyInputSnapshot()` 不覆盖 child captures。
child callback、反馈容量、buffer age、损伤和成功基线独立，稳定 child 不强制 root Swap。

`HandlePopupSurfaceInput(event, identity, snapshot)` 只接受当前已采用身份及 descriptor
指针。公开描述保持 `scene=0`，直接送 root HitTest/HandleInput/ApplyInputSnapshot
均无输入资格。内部处理使用实际 child-local 几何，不临时改写根节点 bounds。
live action、enabled、节点代次和滚动状态继续否决旧内容激活；稍旧的成功提交可以
更新已显示描述，但不能倒退实时滚动语义。连续滚轮使用局部可滚动范围，等待提交
时仍可累计；输入禁用或 action 改变不被误判为外点关闭。

内部 root↔child 焦点转移带 `internal_transfer`，不会误关逻辑面板。非 grab 菜单
的键盘可能仍在 parent，Host 将无坐标的按键交给当前已采用 scope；root 鼠标仍
按根几何处理外点关闭。菜单替换/返回/关闭自动撤销旧采用。`RevokePopupSurface`
使用最近成功的完整身份，取消局部捕获，恢复 root 并按其 viewport 重布局。

最终局部布局按 popup epoch、configure、正文几何和布局签名缓存。纯颜色、交互和
滚动替换当前值、复用几何；文字、字体、尺寸、padding 与拓扑变化重新 Measure/Place。
`GetPopupSurfacePreparationStats()` 记录成功 plans、实际局部布局与复用，根
`GetRenderStats()` 保留原语义。这一配置级缓存不等于通用节点增量布局或 DisplayList
分块缓存。完整生命周期与当前验证范围见
[子层生命周期规范](POPUP_TARGET_LIFECYCLE.md)；构建/测试结果由当次执行文档记录。

## 视觉与通用布局升级（2026-09-26）

2026-09-26 检查点新增 `Image`、`Icon`、`IconButton`、`Separator`、`Progress`、`Toggle`。所有控件继续使用通用 action 和带类型 binding；业务状态与应用行为保留在 module。`IconButton("play", "toggle_play", ...)` 的图标可使用 `$playback_icon` 动态绑定；`Progress(value: $progress)` 接受 0–1；`Toggle(checked: $enabled, action: "toggle")` 接受 bool。Toggle 和 Progress 无内置业务计时或状态翻转。该阶段尚未加入 Slider 拖动，其后续实现见本文值控件段落。

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
状态快照取值，主题 Controls 继续为原有控件提供颜色和线宽；第二阶段新增的声明式
StateRule 见下节。

`SetEnabled(NodeId, bool)` 是 Scene 的 C++ 输入接口，父节点禁用影响整棵子树，立即
取消相关输入，不改写业务 binding。v22 之后源码同时支持 DSL `enabled` Boolean 属性/绑定；旧 v22 不支持。`CancelInput()`
清理当前 Scene 的输入；隐藏、区域替换与 action 改变按目标身份取消，存活且未改变的
区域保留自己的捕获。主题/区域事务的提交后清理不分配新的输入状态存储。

局部状态只产生 Paint 失效；同一目标内没有状态变化的 motion 不推进像素版本。
独立 Scene 的兼容输入路径在布局变化后重新命中；SDK 的快照路径等成功提交确认后
才更新静止指针。普通颜色/进度动画不重复执行该过程。
重新显示时的 hover 来自当前几何重新命中，不恢复旧捕获、按下或键盘焦点。
`ActionAt`、`SetPointer`、`FocusNext`、`FocusedAction` 保留为 Scene 级兼容查询/便利接口；
SDK 动作分派统一走 HandleInput，没有第二套按下即激活路径。

独立感应区、状态作用域与装饰子树呈现由第二阶段接入；触摸与提交快照由第三阶段接入。
拖动识别和交互节点整体变换继续后续实施。接口与验证记录见
[交互与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)。

## 固定交互目标与状态呈现第二阶段（2026-09-29）

`InteractionTarget` 与 `Visual` 使用 Card 的叠放布局。前者是稳定输入目标，可有具名
action，也可只接收局部状态；无 action 时释放不生成空 Activation。后者及其所有后代
不参加命中和 surface 输入区域，允许装饰运动而不改变父目标的点击位置。

`.state(when: "hovered", scope: "target", scaleX: 1.15)` 在纯准备阶段编译为 StateRule；
scope 只允许最近的 InteractionTarget，声明必须处于其 Visual 子树内。Scene 安装时
验证全部规则/主题引用并关联目标身份。运行期按目标状态解析覆盖，不向 module 发送
hover 或把动画样本回写 binding。基础属性与有效状态目标分开保存；业务或主题改变基础
值时，状态退出使用最新基础。快速重定向沿用当前呈现值，初次安装不自动播放。

当前条件为 hovered、pressed、captured、disabled、focused、focusVisible。普通属性优先级
为 disabled > pressed > captured > hovered；焦点条件使用独立属性，不同条件与其写同一
属性时编译拒绝。重复条件/属性、非法 scope、状态值 binding、类型和范围错误均拒绝。
StateRule 的属性及主题引用沿组件组合、Blueprint、区域事务与主题候选保留，计入准备预算。

Visual 的 translateX/Y、scaleX/Y、originX/Y、opacity 与 background 可声明状态；除了
originX/Y，其余新增属性可声明时长 Transition。Visual 内直接绘制节点的 foreground 可
响应目标状态。范围与可复制示例见[交互规范第 13 节](INTERACTION_AND_PRESENTATION_SPEC.md)。
`Progress.value` 的既有 Transition 保持业务目标语义，不扩展为状态覆盖属性。

Visual 不能作为组件根；其后代禁止 InteractionTarget、Slot、action、material、
backdropBlur 与 inputShape 声明。静态材料可以放在外部 Card 或 InteractionTarget，
从而保留玻璃/阴影布局而只移动内部装饰。Scene 也校验直接 Blueprint 和动态属性写入
的安全边界。InteractionTarget 本身不接受呈现变换；移动交互对象的同代输入协议未接通。

新增呈现属性随 SnapshotNode 进入 RenderTree/DisplayList；子树 opacity 采用整组图层
语义，而非逐图元 alpha 相乘。它们当前产生 Paint、完整列表更新及 Skia 回放，不产生
Layout 失效；临时 opacity 图层不等于保留缓存。无变化输入或已经结束的轨迹不应继续
Build/Render/Swap。旧控件保留主题 Controls 的默认反馈，InteractionTarget 由显式
StateRule 描述反馈，不在 renderer 中写死 Topbar/Dock 样式。

本阶段完整构建、60/60 CTest 和五项隔离 V3D SDK 门槛通过，像素对照与详细记录见交互规范第 13 节；尚未部署到现有 VNC 会话。


## 输入快照与触摸第三阶段（2026-09-29）

`InputGeometry()` 返回最近 Build/Capture 准备的 `shared_ptr<const InputSnapshot>`。每个快照包含
独立 Scene 身份和版本；节点记录身份、action、可用性、布局盒、圆角及祖先裁剪。
纯呈现动画复用原快照；布局/身份变化才重新检查几何。Action-only 更新产生 Composite
失效；SDK 通过 `CaptureInputSnapshot()` 单独冻结输入，不增加显示列表构建尝试或像素版本。
该接口要求布局已完成，未解析的 Layout 会被拒绝；圆角、裁剪与动作变化可在布局后独立
捕获。它不确认 Composite，也不表示 worker 已采用该候选。现有直接调用 Build 的路径
仍会同步准备快照。

`HandleInput(event, snapshot)` 始终按事件携带的已提交几何解释输入。空快照、跨 Scene
快照和已经失效的目标不能启动或完成动作；当前可见性、enabled、action 与节点存活检查
仍优先于旧几何。现有 `HandleInput(event)` 保留直接 Scene 测试/使用的本地几何语义。
SDK 始终使用显式快照重载，不回退未提交的最新布局。

`ApplyInputSnapshot(snapshot)` 在有序提交确认后更新已有指针/触点使用的几何，拒绝跨
Scene 和倒退版本。静止指针可能因此产生新状态或启动过渡；SDK 会发布相应反馈帧。
候选更新不提前覆盖事件几何，区域卸载与 UI 替换也不会使旧 NodeId 激活新内容。

TouchDown/Motion/Up/Cancel/Frame 使用 source+contact 身份，每个触点独立捕获目标，
不形成持久 hover。Up 按最后触点坐标和事件快照完成一次激活；滑出、取消、失焦、禁用、
卸载、关闭和逻辑设备撤回清理序列。原生 pointer button 和 touch down 保留 opaque
`protocol_serial`，未来系统控制必须由平台核对该凭据，客户端 source/time 不提供权限。

当前只支持现有固定/布局变化目标；Visual 仍只装饰。交互节点的呈现变换、逆矩阵命中、
手势识别、跨 surface 效果变换和组控制分开推进。第三阶段构建与验收结果见
[交互规范第 14 节](INTERACTION_AND_PRESENTATION_SPEC.md)。

## 连续手势（2026-09-30）

`InteractionTarget` 可声明 `.gesture(action: "drag", threshold: 6)`，手势元数据独立于
可动画绘制属性，PrepareComponent、Blueprint 和 Scene 使用相同校验。达到逻辑距离
阈值后产生 Begin，后续移动/释放/取消产生 Update/End/Cancel，认领拖动后抑制点击。
`dragging` 状态可驱动 Visual 规则；未达到阈值保留原点击行为。

`TakeGestureEvents()` 取走拥有数据的批次，同一次终止不会重复交付；直接 Scene 用户
须及时取走批次（保留序列最多 256）。SDK 负责 owner 线程派发与 UI 代次检查。
输入快照包含对应 gesture 元数据，旧几何不能为已经更换识别器的节点启动新手势。
这些通用接口不包含组沉浸、Dock 或 WM 分隔线策略；完整约束见
[布局控制计划第 8 节](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md#8-第二步源码交付连续手势与-typed-控制会话)。

## 界面系统行框（v22 之后源码）

`lineHeight` 为 Text/Button 标签/TextField/TextArea 的可绑定最小行框，支持主题 Number，
不允许 State/Transition。Text 按 LF/CRLF/CR 显式分行，编辑框光标/选区/命中与行框一致。
0 保留默认，过小正值扩展到字体度量；不新增自动折行或字体回退。
[配方与验证](INTERFACE_CONTROLS_AND_TYPOGRAPHY.md)记录全部 token 和兼容边界。

Checkbox 值语义容器及 ControlEdit 已接入：离散有效释放生成 typed Commit，外观由
Visual 子树与业务 binding 组合，独立值修订校验捕获。详见
[值控件契约](CONTROL_VALUE_CONTRACT.md)。Checkbox 不改变旧 Toggle 的 action 行为。

RadioGroup/Radio 与 SegmentGroup/Segment 已追加到组件 schema：组持有 selectedKey，
选项固定 key，selected 为只读状态。键盘组导航、单次 Tab 停靠与 String Commit 复用
既有值通路，未增加 WM 或绘制后端依赖；详见值控件契约第 9 节。

水平 Slider 现已接入 typed Number 会话：固定数值域、鼠标捕获、键盘连续预览与终结。
`TakeControlEvents()` 提取拥有数据的事件；隐藏/禁用/值修订/失焦取消 retained stream。
InputSnapshot 保存成功提交的 slider_track，视觉快照投影 track/fill/thumb 几何而不使
父布局失效。角色外观仍由 Visual 与主题定义，完整约束见值控件契约第 11 节。

纵向内容视口、裁剪、嵌套滚轮与焦点显露参见 [ScrollView 契约](SCROLL_VIEW_CONTRACT.md)。
