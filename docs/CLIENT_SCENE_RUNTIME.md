# 第四步检查点：独立客户端 Scene

`prism_client_scene` 只链接 `prism_contracts`。`prism_client_dsl` 单独编译现有 lexer/parser，把一次性 AST 转为客户端 Blueprint；运行期 Scene 不持有 AST、WM、Wayland 或 ImGui 对象。UI 状态与布局均由客户端 Scene 保存。

目前支持 DSL 的 `HStack`、`VStack`、`Card`、`Text`、`Button`，以及 `width`、`height`、`font`、`spacing`、`padding`、`cornerRadius`、`clip` 的基础语义。`Button` 通过客户端局部坐标命中并返回 action。未支持的组件和修饰符会抛出错误；现有含 blur、Slider 等的完整演示样例尚不能由此模块运行。

`SetSlot` 是带类型的 `SetBinding` 的字符串便捷入口；Scene 的属性存储依据 schema 元数据决定 Layout/Paint dirty，背景色变化只标记 Paint，viewport 变化标记 Layout/Paint。`Build` 在无变化时返回空，不产生新的提交。输出是进程内 DisplayList。文字 glyph id 与位置由调用方提供的 shaping 接口生成；Skia 后端现提供 HarfBuzz + FreeType 实现。固定尺寸与均分的 Row/Column、基础裁剪、圆角和文字命令已通过单元测试。

此文件记录 Scene 首个检查点。后续已加入 Skia CPU 诊断后端、HarfBuzz 字体排版和真实 Wayland 测试窗口，见 [SKIA_BACKEND_PI.md](SKIA_BACKEND_PI.md)。PNG 图片资源的首个异步检查点见 [IMAGE_RESOURCES.md](IMAGE_RESOURCES.md)；GLES GPU 后端及复用客户端入口见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md) 和 [CLIENT_APP_SDK.md](CLIENT_APP_SDK.md)。

验证：

```sh
cmake --build build -j4
ctest --test-dir build -R '^client_scene_test$' --output-on-failure
```
