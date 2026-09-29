# DisplayList 绘制契约

`DisplayList` 是客户端进程内的有序命令序列。`window` 指明目标窗口，`generation` 是 Scene 提交序号；该 C++ 对象不是 IPC 格式。列表生成后，提交者应保持其内容不变，直到回放结束。

- 坐标使用窗口内逻辑像素，原点在左上，X 向右、Y 向下。矩形为 `x/y/width/height`；宽高非负。当前 CPU 与 GLES 回放都按目标画布的 1:1 坐标执行；窗口输出缩放由平台层负责，尚未纳入命令本身。
- `Color` 是非预乘的 sRGB RGBA 字节；绘制采用 Skia 的 source-over 合成。回放开始时清为透明（RGBA 0），随后严格按命令顺序绘制。CPU BGRA8888 诊断目标与 GLES RGBA8888 输出均为预乘 alpha；颜色与 PNG 解码输入保持非预乘，由 Skia 转换。客户端默认不声明不透明区域，圆角之外可透出 compositor 的真实下层。图片支持 `fill/contain/cover`，采用线性采样；cover 在回放层按资源实际尺寸居中裁源，contain 居中留透明边。尚无九宫格和颜色空间转换选项。
- 裁剪与仿射变换按命令顺序入栈、出栈，遵循嵌套作用域。`PushClipRect`、`PushClipRoundedRect` 均只允许以 `PopClip` 结束；`PushTransform` 只允许以 `PopTransform` 结束。矩阵为行主序六元组 `[a,c,tx,b,d,ty]`，对应 `x'=ax+cy+tx`、`y'=bx+dy+ty`。回放前校验栈配对、数值有限和资源是否就绪；无效列表不会写入目标画布。
- `ResourceId` 是一个客户端运行期本地 ID，0 无效。字体 ID 必须注册到字体表，图片 ID 必须注册到图片表；两种资源分别按命令类型查找。文字 shaping 产生的 glyph ID 必须来自该命令引用的同一字体文件。资源应在回放完成前保持有效。当前字体注册后不卸载，图片注册后可由同一 ID 更新。
- CPU 和 GLES 都通过同一个 Skia 回放器解释命令，差别仅在目标画布和 GPU 提交。结构验证由共享回放器完成；像素差异仍受光栅化、采样和颜色格式影响。

当前 UI 字体整形由 `runtime::TextShaper` 持有 FreeType/HarfBuzz 状态；回放字体由 `RasterRenderer::RegisterFont` 注册 SkTypeface。两侧默认字体在构造时从同一配置路径注册，`TextShaper::Shape(font, text, size)` 与 `DrawGlyphRun.font` 使用相同本地 ID。图片通过 `ImageResources` 异步解码后注册到回放器，Scene 收到 `ImageReady` 才发出 `DrawImage`；`FramePacket` 另带该 DisplayList 实际引用图片的 UI 侧版本，提交前与已登记/上传版本核对。UI 与渲染所有者使用同一次 ConfigureWindow 固定的字体路径和默认字体 ID；运行期不支持换字体。后续开放可变字体时必须在帧/资源协议中加入字体身份与版本，不能只靠整数 ID。

## 本地视觉命令（2026-09-26）

`StrokeRoundedRect` 在几何范围内绘制描边；`RoundedRectShadow` 接收圆角、Gaussian sigma、Y 偏移、直 alpha 颜色和 `inset`。外阴影先于节点 clip 回放，不改变布局槽位或输入范围；内阴影在该圆角轮廓内回放。阴影参数为静态主题属性，本阶段无动画。

`DrawIcon` 引用 SDK 的 `VectorIcon` 枚举资源，使用统一的 24 单位 code-native 路径图形，始终按等比尺寸居中。它不依赖 emoji 或图标字体，也不携带应用名。支持 grid/music/settings/folder/terminal/play/pause/previous/next/volume/wifi/battery/search/sun/moon/power/check/chevron/refresh/cpu/memory/heart。

跨 surface 背景模糊不属于 DisplayList。Scene 将 `backdropBlur` 节点的已布局区域转换为 `SurfaceEffectRegion`；平台随下一次 `wl_surface.commit` 提交，由 compositor 采样实际下层。扩展不可用时仍按透明 tint 正常呈现，客户端不模拟背景模糊。
