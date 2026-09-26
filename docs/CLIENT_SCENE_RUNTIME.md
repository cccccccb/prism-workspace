# 第四步检查点：独立客户端 Scene

`prism_client_scene` 只链接 `prism_contracts`。`prism_client_dsl` 单独编译现有 lexer/parser，把一次性 AST 转为客户端 Blueprint；运行期 Scene 不持有 AST、WM、Wayland 或 ImGui 对象。UI 状态与布局均由客户端 Scene 保存。

目前支持 DSL 的 `HStack`、`VStack`、`Card`、`Text`、`Button`，以及 `width`、`height`、`font`、`spacing`、`padding`、`cornerRadius`、`clip` 的基础语义。`Button` 通过客户端局部坐标命中并返回 action。未支持的组件和修饰符会抛出错误；现有含 blur、Slider 等的完整演示样例尚不能由此模块运行。

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

主题唯一输入为 `resources/themes/prism-glass.json`，构建生成纯 `contracts/theme_tokens.hpp`，客户端与 WM 使用同一版本。属性引用采用字符串 `"@panelTint"` / `"@font_body"`；frontend 先解析 token，再执行属性 schema 的类型、范围和组件约束。未知 token 与类型错配明确拒绝，不回退默认值。

布局先测量再放置：Text 使用真实 shaping 的宽高，图片使用解码尺寸，Row/Column 的固有尺寸累加主轴与 gap。`width/height > 0` 指定尺寸；容器未指定主轴尺寸时填充剩余空间，`flex` 明确指定权重；文本和叶控件默认采用固有主轴尺寸；自动图片在流式容器中保持解码固有宽高，指定尺寸或 Card 的 fill 槽位才提供缩放目标。`align` 为交叉轴 start/center/end/stretch，`justify` 为主轴 start/center/end/spaceBetween；`paddingX/Y` 可覆盖统一 padding，`inset` 是节点四周留白。

Card 叠放子项支持 `anchor: "left"/"center"/"right"`。center 子项依据整个 Card 内容区居中；左右 anchor 的可用宽度向 center 让位，配合 `overflow: "clip"` 限制侧区内容。未指定 anchor 的 Card 子项沿用 fill。固定宽子项在 Row/Column 不逐个缩短；`overflow: "clip"` 明确裁剪越界内容，`visible` 允许越界绘制。窄屏应用因此必须选择自身的布局与溢出策略。

`borderWidth/borderColor`、`shadowBlur/shadowY/shadowColor`、`innerShadowBlur/innerShadowColor` 进入 RenderTree 视觉记录，统一转成 DisplayList。`clip: true` 或 `overflow: "clip"` 在存在 cornerRadius 时使用圆角 clip。应用窗口本身的外框和跨窗口阴影由 compositor 负责；客户端属性用于局部卡片/控制面板。

`Scene::InputRegions()` 将可见材料/控件区域转换为平台输入轮廓，并裁到祖先 clip；任意嵌套圆角 clip 的交集按逻辑像素行求交，生成最终输入矩形，避免透明角吞点击。输入轮廓仅在布局或影响输入形状的属性变化后重新计算，hover/键盘焦点等 Paint 更新复用缓存；透明 Shell 外部留白不吞点击。输入 hit 同时尊重矩形/圆角 clip。鼠标悬停提供静态高光；Tab 遍历 action，Enter/Space 调用当前 action，点击只处理主按键。键盘行为沿用业务 action，不直接改业务 binding。

`backdropBlur` 范围 0–48，圆角 0–256，每 surface 最多 8 个可能使用的背景效果节点（包括绑定节点）。前端拒绝超过区域上限的 Blueprint。`Scene::SurfaceEffects()` 先将布局区域与 viewport、祖先 clip 求交，完整隐藏的区域不提交；矩形或能精确保留的统一圆角矩形交集正常提交。偏移圆角相交、截断曲线等产生非统一圆角 mask 时，v1 无法表达，SDK 明确报告 unsupported 并中止该呈现，不扩大区域或泄漏到 clip 外。最终区域 x/y 为 ±8192、宽高 ≤8192，超范围拒绝并给出诊断，不发送非法 Wayland 请求。无需裁剪的原圆角保持不变；同 bounds 的圆角 clip 取更严格的轮廓。无 AST/控件树跨进程；布局缓存与 DisplayList 分块缓存仍未加入。

新增 `visual_scene_test` 覆盖全栏居中、窄栏让位、flex 比例、控件 typed binding、圆角命中和祖先 clip 输入区；raster/GLES 测试检查透明预乘与圆角裁剪。本段描述实现契约，现场外观验收由实机记录独立给出。
