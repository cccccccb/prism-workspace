# 第四步检查点：独立客户端 Scene

`prism_client_scene` 只链接 `prism_contracts`。`prism_client_dsl` 单独编译现有 lexer/parser，把一次性 AST 转为客户端 Blueprint；运行期 Scene 不持有 AST、WM、Wayland 或 ImGui 对象。UI 状态与布局均由客户端 Scene 保存。

目前支持 DSL 的 `HStack`、`VStack`、`Card`、`Text`、`Button`，以及 `width`、`height`、`font`、`spacing`、`padding`、`cornerRadius`、`clip` 的基础语义。`Button` 通过客户端局部坐标命中并返回 action。未支持的组件和修饰符会抛出错误；现有含 blur、Slider 等的完整演示样例尚不能由此模块运行。

`SetSlot` 更新绑定文字并标记 Layout/Paint；背景色变化只标记 Paint；viewport 变化标记 Layout/Paint。`Build` 在无变化时返回空，不产生新的提交。输出是进程内 DisplayList。文字 glyph id 与位置由调用方提供的 shaping 接口生成；当前没有生产字体 shaping 实现。固定尺寸与均分的 Row/Column、基础裁剪、圆角和文字命令已通过单元测试。

本机没有 Skia 开发库。此检查点未包含 Skia 绘制、图片资源、真实字体排版，也未连接第三步的 Wayland 客户端。不能据此宣称第四步完成，更不能推断 Pi 上的 GPU 性能。后续先接字体 shaping/资源表与 Skia DisplayList 后端，再连 Wayland surface 做单线程端到端验证，并在真实设备上比较 Vulkan 与 OpenGL ES。

验证：

```sh
cmake --build build -j4
ctest --test-dir build -R '^client_scene_test$' --output-on-failure
```
