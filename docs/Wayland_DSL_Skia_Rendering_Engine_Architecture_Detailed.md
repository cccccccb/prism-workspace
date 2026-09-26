# Wayland DSL 客户端渲染引擎总体架构设计

> **定位**：Linux / Wayland 原生客户端渲染引擎\
> **建议技术栈**：C++20/23 + Wayland + Vulkan + Skia GPU + 自研 DSL
> Runtime\
> **文档目的**：确定长期架构方向，而不是定义具体 DSL 语法。\
> **核心原则**：DSL/Scene/Layout/Animation 是你的引擎；Skia
> 是图形后端；Vulkan/Wayland 是平台与呈现层。

## 本项目实施补充（2026-09-26）

以下是 Prism 在 Raspberry Pi 上落地本文时必须遵守的边界；下文的总体架构目标不因当前实现仍处于迁移期而降低。实施检查点与已测事实见 [SKIA_MIGRATION_PLAN.md](SKIA_MIGRATION_PLAN.md)。

1. **WM 与应用界面分进程。** WM 只管理 surface、输出、焦点、布局、装饰和获授权的 shell 角色；应用进程持有 DSL AST、Scene、绑定、资源和 Skia 上下文。不能把 topbar/dock/desktop 的业务绘制或应用 `$slot` 留在 WM。普通 xdg_toplevel 不能冒充 shell 角色；特权角色不能只凭 `app_id`/标题或当前无鉴权的文本 IPC 授予。
2. **Skia 是绘图后端，不是控件系统。** AST、Scene、Render Tree、DisplayList 分别承担语法、持久状态、视觉结构和绘制命令；核心头文件不暴露 `Sk*`、`Vk*`、`wl_*`。当前已拆出独立布局、Render Tree 和 DisplayListBuilder；节点增量布局与 chunk cache 属于后续性能阶段。命令和资源的当前约束见 [DISPLAY_LIST_CONTRACT.md](DISPLAY_LIST_CONTRACT.md)。
3. **GPU 渲染和 Wayland 呈现必须同时成立。** CPU Skia → `wl_shm` 已通过 headless 测试，只能证明功能；Ganesh GLES → EGL Wayland WSI 现已在 headless Pi 上以客户端 V3D 完成图片、文字和帧提交，实测范围见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md)。Vulkan/GLES 后端需要实际 GPU buffer、同步、释放和 resize 生命周期；不能以 GPU 绘制后读回 CPU 再提交 `wl_shm` 的结果宣称 GPU 呈现性能。Wayland 并不要求所有客户端直接管理 Vulkan swapchain；可以采用经验证的 WSI 或 dma-buf 路径。Pi 上必须排除 llvmpipe，以物理显示上的 V3D 帧时决定发布后端。此前无头 Vulkan swapchain 创建失败，原因尚未确认，不能据此否定实际会话中的 GPU 路径。
4. **尺寸与资源状态显式管理。** logical size、buffer 像素尺寸和 scale 分开；configure/ack、buffer release、frame callback 及 GPU 同步不能由一次 `draw` 调用隐式处理。Scene 只持有资源句柄；解码线程只交付完成事件，由运行时线程更新 Scene。按资源状态和内存预算处理失败、恢复与逐出；资源尺寸变化才触发必要的布局重算。
5. **性能数字必须来自可复现测量。** 首帧、静态唤醒、resize、输入到呈现、文字、图片、blur、内存和温度分阶段测量，记录 p50/p95/p99；CPU 绘制微基准和 headless pixman 结果都不能替代物理显示上的 V3D 测试。
6. **迁移分支与发布边界不同。** `tests/` 放单测、probe 和验证 DSL；生产模块只放可复用代码。开发分支允许旧路径和新诊断链路共存以逐项验收；发布切换时一次删除 ImGui、WM 内应用渲染及旧状态差分绘制，不发布两条生产 UI 路径。

7. **统一生产 host 和会话。** 五个自研应用均采用 DSL/资产包 + 纯业务 C ABI 模块，统一 prism-app-host 接管前端。session supervisor 建立可信 launcher↔WM 控制 FD，launcher 在 WM 登记真实 PID/instance/role 并获确认后绑定 worker；WM 用内核连接凭据、pidfd 和一次性登记授权 Shell。Dock 通过实例订阅启动/激活应用，不派生进程；生产入口不保留五个独立 SDK 主循环。协议、故障清理与物理部署边界见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。
8. **真实窗口几何只有一个来源。** 本轮已将普通 XDG view 生命周期与 managed Window/TreeEngine 对接，BSP 目标几何驱动 configure、可见性和焦点；Shell 不进入平铺树。共享 Shell 保留带和 gap 决定 work area，删除按窗口数量横向均分的真实布局路径。显式 fullscreen、方向焦点/交换和工作区操作由同一真实窗口记录处理；不伪造共享内存客户端或恢复第二套焦点状态。
9. **主题是类型化共享配置。** `resources/themes/prism-glass.json` 生成纯契约 `prism/contracts/theme_tokens.hpp`。WM 引用几何/装饰常量；SDK 解析 `"@tokenName"` 为经 Schema 校验的数值或 RGBA 颜色。五应用以图标为主、文字辅助，使用同一向量图标和控件实现；保留当前壁纸，不新增花哨动画。全局运行时主题切换尚未实现，Settings 外观开关只实际改变自身窗口配色。
10. **毛玻璃由 compositor 采样下层内容。** 本轮已接入 `prism_surface_effects_v1` 的版本化、能力协商、surface-local 逻辑像素区域与 commit 生效契约。SDK 将布局完成的区域/圆角/模糊半径交给 Wayland 平台；WM 只处理效果数据，在 GLES pass 中采样该 surface 下层场景、进行两次可分离模糊并组合圆角材料/装饰。客户端 Skia 仅绘制本地 tint、控件及卡片边框/阴影。缺少扩展能力时保留透明 tint；不读取壁纸制作假背景模糊，不采样自身或更上层 surface。
11. **代码检查点与实机验收分别记录。** BSP、共享 tokens、图标玻璃主题与背景材料已随 0.1.0-5 部署，最终 CTest 26/26、真实 host 双/三/四窗树与截图以及 V3D 背景更新检查通过；用户确认 1024×600 当前单输出“外观正常，以上点击都正常”。现场覆盖播放图标、Preferences 本窗口配色和 Dock 激活，不扩展为全部键盘快捷键或多屏/DPI 验收。Music 仍为演示曲库和模拟播放进度，没有音频解码/输出。Slider、全局主题、交互调整分割比例、动效优化、节点增量布局、分块缓存和零拷贝专项仍为后续工作。记录见 [VISUAL_TILING_REFINEMENT_PLAN.md](VISUAL_TILING_REFINEMENT_PLAN.md) 和 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)。

------------------------------------------------------------------------

## 1. 目标与非目标

### 1.1 目标

这套系统不是简单的"把 DSL 翻译成一串
`SkCanvas::drawXXX()`"。目标应该是一套真正的 retained-mode 客户端 UI/2D
渲染运行时：

-   DSL 只在源码变化、热更新或首次加载时解析。
-   Scene Tree 长期存在并保存运行时状态。
-   属性变化通过依赖和 Dirty 系统传播。
-   Layout、Text Layout、Paint、Composite 分阶段处理。
-   Render Tree 表示最终视觉与合成结构。
-   DisplayList 记录可缓存、可回放的绘制命令。
-   Skia 负责高质量 2D 图形和 GPU backend。
-   Vulkan 负责 Linux/Wayland 下的 GPU device、surface、同步和 present。
-   支持图片、文字、Path、渐变、裁剪、阴影、Blur、Mask、Transform、Opacity、动画。
-   设计时就允许增量布局、DisplayList Cache、Culling、Layer
    Cache、Composite-only animation。
-   Batch 是优化手段之一，但不是第一层架构的中心。

### 1.2 暂时不做的事情

第一版不要试图同时实现：

-   浏览器级 CSS 完整兼容。
-   自己实现 Vulkan Path Tessellation。
-   自己实现完整字体 rasterizer / shaping。
-   自己实现 GPU command batching 替代 Skia。
-   复杂 RenderGraph。
-   多窗口、多 GPU、多后端同时完善。
-   全量 SVG/CSS Filter。
-   极端复杂脚本 VM。

先把一条完整、可测、可优化的链路做正确。

------------------------------------------------------------------------

# 2. 总体架构

``` text
                    DSL Source
                        │
                        ▼
              ┌───────────────────┐
              │   DSL Frontend    │
              │ Lexer / Parser    │
              │ AST / Semantic    │
              └─────────┬─────────┘
                        │
                        ▼
              ┌───────────────────┐
              │ Runtime / Scene   │
              │ Node / Property   │
              │ Binding / Event   │
              │ Component / State │
              └─────────┬─────────┘
                        │
          ┌─────────────┼──────────────┐
          ▼             ▼              ▼
       Style         Layout        Animation
          │             │              │
          └─────────────┼──────────────┘
                        ▼
                  Scene Snapshot
                        │
                        ▼
                 RenderTreeBuilder
                        │
                        ▼
                   Render Tree
                        │
            ┌───────────┼───────────┐
            ▼           ▼           ▼
          Dirty       Culling     Layer Analysis
            │           │           │
            └───────────┼───────────┘
                        ▼
               DisplayListBuilder
                        │
                        ▼
                  DisplayList
                        │
              Cache / Replay / Debug
                        │
                        ▼
                 Render Scheduler
                        │
                        ▼
                  SkiaRenderer
                        │
                        ▼
                 Skia GPU Backend
                        │
                        ▼
                      Vulkan
                        │
                        ▼
                     Wayland
```

这里最重要的边界是：

**AST ≠ Scene Tree ≠ Render Tree ≠ DisplayList。**

这四个对象解决的是四类完全不同的问题，不建议为了"简单"合并。

------------------------------------------------------------------------

# 3. 第三方模块选择

## 3.1 图形：Skia

建议 Skia 负责：

-   Rect / RRect / Circle / Line
-   Path / Stroke / Fill
-   Anti-Aliasing
-   Gradient
-   Image
-   Color Filter
-   Image Filter
-   Blur
-   Shadow 的底层实现
-   Clip
-   Matrix Transform
-   Blend
-   GPU 2D rasterization
-   GPU resource/cache 的大量底层工作

**不要让 Skia 类型进入核心 Scene API。**

错误：

``` cpp
struct SceneNode {
    SkRect rect;
    SkPaint paint;
    sk_sp<SkImage> image;
};
```

正确方向：

``` cpp
struct SceneNode {
    Rect rect;
    ComputedStyle style;
    ImageId image;
};
```

只有 `render_skia/` 中允许做：

``` cpp
Rect       -> SkRect
Paint      -> SkPaint
Path       -> SkPath
Matrix     -> SkMatrix
ImageId    -> sk_sp<SkImage>
```

这样以后增加专用 Vulkan pass、CPU renderer、测试 renderer，都不用推翻
Runtime。

------------------------------------------------------------------------

## 3.2 GPU：Vulkan

Vulkan 层建议只负责：

-   `VkInstance`
-   Physical device 选择
-   `VkDevice`
-   Graphics queue / present queue
-   `VkSurfaceKHR`
-   Swapchain
-   Swapchain recreation
-   acquire/present
-   semaphore/fence
-   与 Skia GPU context 的初始化边界
-   外部 VkImage/视频纹理的同步边界
-   device lost 处理

不要把 Vulkan 类型暴露给：

-   DSL
-   Scene
-   Layout
-   Animation
-   Event
-   普通 RenderNode

### VulkanDevice

``` cpp
class VulkanDevice {
public:
    bool initialize(const VulkanOptions&);
    VkInstance instance() const;
    VkPhysicalDevice physicalDevice() const;
    VkDevice device() const;
    VkQueue graphicsQueue() const;
    uint32_t graphicsQueueFamily() const;

    void waitIdle();
};
```

### VulkanSwapchain

``` cpp
class VulkanSwapchain {
public:
    bool create(VulkanDevice&, VkSurfaceKHR, Extent2D);
    AcquireResult acquire();
    PresentResult present(const PresentInfo&);
    void recreate(Extent2D);
};
```

**注意：** swapchain resize、Wayland configure、surface scale
改变必须形成清晰状态机，不能在任意 draw 调用中临时重建。

------------------------------------------------------------------------

# 4. Wayland Platform 层

建议目录：

``` text
platform/wayland/
├── WaylandConnection
├── WaylandWindow
├── WaylandSeat
├── WaylandPointer
├── WaylandKeyboard
├── WaylandOutput
└── WaylandEventLoop
```

Wayland 层只生成平台无关事件：

``` cpp
struct PointerMoveEvent {
    WindowId window;
    Vec2 position;
    Timestamp time;
};

struct PointerButtonEvent {
    WindowId window;
    MouseButton button;
    ButtonState state;
    Vec2 position;
};
```

上层禁止直接处理 `wl_pointer*`。

需要重点考虑：

-   `xdg-shell`
-   `xdg_surface.configure`
-   `xdg_toplevel.configure`
-   logical size 与 framebuffer size
-   fractional scale
-   output scale 改变
-   seat hotplug
-   pointer/keyboard capability 动态变化
-   surface resize 时 swapchain recreation
-   frame callback / presentation timing

**不要把"窗口大小"简单等同于 Vulkan framebuffer pixel size。**

至少区分：

``` cpp
struct WindowMetrics {
    Size logicalSize;
    Size framebufferSize;
    float scale;
};
```

------------------------------------------------------------------------

# 5. DSL Frontend

DSL 前端建议分：

``` text
dsl/
├── source/
├── lexer/
├── parser/
├── ast/
├── semantic/
├── diagnostics/
└── compiler/
```

## 5.1 SourceManager

不要只把 parser 输入设计成 `std::string`。

``` cpp
using SourceId = uint32_t;

struct SourceLocation {
    SourceId source;
    uint32_t offset;
};

struct SourceRange {
    SourceLocation begin;
    SourceLocation end;
};
```

每个 AST Node 保存 `SourceRange`。

这样以后错误可以准确显示：

``` text
main.ui:38:17
unknown property 'backgroud'
did you mean 'background'?
```

也为热更新、IDE/LSP 做准备。

## 5.2 Parser 输出只表示语法

AST 不保存：

-   `SkPaint`
-   实际像素坐标
-   hover
-   animation 当前进度
-   GPU 资源

AST 是 immutable 或接近 immutable 的编译输入。

## 5.3 Semantic 阶段

负责：

-   名字解析
-   类型检查
-   属性合法性
-   默认值
-   enum 转换
-   单位解析
-   resource reference
-   component reference
-   binding dependency 建立

建议将：

``` text
"20px"
"50%"
"auto"
```

在 Semantic 后变成强类型对象，而不是运行时不断解析字符串。

------------------------------------------------------------------------

# 6. Scene Tree / Runtime

Scene Tree 是系统的核心长期状态。

``` cpp
struct SceneNode {
    NodeId id;
    NodeType type;

    NodeId parent;
    SmallVector<NodeId, 4> children;

    PropertyStore properties;
    ComputedStyle computedStyle;

    LayoutBox layout;

    Transform2D transform;
    float opacity = 1.0f;

    NodeState state;

    DirtyFlags dirty;
    uint64_t generation;
};
```

## 6.1 NodeId 不要直接使用裸指针作为跨系统身份

建议：

``` cpp
struct NodeId {
    uint32_t index;
    uint32_t generation;
};
```

类似 generational handle。

原因：

-   Node 删除后避免 stale pointer。
-   Event、Animation、Binding、Resource callback 都可能延迟引用 Node。
-   热更新会大量替换节点。

## 6.2 节点存储

第一版可以：

``` cpp
std::vector<std::unique_ptr<SceneNode>>
```

长期更建议 arena/slot-map：

``` text
NodeArena
 index -> SceneNode
 generation validation
```

可以降低碎片，并使 NodeId 稳定。

------------------------------------------------------------------------

# 7. Property System

如果 DSL 将来支持 binding/animation，Property System 不应该只是 C++
成员变量。

建议：

``` cpp
using PropertyValue = std::variant<
    bool,
    int64_t,
    double,
    StringId,
    Color,
    Length,
    Vec2,
    Rect,
    ResourceId,
    ...
>;
```

每个 Property 有 metadata：

``` cpp
struct PropertyMetadata {
    PropertyId id;
    ValueType type;

    bool affectsStyle;
    bool affectsLayout;
    bool affectsTextLayout;
    bool affectsPaint;
    bool affectsComposite;
};
```

于是：

``` text
width changed
    ↓
metadata.affectsLayout
    ↓
mark LayoutDirty
```

而不是到处手写：

``` cpp
if (property == "width") ...
```

这是 Dirty 系统能长期维护的关键。

------------------------------------------------------------------------

# 8. Binding / Dependency Graph

如果 DSL 允许：

``` text
width = parent.width * 0.5
text = model.userName
visible = state.connected
```

需要显式依赖图：

``` text
Property A
   │
   ├── depends on B
   └── depends on C
```

不要每帧重新执行所有表达式。

建议：

``` cpp
class DependencyGraph {
public:
    void addDependency(PropertyRef source, PropertyRef target);
    void invalidate(PropertyRef changed);
    void evaluateDirty();
};
```

必须检测循环：

``` text
A.width -> B.width
B.width -> A.width
```

并输出 DSL diagnostic。

------------------------------------------------------------------------

# 9. Style System

Style 需要区分：

``` text
Specified Style
      ↓
Resolved Style
      ↓
Computed Style
```

例如 `%`、inherit、theme variable 等不应该一直保留为字符串。

`ComputedStyle` 最终应接近：

``` cpp
struct ComputedStyle {
    Background background;
    Border border;
    CornerRadius radius;

    Color color;
    FontStyle font;

    LayoutStyle layout;

    Overflow overflow;
    BlendMode blend;
    FilterList filters;
};
```

建议 ComputedStyle 支持共享/Interning。

大量节点可能拥有相同 style：

``` text
1000 list items
```

不必复制大对象 1000 次。

------------------------------------------------------------------------

# 10. Layout Engine

Renderer 不参与 Layout。

建议接口：

``` cpp
class LayoutEngine {
public:
    void layout(Scene& scene,
                NodeId root,
                const LayoutConstraints&);
};
```

节点布局结果：

``` cpp
struct LayoutBox {
    float x;
    float y;
    float width;
    float height;

    float contentX;
    float contentY;
    float contentWidth;
    float contentHeight;

    EdgeInsets margin;
    EdgeInsets padding;
};
```

第一版建议只实现：

-   absolute
-   row
-   column
-   fixed size
-   percentage
-   auto/basic intrinsic sizing

再逐步扩展 Flex。

### 增量 Layout

不要每次从 root 全树 layout。

每个节点维护：

``` cpp
LayoutGeneration layoutGeneration;
bool needsMeasure;
bool needsLayout;
```

变化向上寻找最近 layout boundary。

例如：

``` text
Root
└── FixedSizePanel   <- layout boundary
    └── Text changed
```

如果 Panel 尺寸不依赖 Text，可以停止向 Root 传播。

这是后期性能差异非常大的地方。

------------------------------------------------------------------------

# 11. Text System

文字系统建议单独：

``` text
text/
├── FontManager
├── FontCollection
├── TextStyle
├── TextLayoutEngine
├── TextLayout
├── GlyphRun
└── TextCache
```

推荐优先利用 Skia 的文字能力（例如 SkParagraph/SkShaper
所在体系），而不是自己从 glyph 开始。

输入：

``` cpp
struct TextLayoutRequest {
    String text;
    TextStyle style;

    float maxWidth;
    float maxHeight;

    TextAlign align;
    TextDirection direction;
};
```

输出：

``` cpp
struct TextLayout {
    Size size;
    float firstBaseline;

    std::vector<TextLine> lines;
};
```

缓存 key 至少考虑：

``` text
text hash
font/style
font size
max width
locale/direction
line-height
letter spacing
```

文字内容没变、约束没变时不要重新 layout。

------------------------------------------------------------------------

# 12. ResourceManager

建议资源状态机：

``` text
Unloaded
   ↓
Loading
   ↓
Decoded
   ↓
GPUReady
   ↓
Evicted
```

资源 API：

``` cpp
ImageId requestImage(const ResourceUri&);
FontId requestFont(const FontDescriptor&);
```

Scene 只保存 `ImageId`。

## 12.1 异步资源

图片加载完成：

``` text
IO Thread
 ↓
Decode
 ↓
ResourceReadyEvent(ImageId)
 ↓
Runtime Thread
 ↓
找到依赖节点
 ↓
ResourceDirty
 ↓
如果 intrinsic size 改变
    LayoutDirty
否则
    PaintDirty
```

不要 worker thread 直接修改 Scene。

## 12.2 资源预算

至少统计：

``` text
decoded CPU image bytes
GPU image bytes
font bytes
cached layer bytes
DisplayList cache bytes
```

未来 ResourceManager 需要 budget：

``` cpp
struct ResourceBudget {
    size_t cpuImageBytes;
    size_t gpuBytes;
    size_t layerCacheBytes;
};
```

------------------------------------------------------------------------

# 13. Render Tree

Scene Tree 不应该直接绘制。

RenderTreeBuilder：

``` cpp
RenderTree build(
    const SceneSnapshot&,
    const RenderTree* previous);
```

Render Node：

``` cpp
struct RenderNode {
    RenderNodeId id;

    Rect localBounds;
    Rect worldBounds;

    Matrix3x3 transform;

    RenderContent content;
    EffectState effects;

    SmallVector<RenderNodeId, 4> children;

    RenderGeneration generation;
};
```

`RenderContent`：

``` cpp
variant<
    Empty,
    RectContent,
    RRectContent,
    PathContent,
    ImageContent,
    TextContent
>
```

效果单独存在，不要把 blur/shadow 塞到每种 geometry 中。

------------------------------------------------------------------------

# 14. Layer / Effect Tree

高级效果建议结构化：

``` text
TransformLayer
└── ClipLayer
    └── OpacityLayer
        └── FilterLayer
            └── PaintContent
```

可以逻辑上存在，不一定必须每层分配 C++ heap object。

实现可以扁平化：

``` cpp
struct EffectState {
    Matrix3x3 transform;
    ClipStack clip;
    float opacity;
    FilterList filters;
};
```

但概念上必须区分：

-   Paint property
-   Composite property

## Composite-only 属性

优先将：

-   translate
-   rotate
-   scale
-   opacity

设计为可以不重新生成 subtree 内容。

这是动画性能的关键。

------------------------------------------------------------------------

# 15. DisplayList

DisplayList 建议是 immutable。

``` cpp
class DisplayList {
public:
    Span<const DisplayCommand> commands() const;
    Rect bounds() const;
    uint64_t hash() const;
};
```

不要所有命令都使用大量 polymorphic heap object。

推荐：

``` text
compact command stream
+
side tables/resources
```

例如：

``` cpp
enum class Op : uint8_t {
    Save,
    Restore,
    Transform,
    ClipRect,
    ClipPath,
    DrawRect,
    DrawRRect,
    DrawPath,
    DrawImage,
    DrawText,
    SaveLayer
};
```

可使用：

``` text
[Op][payload][Op][payload]...
```

或者 `std::variant` 先实现，profile 后再压缩。

### DisplayList Chunk

不要整个窗口只有一个 DisplayList cache。

建议：

``` text
Root DisplayList
├── Header Chunk
├── Sidebar Chunk
├── Content Chunk
│   ├── StaticBackground Chunk
│   └── DynamicTimer Chunk
└── Footer Chunk
```

每个 chunk：

``` cpp
struct DisplayListChunk {
    ChunkId id;
    Rect bounds;

    uint64_t contentGeneration;
    ResourceUseSet resources;

    std::shared_ptr<const DisplayList> list;
};
```

静态 chunk 直接复用。

------------------------------------------------------------------------

# 16. Dirty System

建议：

``` cpp
enum class DirtyFlags : uint32_t {
    None       = 0,
    Style      = 1 << 0,
    Layout     = 1 << 1,
    TextLayout = 1 << 2,
    Paint      = 1 << 3,
    Transform  = 1 << 4,
    Opacity    = 1 << 5,
    Resource   = 1 << 6,
    Children   = 1 << 7,
    Composite  = 1 << 8,
};
```

### 示例：颜色变化

``` text
background
red -> blue

PaintDirty
```

不 Layout。

### 示例：width

``` text
width
200 -> 300

LayoutDirty
  ↓
affected descendants/ancestors
  ↓
PaintDirty
```

### 示例：translate 动画

``` text
translateX
100 -> 101

CompositeDirty
```

理想情况下：

``` text
NO parser
NO style
NO layout
NO text layout
NO subtree DisplayList rebuild
```

只更新 transform。

------------------------------------------------------------------------

# 17. Damage Region

除了 node dirty，还应维护屏幕 damage。

节点从：

``` text
old bounds = [100,100,200,200]
new bounds = [110,100,210,200]
```

damage 应包含：

``` text
union(oldBounds, newBounds)
```

因为旧位置也需要擦除/重绘。

``` cpp
class DamageTracker {
public:
    void add(Rect);
    void addOldAndNew(Rect oldRect, Rect newRect);
    Region finalize();
};
```

即使第一版最终仍整帧 present，这个抽象也应该保留。

------------------------------------------------------------------------

# 18. Culling

RenderNode 保存 world bounds。

``` cpp
if (!intersects(node.worldBounds, currentClip))
    skip;
```

但注意：

-   Shadow 会扩大 visual bounds。
-   Blur 会扩大 filter bounds。
-   Transform 后要使用 transformed bounds。
-   非矩形 Clip 可以先使用 conservative bounds。
-   Backdrop filter 不能简单按普通 subtree culling 处理。

因此建议同时区分：

``` cpp
layoutBounds
paintBounds
visualBounds
```

不要只有一个 `Rect bounds`。

------------------------------------------------------------------------

# 19. Batch

## 19.1 不要第一版自己接管 Vulkan Batch

Skia GPU backend 已经负责很多：

-   draw op aggregation
-   texture/resource cache
-   glyph atlas
-   pipeline/state 管理
-   GPU command recording

你的第一层优化应该是减少交给 Skia 的无效工作。

优先级：

``` text
Dirty
↓
Incremental Layout
↓
Text Cache
↓
DisplayList Cache
↓
Culling
↓
Layer Cache
↓
Composite-only Animation
↓
Resource Cache
↓
Profile
↓
Custom Batch if needed
```

## 19.2 引擎层可以保存 Batch 信息

``` cpp
struct BatchKey {
    MaterialClass material;
    ResourceId texture;
    BlendMode blend;
    ClipStateId clip;
    LayerId target;
};
```

但第一版用于统计：

``` text
draw commands = 830
potential compatible runs = 91
texture switches = 35
clip changes = 12
```

而不是立刻自己生成 `vkCmdDrawIndexed()`。

## 19.3 不能随便 reorder

2D UI 基本遵循 painter's order。

``` text
A
B
C
```

可能存在覆盖关系。

不能因为 A/C paint 相同就变：

``` text
A
C
B
```

除非做 dependency/overlap analysis。

第一版最多优化连续 compatible run。

------------------------------------------------------------------------

# 20. Layer Cache

昂贵 subtree：

``` text
Panel
├── SVG
├── Text
├── Image
├── Blur
└── Shadow
```

如果内容长期不变但整个 Panel 在移动：

``` text
Frame N:
subtree -> offscreen texture

Frame N+1:
reuse texture
transform changed only
composite
```

LayerCache：

``` cpp
struct LayerCacheEntry {
    LayerId id;

    Rect bounds;
    uint64_t contentGeneration;
    uint64_t effectGeneration;

    size_t estimatedBytes;
    uint64_t lastUsedFrame;

    BackendLayerHandle handle;
};
```

需要 LRU/budget。

### 自动 Promotion 参考因素

``` text
paint cost
filter cost
subtree command count
area
frames stable
transform animation active
memory cost
```

不要看到所有节点都缓存 layer，否则显存会迅速爆炸。

------------------------------------------------------------------------

# 21. Blur / Shadow / Mask 注意事项

这些效果往往会创建 offscreen surface。

需要 RenderTree 提前知道：

``` text
是否需要 saveLayer
layer bounds
filter outset
clip relationship
是否可缓存
```

### Blur

Blur radius 会扩大实际采样区域，因此：

``` text
visualBounds != layoutBounds
```

### Shadow

Shadow 也会扩大 damage/culling bounds。

### Backdrop Blur

这是特殊情况：

它依赖"节点后面的内容"。

意味着：

``` text
background content
↓
capture/sample
↓
blur
↓
foreground
```

会打破很多普通 subtree cache 假设。

建议把 backdrop filter 明确标记成高级 effect，不要和普通 blur
混为一个字段。

------------------------------------------------------------------------

# 22. Animation

Animation Engine 不直接画。

``` cpp
struct Animation {
    PropertyRef target;
    PropertyValue from;
    PropertyValue to;

    Duration duration;
    Easing easing;
};
```

每帧：

``` text
Timeline tick
 ↓
evaluate
 ↓
Property update
 ↓
Dirty classification
```

关键优化：

``` text
transform/opacity animation
        ↓
CompositeDirty
```

而：

``` text
width animation
        ↓
LayoutDirty
```

这两个性能等级完全不同。

未来 DSL 文档甚至可以提示开发者：

> transform 动画比 width/height 动画便宜。

------------------------------------------------------------------------

# 23. Event / HitTest

不要让 Skia 做 UI HitTest。

维护独立 HitTest 数据：

``` cpp
struct HitTestEntry {
    NodeId node;
    Rect bounds;
    Matrix3x3 inverseTransform;
    ClipId clip;
    int zOrder;
};
```

Pointer：

``` text
Wayland event
↓
logical coordinate
↓
HitTest
↓
Capture
↓
Target
↓
Bubble
```

必须考虑：

-   transform
-   clip
-   visibility
-   pointer-events
-   z order
-   disabled
-   modal layer
-   pointer capture

如果以后要支持拖拽，pointer capture 很重要。

------------------------------------------------------------------------

# 24. Focus / Keyboard

FocusManager 独立：

``` cpp
class FocusManager {
public:
    NodeId focusedNode() const;
    void requestFocus(NodeId);
    void clearFocus();
    void focusNext();
};
```

不要把 focus 绑定到 Wayland keyboard object。

Wayland 只说明窗口是否拥有 keyboard focus。

窗口内部哪个 DSL Node focus，是 Runtime 自己的事情。

------------------------------------------------------------------------

# 25. Frame Scheduler

建议每帧不是无脑 while(true) 60 FPS。

``` text
Event / Animation / Resource completion
            ↓
        requestFrame()
            ↓
 Wayland frame callback / scheduler
            ↓
         build frame
```

如果整个 UI 静止：

``` text
0 animation
0 dirty
0 video
```

可以不持续重新 build/render。

### Frame 状态

``` cpp
enum class FrameState {
    Idle,
    Requested,
    Building,
    Rendering,
    Presenting
};
```

避免多个 property change 重复 request frame。

------------------------------------------------------------------------

# 26. 多线程

第一版推荐：

``` text
Runtime/UI Thread
    │
    ├── Wayland events
    ├── DSL runtime
    ├── state/event
    ├── style/layout
    └── produce FramePacket
             │
             ▼
        Render Thread
             │
             ├── DisplayList replay
             ├── Skia
             ├── Vulkan submit
             └── present

Worker Pool
    ├── image IO/decode
    └── selected background work
```

不要一开始把 Scene 多线程读写。

FramePacket 尽量 immutable：

``` cpp
struct FramePacket {
    FrameId id;
    Viewport viewport;
    DamageRegion damage;

    shared_ptr<const DisplayList> displayList;
    ResourceUseSet resources;
};
```

------------------------------------------------------------------------

# 27. SkiaRenderer

Renderer 应该"傻"。

``` cpp
class SkiaRenderer final : public IRenderer {
public:
    FrameResult beginFrame(const FrameInfo&) override;
    void execute(const DisplayList&) override;
    void endFrame() override;

private:
    void executeDrawRect(...);
    void executeDrawPath(...);
    void executeDrawImage(...);
    void executeDrawText(...);

    SkCanvas* canvas_ = nullptr;
};
```

它不应该：

-   计算 layout
-   执行动画
-   解析 DSL
-   判断 button hover
-   加载图片
-   决定 component 状态

它只是：

``` text
Engine Draw Command
        ↓
Skia API
```

------------------------------------------------------------------------

# 28. Skia GPU Context 生命周期

建议独立：

``` cpp
class SkiaGpuContext {
public:
    bool initialize(VulkanDevice&);
    void abandon();
    void purgeCaches();
};
```

Renderer 不应该自己到处创建 GPU context。

### Device Lost

需要明确：

``` text
Vulkan device lost
↓
停止提交
↓
Skia context abandon/release
↓
Backend resources invalid
↓
ResourceManager backend generation++
↓
重建 device/context
↓
lazy recreate GPU resources
```

即使第一版只选择退出程序，也应该把错误路径设计清楚。

------------------------------------------------------------------------

# 29. Swapchain Resize

Wayland configure：

``` text
configure 1280x720
↓
update logical metrics
↓
calculate framebuffer extent
↓
mark swapchain out-of-date
↓
安全点 recreate
↓
recreate Skia target wrappers
↓
full damage
```

不要在 configure callback 中直接 destroy 当前正在 GPU 使用的资源。

需要等 fence / frame ownership 安全。

------------------------------------------------------------------------

# 30. 色彩空间

从开始至少把颜色定义为明确对象：

``` cpp
struct Color {
    float r, g, b, a;
    ColorSpaceId colorSpace;
};
```

第一版可以固定 sRGB，但 API 不要默认"所有颜色永远就是 uint32 ARGB"。

图片也保存 color space metadata。

否则未来 HDR/P3 会很难补。

------------------------------------------------------------------------

# 31. DPI / Fractional Scaling

DSL 的逻辑单位最好独立于 framebuffer pixel。

``` text
DSL logical unit
      ↓
Layout logical coordinate
      ↓
device scale
      ↓
physical pixel
```

文字和几何都在 logical coordinate 计算。

最终 Renderer 根据 scale 建立 transform。

避免上层到处乘 DPI。

------------------------------------------------------------------------

# 32. 热更新

既然是 DSL，非常值得从架构上支持 hot reload。

不要 reload 后整个 Runtime 全毁。

理想过程：

``` text
new DSL
↓
parse new AST
↓
semantic
↓
diff old/new definition
↓
stable key/id matching
↓
patch Scene Tree
↓
preserve compatible runtime state
↓
dirty affected subtree
```

因此 DSL 节点最好支持稳定 key。

例如概念上：

``` text
component key = "status-panel"
```

热更新时可以保留：

-   scroll position
-   focus
-   animation state（视规则）
-   runtime data binding

------------------------------------------------------------------------

# 33. 错误隔离

DSL 某个节点解析/资源失败，不应该导致整个 renderer 崩溃。

例如 Image load 失败：

``` text
ImageResource = Failed
↓
placeholder / error visual
↓
diagnostic
```

Path 错误：

``` text
skip invalid path
↓
report source range
```

Renderer 永远假设收到的是 semantic validated 数据。

------------------------------------------------------------------------

# 34. 内存管理

高频帧路径尽量避免：

``` text
new/delete
std::string allocation
shared_ptr churn
unordered_map lookup
```

建议：

-   FrameArena：每帧临时数据。
-   SceneArena：长期节点。
-   StringInterner：DSL identifier/style key。
-   SmallVector：少量 children/commands。
-   ResourceHandle：整数句柄。
-   DisplayList immutable storage。
-   generation handle 防悬空。

不要一开始为了"现代 C++"让每个 draw command 都拥有多个 `shared_ptr`。

------------------------------------------------------------------------

# 35. FrameArena

``` cpp
class FrameArena {
public:
    void* allocate(size_t, size_t alignment);
    void reset();
};
```

每帧：

``` text
begin frame
↓
arena.reset()
↓
build temporary render data
↓
submit
```

注意 Render Thread 异步消费的数据不能引用已经 reset 的 arena。

所以需要明确：

``` text
temporary build data
vs
FramePacket owned data
```

------------------------------------------------------------------------

# 36. Render Optimizer

建议独立模块：

``` cpp
class RenderOptimizer {
public:
    OptimizedFrame optimize(
        const RenderTree&,
        const PreviousFrameState&,
        const Viewport&);
};
```

它负责：

-   culling
-   cache decision
-   layer promotion
-   damage calculation
-   DisplayList chunk reuse
-   effect boundary
-   potential batch statistics

它**不允许改变视觉结果**。

------------------------------------------------------------------------

# 37. Render Cache Key

缓存不能只看 NodeId。

建议：

``` cpp
struct RenderCacheKey {
    NodeId node;
    uint64_t paintGeneration;
    uint64_t textGeneration;
    uint64_t resourceGeneration;
    float deviceScale;
};
```

如果 DPI 改变，文字/image sampling 可能需要重新生成。

------------------------------------------------------------------------

# 38. Golden Test

渲染引擎非常适合 screenshot/golden test。

``` text
DSL test case
↓
fixed viewport
↓
render
↓
PNG
↓
compare golden
```

测试：

-   RRect
-   Clip
-   Transform
-   Text
-   Image
-   Shadow
-   Blur
-   Nested opacity
-   fractional DPI

除了 pixel-perfect，可以设置容差，因为 GPU/AA 可能存在细微差异。

------------------------------------------------------------------------

# 39. Parser / Layout 单测

不要所有测试都依赖 GPU。

Parser：

``` text
source -> AST snapshot
```

Layout：

``` text
Scene + constraints
-> expected LayoutBox
```

Dirty：

``` text
change width
-> expected dirty nodes
```

DisplayList：

``` text
RenderTree
-> expected command sequence
```

这样大量 bug 在没有 Wayland/Vulkan 的 CI 环境也能测试。

------------------------------------------------------------------------

# 40. Debug Inspector

强烈建议从早期做开发模式：

``` text
F1 Inspector
```

显示：

``` text
NodeId
type
layout bounds
paint bounds
dirty flags
computed style
resource IDs
DisplayList cache hit
layer cache
```

Overlay：

-   layout border
-   paint bounds
-   clip bounds
-   damage region
-   cached layer
-   culled node
-   repaint flash

这种工具对自研 UI 引擎的价值非常高。

------------------------------------------------------------------------

# 41. Profiling

建议接入 Tracy，并同时做自己的统计。

每帧：

``` cpp
struct FrameStats {
    double styleMs;
    double layoutMs;
    double textMs;
    double renderTreeMs;
    double displayListMs;
    double skiaCpuMs;
    double gpuMs;

    uint32_t dirtyNodes;
    uint32_t layoutNodes;
    uint32_t paintNodes;
    uint32_t drawCommands;
    uint32_t culledNodes;

    uint32_t displayListCacheHits;
    uint32_t layerCacheHits;
};
```

不要只看 FPS。

一个 240Hz 目标：

``` text
frame budget ≈ 4.17 ms
```

必须知道时间到底花在哪。

------------------------------------------------------------------------

# 42. 视频/串流纹理预留

如果未来客户端需要显示实时视频/串流画面，建议现在预留：

``` cpp
VideoFrameId
ExternalImageId
```

不要把视频伪装成普通每帧重新创建 Image。

需要考虑：

-   NV12/P010/YUV
-   color conversion
-   external VkImage
-   decoder output
-   acquire/release synchronization
-   zero-copy
-   Skia 与自定义 Vulkan pass 的 ownership

未来可以允许：

``` text
RenderTree
├── Skia content
├── External video layer
└── Skia overlay
```

必要时由 compositor/render graph 组合，而不是强迫所有东西必须经过
SkCanvas。

------------------------------------------------------------------------

# 43. 为未来自定义 Vulkan Pass 留接口

Skia 是主 renderer，但不要把最终架构封死。

建议最终 Render Backend 概念：

``` text
RenderPass
├── Skia2DPass
├── VideoPass
├── CustomShaderPass
└── CompositePass
```

第一版只有：

``` text
Skia2DPass
```

即可。

如果以后 profile 发现某类效果用原生 Vulkan 更合适，可以增加
pass，而不推翻 Scene/DSL。

------------------------------------------------------------------------

# 44. 工程目录建议

``` text
engine/
├── base/
│   ├── ids/
│   ├── math/
│   ├── arena/
│   ├── containers/
│   └── time/
│
├── dsl/
│   ├── source/
│   ├── lexer/
│   ├── parser/
│   ├── ast/
│   ├── semantic/
│   └── diagnostics/
│
├── runtime/
│   ├── scene/
│   ├── property/
│   ├── binding/
│   ├── component/
│   ├── event/
│   ├── focus/
│   └── hit_test/
│
├── style/
├── layout/
├── animation/
├── text/
│
├── resource/
│   ├── image/
│   ├── font/
│   ├── shader/
│   └── cache/
│
├── render/
│   ├── geometry/
│   ├── paint/
│   ├── render_tree/
│   ├── effects/
│   ├── display_list/
│   ├── damage/
│   ├── layer/
│   ├── optimizer/
│   └── scheduler/
│
├── render_skia/
│   ├── SkiaRenderer
│   ├── SkiaGpuContext
│   ├── SkiaResourceProvider
│   └── SkiaConverters
│
├── platform/
│   ├── wayland/
│   └── vulkan/
│
├── diagnostics/
│   ├── profiler/
│   ├── inspector/
│   └── frame_dump/
│
└── tests/
    ├── parser/
    ├── semantic/
    ├── layout/
    ├── dirty/
    ├── display_list/
    ├── golden/
    └── performance/
```

------------------------------------------------------------------------

# 45. 依赖方向

严格限制依赖：

``` text
base
↑
dsl        platform
↑             ↑
runtime       │
↑             │
style/layout  │
↑             │
render        │
↑             │
render_skia ──┘
```

尤其禁止：

``` text
runtime -> render_skia
layout  -> Skia
dsl     -> Vulkan
```

可以通过 CI include rule 或 CMake target dependency 保证。

------------------------------------------------------------------------

# 46. CMake Target 建议

``` text
pureui_base
pureui_dsl
pureui_runtime
pureui_style
pureui_layout
pureui_animation
pureui_text
pureui_resource
pureui_render
pureui_platform_wayland
pureui_platform_vulkan
pureui_render_skia
pureui_diagnostics
```

最终应用：

``` text
pureui_client
```

这样以后 parser/layout 可以在无 Vulkan 环境独立测试。

------------------------------------------------------------------------

# 47. 第一阶段 MVP

不要第一阶段就做高级 DSL。

目标：

``` text
Wayland Window
↓
Vulkan
↓
Skia GPU Surface
↓
DisplayList
↓
Rect / RRect / Image / Text
↓
Scene Tree
↓
简单 DSL
```

成功标准：

-   打开 Wayland 原生窗口。
-   Vulkan + Skia GPU 正常工作。
-   resize 正常。
-   DPI 正常。
-   DSL 可以生成 Scene。
-   Scene 能 layout。
-   RenderTree 能生成 DisplayList。
-   DisplayList 能画矩形、图片、文字。
-   输入能 HitTest。
-   静止时可以不持续重绘。

------------------------------------------------------------------------

# 48. 第二阶段：增量系统

实现：

-   Property metadata
-   Dirty propagation
-   generation
-   incremental layout
-   TextLayout cache
-   DisplayList chunk cache
-   culling
-   damage tracking
-   FrameStats

这一阶段完成后再开始认真谈性能。

------------------------------------------------------------------------

# 49. 第三阶段：高级视觉

实现：

-   Path
-   Stroke
-   Gradient
-   nested clip
-   shadow
-   blur
-   mask
-   filters
-   custom SkSL effect
-   layer boundary

同时增加：

-   paint bounds
-   visual bounds
-   effect outset

------------------------------------------------------------------------

# 50. 第四阶段：动画与合成优化

实现：

-   Timeline
-   Easing
-   Transform animation
-   Opacity animation
-   Layer promotion
-   Layer cache
-   Composite-only update

重点 benchmark：

``` text
1000 static nodes + moving parent
```

应该避免 1000 个节点每帧重新生成 DisplayList。

------------------------------------------------------------------------

# 51. 第五阶段：性能专项

这时候才依据 Tracy/FrameStats 判断：

-   Skia CPU submit 是否瓶颈？
-   GPU fill-rate 是否瓶颈？
-   blur 是否瓶颈？
-   texture upload 是否瓶颈？
-   layout 是否瓶颈？
-   text shaping 是否瓶颈？
-   layer cache 是否占用过多显存？
-   是否需要 custom Vulkan pass？
-   是否真的需要自研 batch？

如果：

``` text
Layout 2.8ms
Skia CPU 0.4ms
GPU 0.8ms
```

就应该优化 Layout，而不是 Batch。

------------------------------------------------------------------------

# 52. 最重要的几个"不要"

1.  **不要 AST 直接调用 SkCanvas。**
2.  **不要 SceneNode 保存 Skia/Vulkan 类型。**
3.  **不要 Renderer 计算 Layout。**
4.  **不要 Renderer 负责业务状态。**
5.  **不要每帧 Parse DSL。**
6.  **不要所有变化都标成一个 bool dirty。**
7.  **不要 width 动画和 transform 动画走同一条重绘路径。**
8.  **不要为了 Batch 随意改变 painter's order。**
9.  **不要每个高级效果都创建永久 Layer。**
10. **不要 worker thread 直接修改 Scene。**
11. **不要把 Wayland logical coordinate 与 framebuffer pixel
    混为一谈。**
12. **不要在核心层暴露 `wl_*`、`Vk*`、`Sk*` 类型。**
13. **不要在没有 profiling 的情况下自己重写 Skia 已经完成的 GPU 优化。**
14. **不要忽略 device lost / resize / resource failure 等异常路径。**
15. **不要等项目很大以后才做 Inspector 和 FrameStats。**

------------------------------------------------------------------------

# 53. 最终推荐的数据流

正常第一次加载：

``` text
DSL
↓
Lexer
↓
Parser
↓
AST
↓
Semantic
↓
Scene creation
↓
Style
↓
Layout
↓
Text Layout
↓
Render Tree
↓
DisplayList
↓
Skia
↓
Vulkan
↓
Wayland
```

普通属性更新：

``` text
Property change
↓
Dependency invalidation
↓
Dirty classification
↓
affected Style/Layout/Paint only
↓
reuse unchanged DisplayList chunks
↓
Skia
```

Transform 动画：

``` text
Animation tick
↓
Transform property
↓
CompositeDirty
↓
reuse subtree content/layer
↓
new transform
↓
composite
```

资源异步完成：

``` text
Image decoded
↓
ResourceReadyEvent
↓
dependent node ResourceDirty
↓
intrinsic size changed?
├── yes -> LayoutDirty
└── no  -> PaintDirty
```

窗口 resize：

``` text
Wayland configure
↓
WindowMetrics update
↓
Vulkan swapchain recreation at safe point
↓
Skia target recreation
↓
root LayoutDirty
↓
full damage
```

------------------------------------------------------------------------

# 54. 推荐的长期架构结论

最终建议把项目理解成五个彼此独立的系统：

``` text
1. Language
   DSL / Parser / Semantic

2. UI Runtime
   Scene / Property / Binding / Event / State

3. Layout & Visual Model
   Style / Layout / Text / Animation / RenderTree

4. Rendering Engine
   DisplayList / Dirty / Cache / Layer / Culling /
   Damage / Scheduler / Optimizer

5. Backend & Platform
   Skia / Vulkan / Wayland
```

其中 **第 2～4 层才是你的核心技术资产**。

Skia 的价值是让你不用从 Vulkan 开始重新实现成熟的 2D
rasterization、Path、文字绘制、Filter、AA 和大量 GPU
细节；但你的引擎仍然负责"什么时候算、什么时候不算、哪些内容复用、哪些节点提升为
Layer、什么变化需要 Layout、什么变化只需要 Composite"。

因此性能架构的中心不是：

``` text
How do I batch more draw calls?
```

而应该是：

``` text
How do I avoid generating unnecessary work at all?
```

推荐最终优化优先级保持为：

``` text
正确的 invalidation
        ↓
增量 Style/Layout/Text
        ↓
DisplayList reuse
        ↓
Culling / Damage
        ↓
Layer / Composite reuse
        ↓
Resource cache
        ↓
Skia GPU
        ↓
Profiling
        ↓
必要时才 Custom Batch / Custom Vulkan Pass
```

这套架构既能让第一版较快落地，也为未来
2K/4K、高刷新率、大量动画、视频纹理和高级 GPU 效果保留了足够的演进空间。
