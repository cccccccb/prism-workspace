# 通用轮廓：DSL、Scene 与 surface 效果契约

日期：2026-10-06。对应界面系统第四阶段 4e / 4f / 4g / 4h。

## 当前实施范围

4e 建立一个可以由不同层共同消费的有界轮廓，包含纯契约几何、二进制 payload
编解码，以及 DisplayList 的 `FillContour`、`StrokeContour`、`ContourShadow` 和
`PushClipContour`。Skia 适配器使用同一个轮廓完成填充、描边、阴影和裁剪。

4f 接入 surface effect、Wayland v2 请求与 WM 通用覆盖遮罩。4g 继续接入静态 DSL
几何声明、PreparedComponent/Blueprint、Scene、RenderTree、输入快照以及 surface
区域收集；显式轮廓的绘制、裁剪、命中和 backdrop 输出使用同一份规范点数据。

普通节点没有 `Contour` 时继续使用[矩形与圆角契约](ROUNDED_REGION_CONTRACT.md)。
4h 增加 Popup/Menu 的参数化连接颈配方，按最终定位准备规范轮廓，参数可引用主题。
4h完成时尚未接入Host跨surface定位，也尚未替换v22桌面；4k3已提供能力选择与
原生浮层定位，见[浮层计划契约](POPUP_SURFACE_PLAN_CONTRACT.md)。路径绑定、路径
变形动画及同surface正文模糊仍未接入。后续4k3及连接颈修订的部署记录见
[界面系统计划](INTERFACE_SYSTEM_PLAN.md)第30—31节。

4g 的 WM/Host 构建与相关回归 16/16 通过，包含真实 V3D 轮廓回放和八种主题的
文档配方验证。本节保留对应阶段的验证范围，后续部署与视觉验收单独记录。

## 一份几何，多种消费者

`prism/contracts/contour.hpp` 中的 `Contour` 只持有 `vector<LogicalPoint> points`。
纯契约不得依赖 Skia、Wayland、WM、DSL、业务模块或特定应用。客户端将高级路径准备为
规范轮廓后，绘制、命中、输入区域和 backdrop 应消费同一份点数据。

轮廓没有 Popup、声音面板、锚点或主题身份。这些语义留在前端；WM 只负责验证
轮廓和执行跨 surface 效果。连接颈是上述几何能力的一种使用方式，不能成为 WM 的
特殊控件或绘制分支。

### 规范轮廓

- 坐标是逻辑单位，所有点必须位于 `1/256` 网格上；提交的数据不能包含 NaN 或无穷。
- 至少三个点，最多 256 个点。每个坐标绝对值不超过 8192，外包宽、高不超过 8192。
- 一个闭合简单多边形，闭合边由末点到首点隐式形成；点数组不重复存储首点。
- 不允许孔洞、多个独立轮廓、自交、自接触、零长度边或退化面积。非相邻边不能相交
  或相切；相邻边只在共同端点相接，不能折返重叠。
- 顺时针和逆时针均接受；填充规则固定为 even-odd。首点与点序属于数据本身，当前不
  要求旋转首点或统一绕向，不能把点数组的精确相等误当成任意几何等价判定。
- 非法轮廓明确失败，不自动替换成矩形、不忽略坏边，也不以裁剪掩盖拓扑错误。

上述界限约束输入存储与几何处理。面积很大或曲线特别复杂时仍可能达到资源上限，
不能把顶点上限理解为允许无界显存、扫描或运算。

### 高级路径准备

`ContourPath` 包含起点 `start` 和线段序列；每段是 `ContourLine { end }` 或
`ContourCubic { control1, control2, end }`。闭合仍为隐式闭合；没有第二个子路径或洞。

`PrepareContour` 在客户端准备阶段执行以下工作：

1. 校验路径数值和资源预算。
2. 将三次 Bézier 曲线展平为直线段，量化前的逻辑误差不超过 0.25；递归最多 10 层。
3. 将准备结果量化到 `1/256` 网格，并验证最终点数、范围和简单多边形拓扑。
4. 无法在误差、深度或点数预算内生成有效轮廓时，拒绝准备。

量化最多再引入每轴 `1/512`、平面距离 `sqrt(2)/512` 的位移。不能将展平误差单独
宣称为最终输出的总误差，也不能保证任意缩放后的设备像素误差始终小于 0.25。
量化可能让极窄结构坍缩、让边相触；最终验证必须在量化之后执行。
输入最多 256 个段，起点、控制点和终点同样要求有限且绝对值不超过 8192。
展平以控制点到有限弦段的距离为条件，不能只测到无限延长线的距离而丢失曲线折返。
连续点量化后重合时合并，末点等于首点时移除该显式闭合点；其他自接触仍拒绝。

WM 不重复解释曲线或采用另一种展平算法。绘制、输入与模糊均使用准备完成的
多边形，以免不同层在连接颈边缘产生不同轮廓。准备结果适合缓存和不可变快照，
不在渲染回放过程中修改点数组。

## 几何判定与输入目标

包含判断与扫描跨度使用同一简单多边形和 even-odd 规则。凹多边形在一条扫描线上
可能产生多个不相连区间，输入区域不能只取最左到最右的一个跨度。

逻辑像素输入掩码沿用圆角契约：整数像素 `(x,y)` 由中心 `(x+0.5,y+0.5)` 判定；
相同跨度的相邻行可以合并。几何边界约定必须明确并在实时命中、快照命中和栅格化中
共用，避免扫描恰好经过顶点时双重计数或漏边。

几何接口为 `ContourBounds`、`ContourContains` 和 `RasterizeContourIntersection`。
点查询要求调用方先安装经过验证的轮廓，不在每次鼠标取值时重复拓扑验证。
`ContourContains` 默认包含边界，可显式传 `include_edges=false` 排除边界；非有限
查询点始终返回 false。栅格交集的轮廓与圆角形状**合计最多八个**，采用包含边界的
像素中心判定，另与半开矩形 clip 相交；clip 的有限坐标、宽高同样受 8192 上限约束。
轮廓和圆角形状列表均为空时表示无掩码；传入圆角形状时按同一像素中心规则参与交集。
每次栅格化最多生成 65,536 个合并矩形，整个 Scene 收集的 surface input 区域也最多
65,536 个矩形。超限明确拒绝，
不提交部分结果。八形状限制针对一次交集，包含父级 clip，并非应用最多八个交互节点；
相同形状在交集准备中去重，不能仅按外包矩形认为两个轮廓相同。

Skia 抗锯齿输出是覆盖率，输入 region 是二值掩码；两者不要求所有半透明边缘像素
完全相同。验证应比较几何中心判定和确定的全覆盖/全透明区域，并单独记录抗锯齿容差。
外阴影不属于命中或 backdrop 输出区域。Popup 外点关闭屏障也不属于菜单命令轮廓。
实时命中与 `InputSnapshot` 均通过 `SceneShapeContains` 消费规范轮廓；Visual 子树
继续只负责呈现，不创建输入节点。节点的 `clip`、`overflow:"clip"` 和 Popup/Menu
强制裁剪使用轮廓限制后代，外阴影仍不扩大输入区域。
带轮廓的节点即使保留 `inputShape:"bounds"` 样式，也不会把规范形状扩张为外包矩形。

## DSL 静态几何声明

`Contour` 是受支持节点内的**类型化几何元声明**，不是 layout 子节点，也不创建
NodeId、binding 或独立绘制对象。语法解析沿用已有的节点 AST，语义准备阶段将路径
转换为 `contracts::Contour`；不接受 SVG/path 字符串，也不把路径塞进普通字符串属性。
`PropertyValue`、`DslProperty` 编号和 64 位属性掩码保持原有契约。

```prism
Card(width: 120, height: 80, background: "@controlSecondary", clip: true) {
    Contour(space: "local") {
        Move(x: 0, y: 0)
        Line(x: 108, y: 0)
        Cubic(c1x: 114.628, c1y: 0, c2x: 120, c2y: 5.372, x: 120, y: 12)
        Line(x: 120, y: 80)
        Line(x: 0, y: 80)
    }
    Text("Panel", font: "@font_body", foreground: "@text")
}
```

### 声明规则与诊断

- `space` 可省略，默认 `"local"`；只接受这个字符串字面值。坐标相对节点布局左上角，
  单位为逻辑像素，不随 width/height 自动拉伸。`"normalized"` 尚未支持并明确拒绝。
- 每个节点最多一个 `Contour`。首个且唯一的 `Move(x:,y:)` 声明起点，后续只允许
  `Line(x:,y:)` 与 `Cubic(c1x:,c1y:,c2x:,c2y:,x:,y:)`，最多 256 个后续段。
- 参数必须具名，坐标必须是有限数值字面值，绝对值不超过 8192。binding、主题引用、
  字符串数值、颜色、bool、identifier、列表、重复/未知/缺少参数均拒绝。所有段禁止
  子节点和 modifier；`Contour` 自身也禁止 modifier。闭合由末点到首点隐式完成，
  不提供第二个 Move、Close、多个子路径或孔洞声明。
- `PrepareContour` 在语义准备阶段执行一次，完成曲线展平、量化与拓扑验证。非法
  路径抛出 `LoadFailure`，阶段为 `Semantic`，保留 component/source/version；字段
  错误指向参数或命令所在行，整体拓扑错误指向 Contour 所在行。
- 支持 `Box/Row/Column/Visual/InteractionTarget/Popup/Menu/MenuItem/MenuBack`。
  对应常用 DSL 名称包括 Card、HStack、VStack；Button 映射为 Box，允许几何元声明，
  原有隐式 Text 标签仍由前端创建。Text、Image、Icon、IconButton、编辑器、Slider、
  ScrollView 和其他专门控件首版不支持直接声明轮廓。
- 带 `sliderPart` 或 `scrollPart` 的自动布局 Visual 部件也拒绝轮廓；它们的几何由
  控件生成流程决定。`material:"window"` 与显式轮廓组合在 DSL 和 Scene 安装边界
  都拒绝，保留 compositor 的标准外框规则。
- `.contour(...)`、路径 binding、state 中的 contour 和 contour transition 均不支持。
  Visual 已有的颜色、透明度和呈现变换能力仍可用于带轮廓的装饰；它们不会把静态
  路径变成可插值的几何属性，也不会使 Visual 获得输入或 backdrop 能力。

普通 `cornerRadius`、数字主题引用和绑定可以保留在样式数据里；显式 Contour 覆盖
该节点的完整形状，填充、描边、阴影、裁剪和命中不会再叠加 uniform cornerRadius。
材质的 tint、blur、border、shadow 仍按各自接口提供，局部轮廓不会重写材质的定义。
静态 Contour 的主题切换不会自动替换路径；另一条静态路径需要另一份 UI/组件描述。
4h 参数化配方另由最终定位和解析后的 Number theme token 生成，不改变静态路径规则。

### 静态连接颈配方

下面的完整视图保留标准 window 根装饰，在其内部声明一个 160×104 的面板。
正文上缘从 y=16 开始，顶部中央的连接颈与正文构成**一个**闭合轮廓；不会通过两个
重叠的背景区域拼接。内容留在正文范围，图标与可读文字组合，材质颜色使用现有主题
token。调用方需要像其他主题 UI 一样为 Scene 提供当前 ThemeSnapshot。

```prism
VStack(width: 240, height: 224, padding: 24, spacing: 16, material: "window") {
    Text("Sound", height: 24, font: "@font_heading", foreground: "@text")

    Card(width: 160, height: 104, clip: true,
         background: "@controlSecondary", backdropBlur: 12,
         borderWidth: 1, borderColor: "@controlOutline",
         shadowBlur: 12, shadowY: 4, shadowColor: #00000040,
         innerShadowBlur: 1, innerShadowY: 1, innerShadowColor: #FFFFFF1A) {
        Contour(space: "local") {
            Move(x: 76, y: 0)
            Line(x: 84, y: 0)
            Cubic(c1x: 86.2, c1y: 0, c2x: 88, c2y: 1.8, x: 88, y: 4)
            Line(x: 88, y: 8)
            Cubic(c1x: 88, c1y: 12, c2x: 92, c2y: 16, x: 96, y: 16)
            Line(x: 148, y: 16)
            Cubic(c1x: 154.628, c1y: 16, c2x: 160, c2y: 21.372, x: 160, y: 28)
            Line(x: 160, y: 92)
            Cubic(c1x: 160, c1y: 98.628, c2x: 154.628, c2y: 104, x: 148, y: 104)
            Line(x: 12, y: 104)
            Cubic(c1x: 5.372, c1y: 104, c2x: 0, c2y: 98.628, x: 0, y: 92)
            Line(x: 0, y: 28)
            Cubic(c1x: 0, c1y: 21.372, c2x: 5.372, c2y: 16, x: 12, y: 16)
            Line(x: 64, y: 16)
            Cubic(c1x: 68, c1y: 16, c2x: 72, c2y: 12, x: 72, y: 8)
            Line(x: 72, y: 4)
            Cubic(c1x: 72, c1y: 1.8, c2x: 73.8, c2y: 0, x: 76, y: 0)
        }

        VStack(paddingX: 16, paddingY: 24, spacing: 8) {
            HStack(height: 20, spacing: 8, align: "center") {
                Icon("volume", width: 16, height: 16, foreground: "@mutedText")
                Text("Output volume", font: "@font_body", foreground: "@text")
            }
            Text("62%", font: "@font_heading", foreground: "@text")
        }
    }
}
```

这是静态几何示例。连接颈的位置不会自动追踪某个锚点，面板大小改变也不会缩放路径。
后续 Popup 配方仍须遵守 Popup/Menu 的 root、popupFor、输入 token 与关闭规则，并在
客户端决定连接颈的方向和位置。本例的 backdrop 采样边界仍是整个所属 surface 下方
的场景，不能据此声称它会模糊本窗口其他正文。

## Scene 消费、缓存与事务

`PreparedNode::contour` 与 `Blueprint::contour` 是 owning 的可选值，准备、链接与
critical 合成都保留它，PreparedComponent 的保留内存统计包含顶点容量。几何元声明
从可视 children 中移除；LayoutCompiler 收集 Slot 路径时同样跳过它，因此 Contour
出现在 Slot 前后都不会改变挂载目标。

Scene 安装再次验证轮廓和适用节点，把源描述持有为 `shared_ptr<const Contour>`。
布局后在 detached SceneSnapshot 上准备 surface-local 轮廓：将**整个布局原点**
统一 round 到 `1/256` 网格，再平移已量化的所有顶点，不逐点选择另一套舍入或再次
展平。原点的每轴舍入位移最多 `1/512`；普通内容布局值不因该操作被整体改写。
放置后的所有点仍受 canonical 坐标、尺寸和拓扑限制，超限不能安装部分结果。

轮廓不提供 intrinsic width/height，也不参与布局测量；节点应声明合适的尺寸与正文
留白。局部点可描述凹形和连接颈，真正的可见范围由父级 clip 与 viewport 决定。
空尺寸或不可见节点不发布轮廓。相同源点与位置复用已放置的不可变轮廓；修改布局或
滚动时先准备候选几何，成功后再更新消费者，避免渲染回放修改点数组。
局部路径可延伸到节点 bounds 外；父级 clip 和 viewport 仍限制绘制与输入，命中返回的
localPosition 仍相对未量化的布局原点，因而可能为负值。Contour 不强制改变 clip 政策。

RenderTree 保存同一份不可变 surface-local 轮廓；DisplayListBuilder 按类型提交填充、
内侧描边、内外阴影与 clip。实时命中和 InputSnapshot 保存相同几何；旧输入快照保持
自己的轮廓，不借用 live 节点的可变数组。SurfaceEffects 收集相同轮廓并置 radius=0，
由 Wayland v2 将完整 canonical 点提交给 WM。

ApplyTheme、绑定投影、Preflight、区域挂载和渐进构造继续走现有 candidate/commit
流程。CurrentBlueprint、结构比较、候选校验保留并检查静态源描述；值投影不能把
保留的 contour 换成另一条路径。通过 UI/区域安装引入新描述仍须完成该安装事务。
几何数值、作用域、区域预算或最终绘制列表失败时应保留上一份可用输出；本步不增加
固定 timer、每帧曲线解析或同步读回。

`ScrollTo` 使用只读前瞻放置上下文，先平移规范轮廓并检查完整 effect/input 区域；
不临时改 live 节点，也不重新 Layout 或展平曲线。预检包括部分 blur 裁剪、最终坐标
及输入矩形预算；失败返回 false，保留 offset、几何、快照、Popup、捕获与原缓存。
成功后提交已经准备好的输入掩码，随后产生的输入政策失效仍有效。锚点滚动导致浮层
关闭时，预检同时使用关闭后的输入政策；空尺寸节点不会在无 Layout 滚动中生成轮廓。

### 输入交集与 backdrop 交集分别验证

二值 surface input 可精确栅格化轮廓与圆角祖先 clip 的交集，包括凹形的多个跨度；
整个结果准备成功后才替换输入区域缓存。外点关闭屏障保留 Popup 的 owner 范围和
会话语义，不把连接颈视为新窗口或单独命令。

backdrop 请求只能携带一个完整已有轮廓，不能用输入 mask 的整数矩形替代其 AA
几何。相同形状或能够证明完整保留某一形状的包含关系可继续提交；完全无交集时
省略。**局部切掉轮廓、不能证明的凹形包含关系或可能产生多块的交集明确拒绝**，
不降级为外包矩形，也不提交一半的 blur 列表。包含性检查可以保守拒绝有效但当前
无法证明的情况；这不表示 WM 已具备任意布尔路径运算。UI 应调整 clip、留白或
明确取消该位置的 blur 后重新准备，不在框架内悄悄扩大效果。

## DisplayList 与 Skia

四个命令都携带规范 `Contour`；它们是进程内有类型 DrawList，不能把 C++ 对象直接
序列化到 IPC。`PushClipContour` 与 `PopClip` 使用既有作用域配对，遵守相同深度限制。

- `FillContour`：以指定颜色填充完整闭合轮廓。
- `StrokeContour`：宽度从边界向内计算；使用两倍宽度的居中圆连接描边，并以同一
  轮廓裁剪。宽度为零不绘制，边界抗锯齿仍保留覆盖率。
- `ContourShadow`：阴影依据完整轮廓生成，使用与现有阴影一致的颜色、偏移、Gaussian
  sigma 与内/外阴影概念；内阴影限制于轮廓内部，外阴影不扩大输入区域。外阴影作为
  偏移轮廓的模糊结果先行绘制，其内部也可有颜色，随后由面板填充覆盖或混合。
- `PushClipContour`：裁剪后续绘制，不扩大为外包矩形。

DisplayList 验证检查每个轮廓的网格、边界、拓扑与预算，并保留颜色、描边、阴影和
clip/transform/opacity 作用域的已有数值门槛。未知或无法验证的几何不能静默通过。

Skia 适配器将已准备的顶点连接为闭合 even-odd Path，无应用专属绘制逻辑。CPU raster
与 GLES 使用同一回放语义。填充、描边、阴影、裁剪的几何边界相同；需要为阴影和
抗锯齿保留适当的设备空间损伤范围。

轮廓命令同时接入 ink bounds、损伤比较和 opacity layer 范围计算。轮廓裁剪变化可以
采用保守整帧修复；无法证明较小范围时不得漏修复。非均匀缩放的模糊阴影不能只按
缩放后的几何外包估计，需遵守现有阴影的设备空间安全规则。

## 独立 payload 格式 v1

`EncodeContour` / `DecodeContour` 是纯契约编解码。payload 格式版本与 Wayland
interface 版本分别管理；payload v1 不表示 Wayland interface 的 v1 绑定具备轮廓请求。

所有多字节值为 **little-endian**，不得直接 memcpy 带 padding 的 C/C++ 结构体，也不
依赖宿主字节序。头部固定 8 字节：

| 字节偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | uint16 | version，固定为 1 |
| 2 | uint16 | count，范围 3..256 |
| 4 | uint16 | flags，固定为 0 |
| 6 | uint16 | reserved，固定为 0 |
| 8 起 | count 对 int32 | x、y，每对 8 字节，signed Q24.8 |

`Q24.8` 值乘以 `1/256` 后得到逻辑坐标。负数按有符号 32 位表示传输。总长度必须
严格等于 `8 + count * 8`，范围为 32..2056 字节；没有尾部扩展、隐藏子路径或附加对象。

编码前和解码后均验证规范轮廓。未知版本、非零 flags/reserved、截断、多余字节、
非法 count、超范围坐标或错误拓扑都拒绝。解码先验证长度与 count，再分配和读取，
不能根据恶意头部先做无界分配。调用方不得先相信外包矩形再跳过顶点验证。

```cpp
// 纯契约编解码；实际 surface 提交由 Wayland 平台适配器完成。
Contour panel{{{0, 0}, {120, 0}, {120, 80}, {0, 80}}};
auto payload = EncodeContour(panel);
auto restored = DecodeContour(payload);
```

## Wayland / WM v2 接入

现有 `prism_surface_effect_manager_v1` / `prism_surface_effect_v1` interface 已
提升到版本 2，保持版本 1 请求语义，新增带 payload 的轮廓请求和轮廓能力声明。
客户端绑定服务端与客户端共同支持的版本，创建的 effect 对象采用协商后的版本。
旧 `capabilities(backdrop)` 事件保持不变；v2 增加 `contour_capabilities(supported)`
事件和 `add_contour(blur_radius, payload)` 请求。仅支持 v1 的绑定不会收到新事件。
完整列表先校验、编码并转换为实际 fixed 值，再 clear/add；坏输入不会造成部分发送。
客户端缓存实际发送的列表，能力不足时仅省略轮廓，已有矩形仍可正常提交。

轮廓请求沿用 `wl_surface.commit` 双缓冲：clear、圆角区域和轮廓区域进入 pending，
commit 原子替换 current。单 surface 最多八个 effect 区域，blur 半径 0..48 和现有
资源限制不因添加轮廓而放宽。服务端验证 payload 后持有自身数据，不保留请求数组的
借用指针；销毁、重建和迟到提交遵守现有 surface 生命周期。能力为零时客户端不发
轮廓请求；绕过协商的请求、坏 payload、范围或总量错误使用 `invalid_region` 拒绝。
surface 已销毁后 clear/add 使用 `surface_gone`。C 回调内处理异常，不越过协议边界。

不支持轮廓 blur 的 compositor 保留客户端透明/着色结果并省略该轮廓的 backdrop。
不能将非矩形轮廓降级为外包矩形，也不能拆成相交的独立模糊区域：后者会重复叠加
覆盖并在描边、阴影和连接处留下接缝。高级交集暂时无法精确表示时应拒绝或采用明确
可见的材质降级；不能偷偷扩大效果。

### 背景采样边界

现有 WM 捕获仅读取 **所属 surface 下方的场景**，遍历遇到该客户端便停止，且排除
它自身的效果节点。通用轮廓只改变效果的输出遮罩，不改变这条跨 surface 采样边界。

因此，同 surface 的 Popup 对自己正文的局部模糊需要后续客户端合成能力或独立且
可信的 surface 策略。本步不能声称仅添加轮廓协议就已实现该能力；也不能把同 surface
正文纳入 WM 捕获从而产生自采样或反馈环。

采样 footprint 可以包含轮廓外的背景，以提供模糊所需像素；最终 backdrop 输出必须
由完整轮廓覆盖遮罩限制。外阴影、装饰描边和请求 blur 的区域各自保持已有责任。

### 缓存、变形与分数坐标

- 缓存键包括顶点、坐标空间、尺寸、缩放、模糊与材质参数；只比较外包矩形不足以
  识别连接颈位置或形状变化。哈希逐字段计算，避免结构体 padding。
- 呈现变形必须变换所有顶点，并同步更新绘制、命中、输入快照及 blur 遮罩。不能
  只改轮廓的 bounds，也不能给不同消费者独立选择量化或展平结果。
- 分数原点必须保留。若效果缓冲锚定在 floor 后的整像素坐标，轮廓在缓冲中的位置
  应扣除实际缓冲原点，不能始终假定左上角就是整数 padding。
- 几何或映射变化时才重建覆盖遮罩；背景像素变化只更新相关采样与材质结果。复用
  现有损伤依赖与不可变提交机制，不增加固定 timer、忙循环或每帧同步读回。
- WM 的外层窗口装饰仍有自身主题规则。局部轮廓不能仅因其 bounds 等于窗口就被
  误判为窗口装饰；frame 判断明确排除通用轮廓请求。

4f 的遮罩由 WM 的通用扫描线实现生成：四个纵向子采样和水平区间面积覆盖，输出
单通道 alpha 后上传 GPU。它是有界 AA 近似，不保证与 Skia 边缘覆盖逐像素一致。
每个遮罩最多 16 MiB、宽高最多 8192，另受实际 `GL_MAX_TEXTURE_SIZE` 限制；预算
校验在大缓冲分配与背景捕获之前执行。失败关闭当前效果节点，保留客户端着色，
不发布旧遮罩或矩形替代品。

遮罩缓存键使用映射后的相对顶点、目标尺寸和实际缓冲内原点，逐字段比较。
背景或 blur 数值改变只重做所需材质，几何不变时复用遮罩上传；全材质缓存另外包含
完整顶点、分数原点和下层依赖。呈现映射变换每个顶点，不再次展平或量化。
缓冲原点向下取整、右/下边缘向上取整；几何相对原点保留小数部分。一半分辨率
捕获的奇数尺寸按实际采样比例回写，逻辑上下方向与纹理上传方向明确转换。

`WorkCounters` 与 typed WM status 的 `effects_work` 增加 `mask_builds`、
`mask_cache_hits`、`mask_failures`，分别记录成功上传、复用检查和失败。静止全材质
缓存命中无需再检查遮罩；这些计数不是 GPU 完成时间、显存或帧率。

## 4h：按最终定位准备面板轮廓

`Contour(recipe: "attachedPanel", radius: "@panel_radius", neckWidth: "@space_section",
neckHeight: "@space_sm", fallback: "detached")` 是 Popup/Menu 的 typed 几何配方。
三项数值和 fallback 必填；数值接受有限字面量或现有 Number theme token，分别限定
radius/neckWidth 为 0..256、neckHeight 为 0..48。仅支持明确的 detached 回退；拒绝
未知字段、children、modifiers、space、binding/state/transition 和颜色 token。
一个节点只允许一份静态 Contour 或一份配方，不增加普通 DslProperty 或协议字段。

width/height 仍表示正文尺寸，padding 只应用一次。布局保存最终 PopupPlacement：
正文 bounds、Below/Above/EdgePanel、有效锚点和受限尺寸。有效锚点先裁到显式祖先
clip 的 bounds，再裁到 available；最终选定点还须通过实际轮廓校验。配方面板水平中心对齐，
随后遵循原有安全边距和上下回退；无配方的调用默认保持 Start 对齐。连接颈伸出正文
上缘或下缘，与正文组成一个闭合轮廓。正常放置的 gap 为原来的 8 加有效 neckHeight，
因此颈尖与锚点仍相隔 8 逻辑像素，这是指向关系，不是与锚点贴合的联合路径。

neckWidth 表示完整肩部占用宽度。中心必须落在正文圆角之间的安全范围及裁剪后锚点
的水平投影交集中；选定的锚点中心还需落在锚点自身及祖先实际裁剪形状内。EdgePanel、
零颈部、过窄或没有安全交集时使用声明的 detached 正文轮廓；选定中心被实际裁剪
形状排除时也保守回退。本步不搜索复杂凹形锚点的全部其他可见中心。保留正文位置
和间距，不反复定位或偷偷用 bounds 代替曲线。非法参数、量化退化或预算溢出报错，
不能当作 detached 理由。Square 的 radius=0 使正文和颈部使用直线；具体颈部形状由
下面的 neckShape 决定。

所有者线程解析配方参数，在布局后的 detached snapshot 中生成并验证规范点，之后才
发布到实时几何。颜色不进入几何缓存键；相同尺寸、方向、中心和参数复用局部准备
结果及已放置的共享点。Above 在控制点阶段镜像，再统一展平与量化；不会镜像已经
量化的点。旧提交快照继续持有旧几何。主题候选独立解析配方，参数变化触发布局与
绘制；关闭/隐藏的面板更新参数不单独触发像素或布局，重新展开时准备新形状。失败
保留原主题、点数据、输入和 Popup 会话。区域事务保护配方的数值/引用
provenance，不能伪造保留节点的描述。

渲染、clip、输入、InputRegion、SurfaceEffect 继续共用 4g 的规范轮廓。WM 不认识
配方、锚点或 Popup 类型。主题复用现有 Number token，不改变主题传输或 Wayland
v2。配方不提供路径动画或同surface正文模糊；Host原生浮层定位由后续4k3接管。

### 圆润三角颈（v25）

用户在 v24 实机检查后调整图 02：不保留颈部平顶，改为圆润三角指向锚点。
`attachedPanel` 新增可选字符串字面量 `neckShape`：`"softTab"` 保持旧平顶轮廓，
省略时也是该值；`"roundedTriangle"` 使用宽肩、连续收尖曲线及圆润的唯一顶点，不含
顶端水平线段。未知值、数值、绑定和 theme 字符串引用均拒绝。形状是声明参数，
宽、高、圆角继续允许 Number theme token；不增加 WM 应用分支或 Wayland 字段。

```prism
Contour(recipe: "attachedPanel", radius: "@panel_radius",
        neckWidth: "@panel_neck_width", neckHeight: "@panel_neck_height",
        neckShape: "roundedTriangle", fallback: "detached")
```

当前四主题提供 `panel_neck_width=64`、`panel_neck_height=20`，可由主题调整比例。
圆润范围受 radius 和颈部尺寸共同限制；radius=0 的三角颈使用直线，正文仍为直角。
上下方向在曲线控制点阶段镜像，再统一展平、量化和预算校验。量化后无法保持唯一
顶点的退化参数拒绝；不能静默变成平顶。关闭颈部和 detached 轮廓不受形状选择影响。

typed shape 参与配方、最终参数、布局缓存及事务 provenance 的值比较。绘制、裁剪、
内外阴影、输入、模糊遮罩共享最终 Contour；WM 消费规范顶点，不解释 `neckShape`。
形状字段自v25 Host支持；C造型需要v26 Host及匹配主题。更旧Host不识别新增DSL字段。

v26 外观按用户选定的[对照稿 C](design/interface-system/neck-study-v1.png)修订：
圆润主题的整段肩部和收尖部分使用连续曲线，顶部为饱满圆弧，不保留 v25 的长直斜边。
每侧肩部与顶端两段 cubic 的接缝切线连续，左右对称；Square 使用直线三角。
接口与旧 softTab 默认保持，v25 的尖锐试验造型不作为已认可视觉基线。

## 分阶段验收

4e 验收覆盖规范轮廓与拓扑拒绝、曲线展平和预算、量化退化、payload 大小/版本/
字节序/严格长度，以及真实 Skia 的填充、描边、阴影、裁剪和损伤修复。测试与视觉
probe 放在 `tests/`，不进入生产应用包。

4f Wayland/WM 验证协商、原子提交、能力降级、分数原点、呈现缩放、缓存失效和真实
GPU 背景采样。4g 的新增门槛包括：

- DSL 的 Move/Line/Cubic 字段类型、位置、有限范围、段数与拓扑拒绝；owning 准备
  结果、内存统计、链接与 critical 合成保留；Contour 位于 Slot 前后及嵌套路径的挂载。
- Scene 绘制、clip、内外阴影、实时/快照命中消费同一几何；凹形多跨度 input 与圆角
  clip 交集；旧快照在布局和滚动后保持可用，外点关闭仍遵守 Popup token 规则。
- 样式与明暗主题切换保留静态形状；区域事务、绑定投影、Preflight 和渐进安装的
  几何校验、结构保护与回滚；不支持的 generated part/window material 明确拒绝。
- 1/256 整体原点舍入、最终 surface 坐标超限、零尺寸与隐藏节点；完整 backdrop
  包含关系、局部轮廓裁剪拒绝，input 的合计八形状与 65,536 矩形预算。
- 真实 Skia 回放与损伤修复、WM/Host 构建，以及文档配方的真实字体整形和八主题
  渲染。Wayland/WM 的版本兼容、降级和采样边界沿用下文 4f 的独立验收记录。

静态示例与独立测试通过不能替代打包、安装和运行画面的确认，也不能据此宣称自动
交互几何动画、同 surface 正文模糊或 VNC 性能已经完成。自动锚点配方另见 4h 验收。

4e 实际记录：Host 构建、相关 CTest 11/11、Broadcom V3D GLES 轮廓回放和同后端
完整/局部修复通过。轮廓库以标准 C++20 独立编译和运行契约测试通过；独立 CMake
配置另遇系统模块文本损坏，尚未修复。完整证据与限制见
[执行计划第 21 节](INTERFACE_SYSTEM_PLAN.md#21-第四阶段-4e通用轮廓基础契约2026-10-05)。

4f 实际记录：WM/Host 构建与相关 CTest 15/15 通过，实际 WaylandWindow 与真实
协议服务端分别覆盖双方的版本兼容、能力、原子提交、缓存和错误边界。真实 V3D
读回验证凹形与连接颈方向、分数覆盖、同 bounds 换形状、非均匀呈现、装饰分离、
正向阴影偏移及遮罩预算失败恢复；合成调度 GPU probe 的 54 个场景通过。
系统 CMake 模块和 JSON 头损坏通过校验过的软件包私有解包隔离，系统文件未修复。
完整记录与 4f 当时保留的 Scene/DSL 接入范围见
[执行计划第 22 节](INTERFACE_SYSTEM_PLAN.md#22-第四阶段-4fwayland-v2-与-wm-通用轮廓遮罩2026-10-06)。

4g 实际记录：WM/Host 构建和相关 CTest **16/16** 通过，覆盖 DSL 准备/链接/Slot、
混合输入精确交集与预算、Scene 实时/快照几何、滚动预检和恢复、Popup 外点消费、
主题/区域事务及旧控件回归。文档中的完整曲线配方直接由测试读取，真实字体整形与
Skia raster 在四材质 × 明暗下均通过；每种生成同一份 56 顶点轮廓和 24 个输入区域，
轮廓填充、描边、内外阴影、clip、输入快照及 contour effect 点数据一致。
真实 GLES gate 仍报告 Broadcom V3D 4.2.14.0，局部修复与同后端完整回放最大通道
差异为 0；该记录验证正确性，不是 FPS 或同 surface Popup 正文模糊结论。
完整记录、首轮兼容诊断失败及修正见
[执行计划第 23 节](INTERFACE_SYSTEM_PLAN.md#23-第四阶段-4gscenedsl-通用轮廓与不可变输入2026-10-06)。

4h 实际记录：WM/Host 构建与相关 CTest **21/21** 通过，包括纯配方、DSL typed
准备/链接/critical 合成、最终定位、主题/区域事务、输入共享/旧快照和原有 GLES 门槛。
声音面板的真实字体与 Skia raster 覆盖四材质 × 明暗 × 四种定位，共 **32 场景**；
圆角附着/detached 50/36 点，Square 8/4 点。首轮测试的 MenuItem fixture 语法错误
已修复并保留失败日志。规范与图像输出详见
[执行计划第 24 节](INTERFACE_SYSTEM_PLAN.md#24-第四阶段-4h最终定位驱动的主题面板配方2026-10-06)。
该验收不表示新增界面已部署，也不验证同 surface 正文背景模糊。
