---
name: prism-app-ui
description: Design, implement, or review third-party applications for the Prism desktop using its DSL, theme system, unified app host, and business module ABI. Use for Prism application UI and package development, including work delegated to another AI developer.
---

# Prism 第三方应用设计与开发

为 Prism 桌面生成风格统一、职责清晰、能适应 BSP 窗口尺寸的应用。默认交付应用包：
DSL 前端、资源、纯业务模块；使用平台现有 `prism-app-host` 和 launcher。

本技能随仓库规范维护，以源码和当前契约为依据。示例是可复制的开发起点，应用的功能、
内容、页面数量和业务依赖由具体需求决定。

## 应用规范与阅读顺序

普通第三方 UI 任务先读本文件，再读设计尺度和视觉参考；无需先翻旧 demo 源码。
设计建议可以随产品调整，Host/主题/输入/ABI 边界必须保持。新增能力先形成通用契约，
不要在应用中假造属性或在 WM 增加应用分支。

## 视觉设计纠偏

v21 的界面未获用户认可，不作为视觉基线。图标主导不等于隐藏文字；禁止把少量设置行
按 flex 平均拉成巨大空卡片。用户已确认第二张 Concept B 为本轮实现基准，见
[确认稿与落地尺度](../../design/prism-concept-b/README.md)。首版实现仍被指出偏差，
必须按[逐项还原检查](../../design/prism-concept-b/FIDELITY_REVIEW.md)核对控件、资产、
背景、边线和间距，不能把功能测试通过写成视觉已认可。重新确定整体风格时遵循
[视觉方案与密度验收](references/visual-approval.md)，再进入实现与部署。

## 界面系统后续基准

控件、浮层、文件任务和反馈设计参考 [界面系统执行计划](../../INTERFACE_SYSTEM_PLAN.md)。
该设计补充 Concept B；能力表明确区分已有接口与提案。`enabled` 已接入本轮源码的
Boolean 属性/绑定，旧 v22 不支持；系统文件服务和透视翻转仍待实现。
不要把截图尺寸当逻辑尺寸，不要把图中的文案或外观当成系统权限。
Popup 与 Menu 已接入本轮输入；MenuItem/MenuBack 支持替换面板式子菜单。
子菜单声明置于 Scene 根末尾，必须有明确返回入口。Host 按能力选择 native 子层或
原窗口呈现，侧向级联仍未接入；应用不自行选择 target 或创建前端进程。
含浮层的窗口将普通内容 clip 与根末尾 Popup/Menu 分开，根保持 window 材质；
具体组合见视觉设计规范 3.1，不能要求 Host 忽略祖先裁剪来取得 native。
开发前核对 [浮层契约](../../POPUP_MENU_CONTRACT.md) 的当前状态，不用 Card 冒充完整菜单。

文字行高与控件组合见 [状态配方](../../INTERFACE_CONTROLS_AND_TYPOGRAPHY.md)。
新增 `lineHeight` 与 `space_*`/`line_*`/`controlPrimary*` 等 token 需配套本轮 Host/主题；
不代表自动折行或字体回退已实现。

值控件接入见 [值交互契约](../../CONTROL_VALUE_CONTRACT.md)：本轮 Checkbox 使用
`checked` 绑定与 `on_control_value` 回调，Visual 子树通过同一绑定呈现选中状态。
RadioGroup/Radio 与 SegmentGroup/Segment 使用组级 `selectedKey` 和选项固定 key，
以只读 `selected` 状态呈现选择。水平 Slider 已接入：固定 minimum/maximum/step，
value 绑定；三个 sliderPart Visual 分别定义轨道、填充与滑块，业务区分 Preview/Commit/Cancel。
不要用 on_action 接收值，也不要把 Preview 当作已确认状态写回 value。示例和限制见契约第 11 节。


## 按任务阅读

- **新建应用或连接业务**：阅读 [开发指南](references/app-development.md)，选择状态、
  动作、加载区域与包结构；可复制 [Counter 模板](assets/starter/)。
- **设计布局或调整外观**：先读 [设计尺度与页面配方](references/design-system.md)，
  再读 [视觉设计规范](references/visual-design.md)，核实主题
  token、材质、图标和窄窗布局；业务接入时再读开发指南对应章节。
- **主题、动效和版本兼容**：读 [动效与坑点](references/motion-and-pitfalls.md)，
  确认所需 Host 能力、命名 motion、输入范围和材料责任；可复制
  [紧凑 Library 视觉组件](assets/examples/compact-library.prism)。
- **异步准备或性能问题**：阅读开发指南的加载、业务工作和性能章节，并核对当前项目
  的 `docs/MASTER_PARALLEL_LOADING.md`、`docs/RENDER_SCHEDULING_AND_INVALIDATION.md`。
- **评审已有应用**：检查职责、主题接口、状态/输入、加载/退出及交付验证，不为了评审
  重写已符合规范的实现。

完整索引在 [项目文档汇总](../../README.md)。本技能的参考文件及模板提供独立开发所需说明；
离开仓库使用时，需由开发者提供匹配版本的 Prism 头文件、Host 和主题包。

## 必须保持的边界

1. WM 管窗口树、焦点、分层、外围装饰和跨 surface 背景效果；不加入应用 DSL、
   Scene、binding 或特定应用的绘制分支。DSL/布局/绘制能力属于通用前端。
2. 普通应用通过业务 C ABI 发布有类型状态和具名动作。业务模块不持有 Wayland、
   EGL、Skia、Scene 或 SDK 主循环，也不自行 spawn/exec 另一个前端。
3. Host 管 Preview/Master、surface、资源、主题、输入及模块生命周期；共享准备池
   产出不可变结果，所有者线程安装与呈现。Preview、Master 使用同一前端窗口。
4. 窗口根使用 `material: "window"`，局部层次使用共享材质与 `@token`。材质主题和
   light/dark 独立；常规窗口根轮廓与外围装饰保持一致，内外阴影按绘制责任分配。
5. 图标承载主操作，文字提供标题、数据、分区和必要说明。采用目前可用的向量图标；
   BSP 窄窗中先保留主要点击区；业务隐藏用 `visible`，尺寸分支用
   `minViewportWidth/maxViewportWidth/minViewportHeight/maxViewportHeight`。
6. Interface v2 显式声明 typed binding、critical/deferred 组件和稳定 Slot。独立区域
   不添加虚假的 `after`；把有真实准备依赖的关系写入加载图。
7. `create`、动作与完成回调快速返回；耗时业务用 Host 管理的 work。工作线程处理
   复制的值数据，完成回到所有者线程。C ABI 尾字段先检查完整 `struct_size` 和指针，
   借用数据按生命周期复制，异常在 ABI 边界内处理。
8. Ready 表示业务实际可用，提交、Swap、实际呈现分别记录；根据真实事件更新
   选中态、运行态和进度。动画、任意动态列表、文本编辑等能力使用前先核对当前 schema。
9. 保持按事件更新与一次性 tick；生产实现不添加固定忙循环、`glFinish` 或测试延迟。
   性能结论使用同内容、同管线、明确计时边界的测量。
10. C++ 排版采用项目 Qt 风格，依赖使用标准 C++ 和纯契约；遵守无 goto、具名长期
    回调、typed JSON、职责间空行和自有生产 C/C++ 单文件最多 800 行的规则。

## 交付方式

先整理「页面区域—binding—action—加载阶段—主题/motion 引用」表，并写出主要
字号、间距、图标/命中尺寸与窄窗策略，再实现界面和业务。
默认标题 20/18、正文 14、辅助 11，局部间距以 4/8/12/16 为推荐尺度，根留白
引用主题；这些是设计起点，不是强制固定布局或尚不存在的 spacing token。
把空、加载中、失败与正常状态设计成明确的 UI；少量内存操作不为异步而异步。

验证实际包路径、模块导出/依赖、DSL 语义与绑定、窄窗主操作、主题/明暗切换、
异步取消与退出；测试/probe 在独立目录，开发模板不进入生产包。已有验证足以覆盖
本次改动时，结束额外测试。

交付时说明文件、支持的功能、验证范围和实质限制。编写技能或应用不会自动授权安装
第三方包、切换正在运行的桌面会话或改动用户环境；这些动作遵循本次任务已有授权。

长设置页与短窗口使用纵向 `ScrollView`，固定标题/底栏放在视口外；开发前阅读
[滚动容器契约](../../SCROLL_VIEW_CONTRACT.md)。指示条目前仅呈现，不能设计成可拖动操作；
编辑器仍使用自身滚动。内容使用紧凑自然高度，不靠拉大空隙填充页面。

涉及浮层圆角/裁剪/模糊时核对 [轮廓契约](../../ROUNDED_REGION_CONTRACT.md)。
外阴影不作命中区，外点关闭屏障不作菜单命令区。连接颈的
[通用轮廓契约](../../SURFACE_CONTOUR_CONTRACT.md)已贯通纯几何、payload、Skia DrawList、
Wayland v2、WM 遮罩以及 Scene/DSL、实时/快照命中和输入交集。局部容器或 Popup/Menu
可声明 `Contour(space: "local") { Move(...) Line(...) Cubic(...) }`，使用具名数值字面量；
这是独立几何元数据，不占布局子项或 Slot 索引。阅读契约中的完整配方与预算后使用。
轮廓覆盖 cornerRadius 的形状语义，不随尺寸自动缩放；拒绝 window 材质、生成式控件
部件、动态绑定、normalized 空间和当前不能精确表达的 blur 裁剪。
4h 新增 Popup/Menu 的 `Contour(recipe: "attachedPanel", radius: "@panel_radius",
neckWidth: "@space_section", neckHeight: "@space_sm", fallback: "detached")`。三数值
必填，可引用主题 Number；根据最终 Below/Above 定位生成单一轮廓，Square 保持直角。
正文尺寸/padding 不重复预留颈部，颈尖距锚点 8；空间不足按显式 detached 回退。
v25 增加 `neckShape: "roundedTriangle"`，以圆润三角代替平顶；宽/高可引用
`@panel_neck_width`/`@panel_neck_height`。省略形状保持旧 softTab，Square 使用直线。
参见轮廓契约的圆润三角节；字段自v25支持，用户选定的C造型需要v26 Host及匹配
主题，不能向更旧版本声明新增字段。
请读契约中的缓存、主题候选、输入和配方限制；不要自行在应用中拼接重叠背景。
4i—4k3 由 Host 管理同连接子层、共享 Context/Ganesh 与各 target WSI、输入和提交
基线。scale=1、能准确导出局部布局时按能力自动 native；首个 child Pixels 成功才撤
原窗口面板。背景效果使用 v3 独立 popup capability；需要 Contour 时同时要求 contour
能力。首次 blur 等 root 实际提交撤下 fallback 后，以 child State 开启，避免采到旧面板。
blur-only 更新不要求像素重绘；业务动作和值回调保持原接口。旧 compositor、编辑器和
不支持的裁剪/变换保留 root。不要按主题名称猜测能力，也不要新建 ClientApplication、
调用 Wayland 或启动业务进程来绕过回退。
开发浮层时阅读 [子层生命周期](../../POPUP_TARGET_LIFECYCLE.md)、
[背景采样契约](../../POPUP_BACKDROP_CONTRACT.md)和
[Popup/Menu 契约](../../POPUP_MENU_CONTRACT.md)，核对匹配 Host；本步没有新增 DSL 属性。
同 surface 正文模糊仍待实现，native popup 的父正文采样不能视作它已经完成。
匹配这些接口的 `0.1.0-26` Host/WM 已部署到 Pi 正式远程会话，旧 v22 不支持。
部署与实际菜单检查见 [最新记录](../../PI_REMOTE_DESKTOP.md)；最终视觉仍待用户确认，
测试图和自动检查不代替用户验收。
