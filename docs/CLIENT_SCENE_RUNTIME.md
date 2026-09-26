# 客户端 Scene：布局、绘制与运行时主题

日期：2026-09-26。本文保留早期检查点的实现背景；当前主题接口以 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md) 为准，新主题功能的构建、测试与部署结果另行记录。

`prism_client_scene` 链接纯值契约 `prism_contracts`、主题值校验/编码库 `prism_theme_contract` 和资源工作线程依赖，不链接 WM、Wayland、Skia 或 DSL parser。`prism_client_dsl` 使用共享的 `prism_dsl_syntax`，将语法树转换为 Blueprint；运行期 Scene 不持有 AST 或后端对象。应用 UI 和主题包使用同一语法解析实现，但分别进行组件和主题语义校验。

Scene 支持 `HStack`、`VStack`、`Card`、`Text`、`Button`、`Image`、`Icon`、`IconButton`、`Separator`、`Progress`、`Toggle`。布局、样式与 binding 按 schema 检查；未支持的组件和修饰符抛出错误。按钮命中返回 action，业务状态仍由 module 更新。Slider 拖动语义尚未实现。

`SetSlot` 是带类型的 `SetBinding` 的字符串便捷入口；Scene 的属性存储依据 schema 元数据决定 Layout/Paint dirty，背景色变化只标记 Paint，viewport 变化标记 Layout/Paint。`Build` 在无变化时返回空，不产生新的提交。输出是进程内 DisplayList。文字 glyph id 与位置由调用方提供的 shaping 接口生成；Skia 后端现提供 HarfBuzz + FreeType 实现。固定尺寸与均分的 Row/Column、基础裁剪、圆角和文字命令已通过单元测试。

构建链路现为 Scene snapshot → `LayoutEngine` → `RenderTreeBuilder` → `DisplayListBuilder`。Layout dirty 才重新计算几何与文字；Render Tree 保存视觉图元、裁剪、来源版本，并复用未变节点记录。当前 DisplayList 仍在每次提交时完整生成，增量布局和分块缓存尚未实现。

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

RenderTree 的 hover/focus 颜色和焦点线宽、Toggle 轨道/滑块圆角、滑块颜色/间距、默认内阴影偏移来自快照 Controls，后端不判断具体主题名称。主题更新统一标记布局、绘制和合成 dirty，圆角 clip、输入区域和 surface effects 随下一次 buffer commit 生效。完整会话分发、ACK 与失败恢复规则见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。本节描述已接入的接口；验证结果由本轮测试记录给出。
