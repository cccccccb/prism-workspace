# 客户端 SDK：DSL 到 Wayland 窗口

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；SDK 在客户端进程内持有 Scene、图片资源、字体整形、Skia Ganesh GLES、EGL 窗口和 Wayland 对象。WM 不接触 DSL、Scene 或应用 `$slot`。

应用调用 `Open` 完成一次解析与窗口创建；在事件循环中调用 `Pump`；业务状态变化调用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。`Pump` 接收 Wayland 输入与 configure、轮询异步图片解码，资源就绪后标记 Scene 并请求绘制。静止页面不自行连续提交。`Close` 按 Skia → EGL → Wayland 的顺序释放图形对象。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

`tests/probes/skia_gles_wayland_probe.cpp` 已改为调用此 SDK，而不是在测试中拼装另一套渲染生命周期。Pi headless V3D 验收仍为 `configure=2 frame=2 presented=3 images=1/1`，完整 GLES 构建及 CTest 11/11 通过。

当前入口只支持普通 xdg-shell toplevel；topbar、dock、desktop 需要受授权的 shell 角色，不能用普通 app_id 冒充。现有业务 DSL 中的 Slider、blur 等尚未被新 Scene 实现。迁移这些应用前需补齐它们实际使用的语义，再让应用调用同一 SDK；最终切换时移除旧 `prism_sdk`/ImGui 生产路径。
