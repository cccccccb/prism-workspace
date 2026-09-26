# DisplayList 绘制契约

`DisplayList` 是客户端进程内的有序命令序列。`window` 指明目标窗口，`generation` 是 Scene 提交序号；该 C++ 对象不是 IPC 格式。列表生成后，提交者应保持其内容不变，直到回放结束。

- 坐标使用窗口内逻辑像素，原点在左上，X 向右、Y 向下。矩形为 `x/y/width/height`；宽高非负。当前 CPU 与 GLES 回放都按目标画布的 1:1 坐标执行；窗口输出缩放由平台层负责，尚未纳入命令本身。
- `Color` 是非预乘的 sRGB RGBA 字节；绘制采用 Skia 的 source-over 合成。回放开始时清为不透明黑色，随后严格按命令顺序绘制。图片解码为 RGBA，绘制时线性采样至目标矩形；当前没有源矩形、九宫格和颜色空间转换选项。
- 裁剪与仿射变换按命令顺序入栈、出栈，遵循嵌套作用域。`PushClipRect` 只允许以 `PopClip` 结束；`PushTransform` 只允许以 `PopTransform` 结束。矩阵为行主序六元组 `[a,c,tx,b,d,ty]`，对应 `x'=ax+cy+tx`、`y'=bx+dy+ty`。回放前校验栈配对、数值有限和资源是否就绪；无效列表不会写入目标画布。
- `ResourceId` 是一个客户端运行期本地 ID，0 无效。字体 ID 必须注册到字体表，图片 ID 必须注册到图片表；两种资源分别按命令类型查找。文字 shaping 产生的 glyph ID 必须来自该命令引用的同一字体文件。资源应在回放完成前保持有效。当前字体注册后不卸载，图片注册后可由同一 ID 更新。
- CPU 和 GLES 都通过同一个 Skia 回放器解释命令，差别仅在目标画布和 GPU 提交。结构验证由共享回放器完成；像素差异仍受光栅化、采样和颜色格式影响。

当前字体资源由 `RasterRenderer::RegisterFont` 注册，默认字体在构造时注册；`Shape(font, text, size)` 与 `DrawGlyphRun.font` 使用同一个 ID。图片通过 `ImageResources` 异步解码后注册到回放器，Scene 收到 `ImageReady` 才发出 `DrawImage`。
