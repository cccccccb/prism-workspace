# DSL 到 DisplayList 的重写顺序

本计划落实[总体架构文档](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md)中的 AST → Scene → Render Tree → DisplayList 边界。迁移分支每段保留可运行的客户端链路；发布时只保留新 SDK 与 Skia 生产 UI 路径。

1. **通用语法与语义规范。** Parser 只产出带源码位置的调用、具名/位置参数、列表、修饰符、绑定和值；不认识组件名。语义层的组件 schema 声明属性类型、默认值、子节点规则、dirty 影响及组件展开。验收：未知组件/属性、重复参数、类型错误和非法值给出位置明确的错误；现有客户端样例含义不变。旧二进制包 parser 在应用切换前暂留旧路径，不作为新 SDK 的第二套解析器。
2. **带类型的 Blueprint 与属性存储。** 语义层将语法 AST 编译为基础视觉节点和带类型的属性，保留绑定表达式；运行期属性更新按 metadata 标记 Style、Layout、Text、Paint、Composite，避免 `SetSlot` 针对某个控件硬编码。验收：普通属性或组件可在语义层加入，无须改语法 parser 或 Skia。
3. **拆分 Scene、布局和 Render Tree。** Scene 保留稳定 NodeId、状态与事件；独立布局模块计算几何，RenderTreeBuilder 从 Scene snapshot 生成视觉树并复用未变节点。DisplayListBuilder 单独从 Render Tree 生成命令。验收：Scene 不直接写 `DrawCommand`，静止页面无提交，布局与命中结果保持一致。
4. **收紧绘制契约与资源解析。** DisplayList 明确颜色/坐标/裁剪/资源语义；Skia 回放器只认识命令和资源表，删除固定字体 ID 等假设。CPU 与 GLES 对同一列表得到一致的结构和可接受像素差异。验证代码仅在 `tests/`。
5. **应用迁移与旧路径切换。** 先普通 demo，再具备授权 shell 角色的 desktop、topbar、dock；补齐它们实际需要的属性和效果。最后一次性移除旧 WM 应用渲染、旧 `prism_sdk` UI 路径与 ImGui 链接，验证生产产物只有新路径。

每段只增加当前样例需要的语义，先保证分层和错误处理正确，再根据 Pi 真机测量决定缓存、增量布局与渲染线程优化。

## 第一段当前结果

新客户端现使用 `dsl_syntax` 解析通用调用、具名/位置参数、字符串、数字、布尔、颜色、绑定、列表及修饰符，语法树不保存组件枚举。`dsl_schema` 声明目前已支持的组件、属性类型、允许范围及 dirty 影响；`dsl_frontend` 只做语义编译和 Blueprint 构造。新目标不再编译旧 `parser.cpp`，其保留仅为旧生产路径服务。非法字符、颜色、未知组件/属性、重复属性、错误类型和不允许的子节点均明确报错并附行号。

此阶段的 metadata 已建立，但运行期属性存储和 dirty 传播仍沿用现有 Scene；第二段才把这些规则真正用于状态更新。`Button` 的视觉展开目前仍在语义编译器中，第三段会进入组件构造/Render Tree。旧二进制包格式不属于新语法树，应用切换前暂不修改。

兼容检查发现容器默认 `spacing=8` 曾隐含在旧 parser 中，现已明确放进组件 schema。Pi 上 CPU 与 GLES 配置均完整构建、CTest 12/12 通过；新 parser 驱动的 headless V3D 客户端探针通过，报告 `frame=2 presented=3 images=1/1`。
