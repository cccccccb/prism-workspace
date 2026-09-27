---
name: prism-app-ui
description: Design, implement, or review third-party applications for the Prism desktop using its DSL, theme system, unified app host, and business module ABI. Use for Prism application UI and package development, including work delegated to another AI developer.
---

# Prism 第三方应用设计与开发

为 Prism 桌面生成风格统一、职责清晰、能适应 BSP 窗口尺寸的应用。默认交付应用包：
DSL 前端、资源、纯业务模块；使用平台现有 `prism-app-host` 和 launcher。

本技能随仓库规范维护，以源码和当前契约为依据。示例是可复制的开发起点，应用的功能、
内容、页面数量和业务依赖由具体需求决定。

## 按任务阅读

- **新建应用或连接业务**：阅读 [开发指南](references/app-development.md)，选择状态、
  动作、加载区域与包结构；可复制 [Counter 模板](assets/starter/)。
- **设计布局或调整外观**：阅读 [视觉设计规范](references/visual-design.md)，核实主题
  token、材质、图标和窄窗布局；业务接入时再读开发指南对应章节。
- **异步准备或性能问题**：阅读开发指南的加载、业务工作和性能章节，并核对当前项目
  的 `docs/MASTER_PARALLEL_LOADING.md`、`docs/RENDER_SCHEDULING_AND_INVALIDATION.md`。
- **评审已有应用**：检查职责、主题接口、状态/输入、加载/退出及交付验证，不为了评审
  重写已符合规范的实现。

完整索引在 [项目文档汇总](../../README.md)。本技能的两个参考文件及模板自包含；
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
   BSP 窄窗中先保留主要点击区，隐藏子树使用 `visible`。
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

先整理「页面区域—binding—action—加载阶段—主题引用」表，再实现界面和业务。
把空、加载中、失败与正常状态设计成明确的 UI；少量内存操作不为异步而异步。

验证实际包路径、模块导出/依赖、DSL 语义与绑定、窄窗主操作、主题/明暗切换、
异步取消与退出；测试/probe 在独立目录，开发模板不进入生产包。已有验证足以覆盖
本次改动时，结束额外测试。

交付时说明文件、支持的功能、验证范围和实质限制。编写技能或应用不会自动授权安装
第三方包、切换正在运行的桌面会话或改动用户环境；这些动作遵循本次任务已有授权。
