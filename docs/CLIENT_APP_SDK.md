# 客户端 SDK：DSL 到 Wayland 窗口

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；SDK 在客户端进程内持有 Scene、图片资源、字体整形、Skia Ganesh GLES、EGL 窗口和 Wayland 对象。WM 不接触 DSL、Scene 或应用 `$slot`。

应用调用 `Open` 完成一次解析与窗口创建；在事件循环中调用 `Pump`；业务状态变化调用带类型的 `SetBinding`，文字可用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。`Pump` 接收 Wayland 输入与 configure、轮询异步图片解码，资源就绪后标记 Scene 并请求绘制。静止页面不自行连续提交。`Close` 按 Skia → EGL → Wayland 的顺序释放图形对象。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

`tests/probes/skia_gles_wayland_probe.cpp` 已改为调用此 SDK，而不是在测试中拼装另一套渲染生命周期。Pi headless V3D 验收仍为 `configure=2 frame=2 presented=3 images=1/1`，完整 GLES 构建及 CTest 11/11 通过。

五个生产应用现使用此入口，旧 `prism_sdk`/ImGui 生产路径已删除。SDK 仍使用 xdg-shell；WM 对自己启动的三个 shell 进程按凭据授予角色，普通 app_id 不授予权限。Slider、blur 等尚未实现，具体迁移范围见 [APPLICATION_MIGRATION.md](APPLICATION_MIGRATION.md)。空 socket 使用当前 `WAYLAND_DISPLAY`；`LoadUiSource` 提供源码目录与安装目录的模板查找。

## 统一运行时与启动目标

当前逐应用二进制构造 ClientApplication 的方式为迁移检查点。下一主线将 SDK 初始化拆分为可控阶段，统一 host 管理前端生命周期，应用以 DSL/资源和版本化业务模块提供状态/动作；invoker/launcher 管理预热 worker 与实例。目标及 fork 边界见 [LAUNCH_RUNTIME_RESTORATION_PLAN.md](LAUNCH_RUNTIME_RESTORATION_PLAN.md)，目前尚未实现。
