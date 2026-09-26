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

## 第二段当前结果

Blueprint 现在携带 `PropertyAssignment` 与 `PropertyBinding`，每个赋值是明确的 `DslProperty` 和带类型的 `PropertyValue`；`$slot` 在语义编译时记录依赖目标，不再仅保存在 Text 节点的字符串字段中。组件默认值在 schema 中展开，Button 的文字属性转移给其文字子节点。

Scene 持有每个节点的属性存储与绑定索引，`SetProperty`、`SetBinding` 是统一更新入口；`SetSlot` 和 `SetBackground` 仅保留便捷调用。schema 同时声明 DSL 输入类型、运行期类型、数值范围及 dirty 影响。例如颜色变化只标记 Paint，文字和字号变化标记 Layout/Paint，动作字符串变化不要求重绘。错误类型、非法数值、相同值不会改变 Scene。现有 Style、文字和图片字段是属性存储派生的布局/绘制缓存。

这一步仍以整棵 Scene 重新布局或生成 DisplayList，尚未做节点级 dirty 传播、布局边界或 Render Tree；这些属于第三段。图片资源就绪事件单独处理其固有尺寸变化。

Pi 上 CPU 和 GLES 配置分别完整构建，CTest 各 12/12 通过；通过新属性存储和绑定索引的 headless V3D 客户端仍报告 `frame=2 presented=3 images=1/1`。

## 第三段当前结果

Scene 的 `Build` 现在把稳定 NodeId、属性派生状态和上次几何复制为 `SceneSnapshot`。`LayoutEngine` 单独计算布局与文字 shaping；Scene 只在 Layout dirty 时调用它，并将结果写回用于命中。`RenderTreeBuilder` 从 snapshot 构造带来源版本、几何、裁剪及视觉图元的 Render Tree，复用来源与几何均未变化的节点记录。`DisplayListBuilder` 单独遍历 Render Tree 生成命令；Scene 不再直接生成绘制命令。

当前复用是结构边界，尚不是完整增量渲染：每次需要提交时仍复制整棵 snapshot、构造 Render Tree 并生成整张 DisplayList；节点级 dirty 传播、布局边界、DisplayList 分块缓存和遮挡裁剪需要在 Pi 实测后逐步加入。仅改绘制属性时跳过布局和 shaping，动作变化仅更新命中状态而不提交。新增独立 Render Tree 与 DisplayList 单元测试，验证命令顺序和节点版本复用。

Pi 上 CPU 与 GLES 配置均完整构建、CTest 各 13/13 通过；headless V3D 客户端探针报告 `GL renderer=V3D 4.2.14.0`、`configure=2 frame=2 presented=3 images=1/1`。

## 第四段当前结果

[DisplayList 绘制契约](DISPLAY_LIST_CONTRACT.md)明确了进程内生命周期、坐标、颜色、绘制顺序、裁剪/变换栈以及资源就绪要求。Skia 共享回放器按 `ResourceId` 查找字体与图片；文字 shaping 可指定同一个字体 ID，删除了仅接受字体 ID 1 的约束。CPU 与 GLES 仍复用同一套命令校验和回放代码，GLES 仅负责 GPU 画布与提交。新增非默认字体、缺失字体和缺失图片测试。

Pi 上 CPU 配置 CTest 13/13，GLES 配置 CTest 14/14；GLES 专属离屏测试用同一列表对比 CPU 与 GPU 回读的纯色矩形、裁剪内部像素。headless Wayland V3D 探针仍报告 `GL renderer=V3D 4.2.14.0`、`configure=2 frame=2 presented=3 images=1/1`。此像素测试只覆盖纯色和裁剪；文字、图片的逐像素容差仍待后续扩展。

## 第五段当前结果

五个应用入口和 UI 模板已统一使用新客户端 SDK；ImGui 源码与链接、旧共享内存 UI SDK、WM 内 shell 专用绘制、控件 Scene 镜像和 slot 差分回放已删除。shell 由 WM 启动，按 Wayland 进程凭据授予固定层级，普通客户端不能通过 app_id 冒充。WM target 的源文件依赖检查已经启用。具体实现、功能差距和验证方式见 [APPLICATION_MIGRATION.md](APPLICATION_MIGRATION.md)。

本段完成的是生产绘制路径切换；原 demo 的 Slider、blur、图标、壁纸轮换和完整窗口管理联动尚未迁入。后续先在新架构中补齐实际交互与真实 surface 的窗口管理，再考虑增量布局和分块缓存。
