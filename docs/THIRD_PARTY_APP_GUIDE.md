# Prism 第三方应用设计与开发总览

更新：2026-10-04。面向应用作者、UI 设计者和参与开发的 AI。

## 1. 我们要保持的设计

第三方应用交付 **DSL 前端、资源与业务模块**。统一 `prism-app-host` 接管前端
生命周期，平台 launcher 接管实例启动与激活。应用决定业务内容和操作；框架提供
主题、控件、加载、布局、输入与绘制接口，使不同应用在同一桌面下保持一致风格。

```text
应用包：manifest + Preview/Master DSL + 组件/资源 + 业务 .so
                         ↓
              launcher → 统一 prism-app-host
                         ↓
      DSL / typed binding → Scene / Layout → DrawCall → Skia
                         ↓
                    Wayland surface
                         ↓
         WM：BSP、焦点、装饰、跨 surface 背景效果
```

业务与界面通过有类型的 binding 和具名 action 交互。业务模块通过公开 C ABI 使用
Host 能力，耗时准备可以提交给共享工作池，完成回到所有者线程。WM 中不添加具体
应用的 DSL、状态或绘制逻辑；新增控件或资源能力应作为通用框架接口实现。

普通应用使用同一个 surface 展示轻量 Preview 和 Master。Master 声明独立的
critical/deferred 组件与稳定 Slot，准备可并行，资源安装与呈现由所有者线程管理。
Preview 提交、业务 Ready、Master 实际呈现及 deferred 完成是不同事件。

## 2. 统一视觉语言

- **图标为主，文字为辅**：主操作用含义清楚的图标；标题、分区、业务数据和必要
  说明用文字。歧义操作保留简短标签。
- **材质与明暗独立**：玻璃、半透明、透明、直角来自会话主题；light/dark 提供
  独立配色。UI 引用共享 token 和材质，避免固定颜色使主题切换失效。
- **层次清晰**：根 `window` 材质配合 WM 外围装饰；内容分组用 `card`，按钮用
  `control`。阴影和模糊按各层职责使用。
- **适应 BSP**：manifest 尺寸是偏好，真实 configure 尺寸决定布局。窄窗优先保留
  主操作，检查裁剪、文字与输入区域；当前 DSL 没有通用响应式断点或滚动列表。
- **及时反馈、按事件更新**：状态由真实业务结果驱动，隐藏子树不留下命中区；
  状态反馈优先引用命名 Motion；不以业务逐帧 tick 驱动动画，闲置时停止重画。

字号、间距、图标点击尺寸及设置/内容页配方见
[设计尺度](skills/prism-app-ui/references/design-system.md)；版本能力、系统动效和常见错误见
[动效与坑点](skills/prism-app-ui/references/motion-and-pitfalls.md)。这些说明面向不阅读
旧应用源码的作者。具体 token、图标、属性、尺寸建议与完整片段在
[视觉设计规范](skills/prism-app-ui/references/visual-design.md) 中维护。

## 3. 推荐开发顺序

1. 梳理页面，列出「区域—binding—action—加载阶段—主题引用」表，设计正常、空、
   加载与失败状态。
2. 按当前组件 schema 编写主题化布局，确认窄窗中的主操作。缺少通用能力时先提出
   契约扩展，不要用应用专属 WM 分支绕过框架。
3. 形成应用包：manifest、轻量 Preview、Interface v2、layout、组件与 assets。
4. 实现纯业务模块，发布 typed 状态，处理动作与实际 Ready。耗时业务使用有界
   work，明确输入、结果、配额、取消和退出生命周期。
5. 分层验证包、解析、绑定、业务、布局和真实呈现。性能测量区分 CPU 准备、安装、
   渲染提交与实际呈现，不把线程数等同于加速效果。

详细步骤、ABI 注意事项、加载图、DTO 示例与交付方法见
[详细开发指南](skills/prism-app-ui/references/app-development.md)。

## 4. 可复制的完整示例

[Counter 模板](skills/prism-app-ui/assets/starter/README.md) 包含独立 CMake 项目、
manifest、Preview、Master、三个组件和纯业务模块。它演示：

- 两个独立 critical 区域和一个独立 deferred 区域；
- string/number/bool 绑定、增减操作、图标重置、进度与显示开关；
- 主题引用、紧凑布局、Ready、异常边界与借用数据处理。

该模板可复制到新项目中编译，业务模块只依赖公开头文件。异步 DTO、工作池与取消
参考指南及真实 [Music 模块](../demos/demo_player/player.cpp)；Counter 的简单内存
操作不需要额外后台任务。模板不会自动注册应用或进入当前桌面生产包。

本次模板维护验证（2026-09-27）：Pi ARM64 Release 编译通过；四材质 × 明暗 ×
三种参考尺寸的 24 个 CPU Scene/真实业务模块组合通过，包括按钮命中、Tab 遍历、
deferred 挂载/隐藏恢复、初始状态和计数上下界。视觉指南六个组件示例共 48 个主题
组合通过纯前端验证。此记录不包含 GPU 回放、真实 Wayland 输入与实际呈现；图片
例子只验证资源引用，实际 PNG 仍需由应用提供并验证。

另外，由独立 AI 按 Skill 生成的设备状态前端也通过了 24 个主题/尺寸组合的解析、
绑定、区域挂载和 pending/ready 命中检查，验证了规范可用于不同应用。该复用检查
使用注入状态，没有实现设备采样模块或启动新应用。

## 5. 给其他 AI 使用

技能名称：`prism-app-ui`，正文位于
[SKILL.md](skills/prism-app-ui/SKILL.md)。仓库入口为 `.agents/skills/prism-app-ui`，
指向同一份文档。当前开发环境的 `~/.codex/skills/prism-app-ui` 也指向该目录。
技能目录可整体复制给其他工具，其中参考文档和模板随技能携带；源码链接需要匹配
版本的 Prism 仓库。新环境应按其技能发现机制放置目录。

使用示例：

```text
使用 $prism-app-ui，为 Prism 实现一个设备状态应用。
要求：图标主操作，CPU/内存分组，手动刷新，明暗与材质跟随会话主题；
适应 BSP 窄窗，先呈现 Preview 和主操作，再异步准备非首屏数据。
请先列出区域/binding/action/加载阶段/主题与 motion 依赖表，
并说明标题与正文字号、图标/命中尺寸、间距和窄窗策略。使用实际支持的 DSL 属性，
交付应用包、业务模块和验证证据。
```

规范适用于 Prism 第三方应用开发，不规定应用必须复制 Music 或 Preferences 的
业务页面。应用作者可自由组织内容，但共享接口、生命周期与主题边界必须一致。

全部项目文档见 [文档索引](README.md)。

## 6. 设计规范补充与验证（2026-10-04）

新增设计尺度与页面配方、动效与坑点参考，以及独立 compact-library.prism 视觉组件。
已校正主题 schema、组控制、命名 Transition 和图标目录中的过时描述；v19 与工作区
新增文本编辑能力分别说明。Skill 入口、文档索引和本机技能链接共同指向仓库维护版本。

`skill_visual_example_test` 使用实际 DejaVu 字体 shaping，对四材料 × 两配色 ×
三套 Motion × 三种 BSP 尺寸，共 72 种组合验证解析、Scene 构建、32×32 操作范围
及动作命中。测试通过，Skill frontmatter 校验、相对链接检查、代码规范和 diff 空白
检查通过。该测试不包含 GPU 像素截图、真实输入设备、业务 action 执行或部署验收。
证据为 `dist/validation/skill-ui-design-20261004/`。本轮未改正在运行的桌面或应用外观。
