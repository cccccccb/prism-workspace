# DisplayList 绘制契约

`DisplayList` 是客户端进程内的有序命令序列。`window` 指明目标窗口，`generation` 是 Scene 提交序号；该 C++ 对象不是 IPC 格式。列表生成后，提交者应保持其内容不变，直到回放结束。

- 坐标使用窗口内逻辑像素，原点在左上，X 向右、Y 向下。矩形为 `x/y/width/height`；宽高非负。当前 CPU 与 GLES 回放都按目标画布的 1:1 坐标执行；窗口输出缩放由平台层负责，尚未纳入命令本身。
- `Color` 是非预乘的 sRGB RGBA 字节；绘制采用 Skia 的 source-over 合成。回放开始时清为透明（RGBA 0），随后严格按命令顺序绘制。CPU BGRA8888 诊断目标与 GLES RGBA8888 输出均为预乘 alpha；颜色与 PNG 解码输入保持非预乘，由 Skia 转换。客户端默认不声明不透明区域，圆角之外可透出 compositor 的真实下层。图片支持 `fill/contain/cover`，采用线性采样；cover 在回放层按资源实际尺寸居中裁源，contain 居中留透明边。尚无九宫格和颜色空间转换选项。
- 裁剪、仿射变换与组透明度按命令顺序入栈、出栈，遵循嵌套作用域。`PushClipRect`、`PushClipRoundedRect` 均只允许以 `PopClip` 结束；`PushTransform` 只允许以 `PopTransform` 结束；`PushOpacity` 只允许以 `PopOpacity` 结束。三类作用域不可交叉关闭，总深度最多 256。矩阵为行主序六元组 `[a,c,tx,b,d,ty]`，对应 `x'=ax+cy+tx`、`y'=bx+dy+ty`。回放前校验栈配对、数值有限和资源是否就绪；无效列表不会写入目标画布。
- `ResourceId` 是一个客户端运行期本地 ID，0 无效。字体 ID 必须注册到字体表，图片 ID 必须注册到图片表；两种资源分别按命令类型查找。文字 shaping 产生的 glyph ID 必须来自该命令引用的同一字体文件。资源应在回放完成前保持有效。当前字体注册后不卸载，图片注册后可由同一 ID 更新。
- CPU 和 GLES 都通过同一个 Skia 回放器解释命令，差别仅在目标画布和 GPU 提交。结构验证由共享回放器完成；像素差异仍受光栅化、采样和颜色格式影响。

当前 UI 字体整形由 `runtime::TextShaper` 持有 FreeType/HarfBuzz 状态；回放字体由 `RasterRenderer::RegisterFont` 注册 SkTypeface。两侧默认字体在构造时从同一配置路径注册，`TextShaper::Shape(font, text, size)` 与 `DrawGlyphRun.font` 使用相同本地 ID。图片通过 `ImageResources` 异步解码后注册到回放器，Scene 收到 `ImageReady` 才发出 `DrawImage`；`FramePacket` 另带该 DisplayList 实际引用图片的 UI 侧版本，提交前与已登记/上传版本核对。UI 与渲染所有者使用同一次 ConfigureWindow 固定的字体路径和默认字体 ID；运行期不支持换字体。后续开放可变字体时必须在帧/资源协议中加入字体身份与版本，不能只靠整数 ID。

## 本地视觉命令（2026-09-26）

`StrokeRoundedRect` 在几何范围内绘制描边；`RoundedRectShadow` 接收圆角、Gaussian sigma、Y 偏移、直 alpha 颜色和 `inset`。外阴影先于节点 clip 回放，不改变布局槽位或输入范围；内阴影在该圆角轮廓内回放。阴影参数为静态主题属性，本阶段无动画。

`DrawIcon` 引用 SDK 的 `VectorIcon` 枚举资源，使用统一的 24 单位 code-native 路径图形，始终按等比尺寸居中。它不依赖 emoji 或图标字体，也不携带应用名。支持 grid/music/settings/folder/terminal/play/pause/previous/next/volume/wifi/battery/search/sun/moon/power/check/chevron/refresh/cpu/memory/heart。

跨 surface 背景模糊不属于 DisplayList。Scene 将 `backdropBlur` 节点的已布局区域转换为 `SurfaceEffectRegion`；平台随下一次 `wl_surface.commit` 提交，由 compositor 采样实际下层。扩展不可用时仍按透明 tint 正常呈现，客户端不模拟背景模糊。

## 装饰子树呈现（2026-09-29）

`VisualPresentation` 保存客户端本地装饰子树的平移、正比例缩放、归一化原点和 opacity。
布局后的绝对坐标先绕 `bounds.origin + bounds.size * origin` 缩放，再平移；嵌套节点的
矩阵按父级到子级组合。`Visual` 的呈现值不修改布局槽位或固定 `InteractionTarget` 的
输入几何。通用交互节点运动、窗口轮廓和跨 surface 背景效果仍有独立的同步要求，详见
[通用交互与呈现规范](INTERACTION_AND_PRESENTATION_SPEC.md)。

DisplayListBuilder 对每个 `Visual` 始终发出 Transform 和 Opacity 作用域，包含节点外
阴影、内容、子节点、内阴影及边框；identity 或 opacity=1 时也保留命令，保证动画采样
只改变数值，不因默认值切换反复改变列表结构。
`Visual` 的背景命令在 alpha=0 时也保留；损伤和 layer 边界分析将全透明颜色视为空 ink，
因此透明占位命令不会把整个装饰容器误算为可见覆盖。

`PushOpacity.opacity` 必须为有限的 `[0,1]` 数值。组内各图元先按自己的颜色和 alpha
合成，整个结果再按组透明度混合。例如两个不透明且重叠的子节点在 opacity=0.5 下，
重叠部分最终 alpha 为约 0.5；逐图元乘 alpha 会得到约 0.75，不符合该契约。嵌套组
依次完成各自合成，不可将父子透明度提前乘到每个 draw。

CPU 与 GLES 共享 Skia `saveLayerAlphaf` 回放。后端从相同的字体、图标和阴影 ink
计算子树局部边界作为 layer 分配提示，边界不依赖本次 repair clip；未知边界交回 Skia
保守处理。opacity=1 使用普通 save，opacity=0 使用空 clip，两端不需要新建中间层。
这不是跨帧离屏缓存；部分透明子树仍有本帧离屏合成成本。

局部损伤同时遍历旧、新矩阵和裁剪栈。正比例二维缩放和平移使用变换后的真实 ink，
在设备坐标增加 AA 保护后取旧、新覆盖的联合；组透明度改变覆盖该组所有绘制内容。
完全透明的组不贡献像素损伤，恢复透明度时按当前变换重新计算可见范围。任意旋转、
错切、镜像或奇异矩阵继续完整修复；命令结构、显式 clip 和资源版本变化也保守回退。
硬矩形 clip 在保持轴对齐矩形的变换下映射到设备坐标后向外取整，回放和损伤分析采用
相同覆盖语义。详见[渲染调度与损伤规范](RENDER_SCHEDULING_AND_INVALIDATION.md)。
