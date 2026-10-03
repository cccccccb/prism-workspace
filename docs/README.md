# Prism 文档索引

## 第三方应用开发入口

新应用按统一 Host、DSL、主题与业务模块契约开发，从以下入口开始：

| 文档 | 用途 |
| --- | --- |
| [第三方应用设计与开发总览](THIRD_PARTY_APP_GUIDE.md) | 架构、阅读顺序、开发交付与调用 Skill |
| [prism-app-ui Skill](skills/prism-app-ui/SKILL.md) | 给 AI 开发者复用的工作规范与按需阅读入口 |
| [详细开发指南](skills/prism-app-ui/references/app-development.md) | 包结构、DSL、Preview/Master、并行准备、C ABI、异步业务与完整实例 |
| [视觉设计规范](skills/prism-app-ui/references/visual-design.md) | 图标、排版、主题、明暗配色、材质、阴影、BSP 布局与 DSL 示例 |
| [Counter 应用模板](skills/prism-app-ui/assets/starter/README.md) | 可复制的独立 CMake 项目、完整 DSL 文件和纯业务模块 |

Skill 与参考资料只有一份正文，统一在仓库维护。人和 AI 使用相同规范；详细指南区分
当前已支持的接口与待实现能力，开发时仍需核对所使用版本的契约和 schema。

## 当前契约与实现规范

| 领域 | 文档 |
| --- | --- |
| 代码规范 | [CODING_STYLE](CODING_STYLE.md) |
| 应用启动与进程边界 | [APP_LAUNCH_CONTRACT](APP_LAUNCH_CONTRACT.md)、[PROCESS_CONTRACTS](PROCESS_CONTRACTS.md) |
| 统一 Host 与业务模块 | [APP_HOST_RUNTIME](APP_HOST_RUNTIME.md) |
| Preview/Master 与并行加载 | [MASTER_PARALLEL_LOADING](MASTER_PARALLEL_LOADING.md) |
| 客户端 SDK 与 Scene | [CLIENT_APP_SDK](CLIENT_APP_SDK.md)、[CLIENT_SCENE_RUNTIME](CLIENT_SCENE_RUNTIME.md) |
| DSL 与绘制契约 | [DSL_REWRITE_PLAN](DSL_REWRITE_PLAN.md)、[DISPLAY_LIST_CONTRACT](DISPLAY_LIST_CONTRACT.md) |
| 主题编写与切换 | [THEME_AUTHORING](THEME_AUTHORING.md)、[DSL_THEME_RUNTIME](DSL_THEME_RUNTIME.md) |
| 材质与图片资源 | [SURFACE_MATERIALS](SURFACE_MATERIALS.md)、[IMAGE_RESOURCES](IMAGE_RESOURCES.md) |
| 渲染调度与失效传播 | [RENDER_SCHEDULING_AND_INVALIDATION](RENDER_SCHEDULING_AND_INVALIDATION.md) |
| 动画运行时与 DSL 契约 | [ANIMATION_RUNTIME_SPEC](ANIMATION_RUNTIME_SPEC.md)（Paint Transition、StateRule 与装饰呈现；保留层后续实施） |
| 通用交互状态与桌面控制区 | [INTERACTION_AND_PRESENTATION_SPEC](INTERACTION_AND_PRESENTATION_SPEC.md)（固定 InteractionTarget、Visual、状态规则与 Shell 接入；成功提交输入快照、鼠标组沉浸与恢复；分隔线控制见专项计划，触屏交互延期） |
| 动画与渲染优化假设 | [ANIMATION_RENDERING_HYPOTHESES](ANIMATION_RENDERING_HYPOTHESES.md) |
| 客户端渲染线程迁移 | [CLIENT_RENDER_THREAD_MIGRATION](CLIENT_RENDER_THREAD_MIGRATION.md) |
| Wayland 生命周期 | [WAYLAND_LIFECYCLE](WAYLAND_LIFECYCLE.md) |
| launcher 与待命池 | [LAUNCHER_WORKER_POOL](LAUNCHER_WORKER_POOL.md)、[SESSION_LAUNCH_RUNTIME](SESSION_LAUNCH_RUNTIME.md) |
| Preferences 与性能测量 | [PREFERENCES_AND_PERFORMANCE](PREFERENCES_AND_PERFORMANCE.md) |

修改某项通用能力时，同时更新对应契约、Skill 中受影响的指南和相关示例，避免应用
作者依赖过期属性或测试结论。

## 构建、部署与实机验证

| 文档 | 用途 |
| --- | --- |
| [PI_BUILD](PI_BUILD.md) | Pi 编译与依赖 |
| [SKIA_BACKEND_PI](SKIA_BACKEND_PI.md)、[SKIA_GLES_PI](SKIA_GLES_PI.md) | Skia 后端与图形环境 |
| [PI_DEB_DEPLOYMENT](PI_DEB_DEPLOYMENT.md) | deb、服务、实机版本与验证记录 |
| [PI_REMOTE_DESKTOP](PI_REMOTE_DESKTOP.md) | Headless Prism、WayVNC、Windows SSH 隧道与远程输入验收 |
| [BASELINE_ACCEPTANCE](BASELINE_ACCEPTANCE.md)、[基线校验文件](baseline.sha256) | 基线验收 |

## 设计来源与阶段计划

以下文档记录设计背景和演进路径。其中的目标与计划不能单独证明某项 API 已实现；
判断当前支持情况时，以现行契约、schema 和源码为依据。

- [原始架构详细设计](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md)
- [总体设计](DESIGN_SPEC.md)
- [Skia 迁移计划](SKIA_MIGRATION_PLAN.md)
- [应用迁移](APPLICATION_MIGRATION.md)
- [启动架构修订与执行计划](LAUNCH_RUNTIME_RESTORATION_PLAN.md)
- [视觉与 BSP 细化计划](VISUAL_TILING_REFINEMENT_PLAN.md)
- [WM 权威布局、组沉浸与分隔线控制实施计划](LAYOUT_CONTROL_IMPLEMENTATION_PLAN.md)（权威快照、连续手势、typed 控制会话及鼠标组沉浸/恢复；鼠标分隔线调节与递归尺寸约束，触屏交互延期）
- [阶段报告](MILESTONE_REPORT.md)
