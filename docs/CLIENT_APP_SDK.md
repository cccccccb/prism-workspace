# 客户端 SDK：DSL 到 Wayland 窗口

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；SDK 在客户端进程内持有 Scene、图片资源、字体整形、Skia Ganesh GLES、EGL 窗口和 Wayland 对象。WM 不接触 DSL、Scene 或应用 `$slot`。

应用调用 `Open` 完成一次解析与窗口创建；在事件循环中调用 `Pump`；业务状态变化调用带类型的 `SetBinding`，文字可用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。`Pump` 接收 Wayland 输入与 configure、轮询异步图片解码，资源就绪后标记 Scene 并请求绘制。静止页面不自行连续提交。`Close` 按 Skia → EGL → Wayland 的顺序释放图形对象。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

`tests/probes/skia_gles_wayland_probe.cpp` 已改为调用此 SDK，而不是在测试中拼装另一套渲染生命周期。Pi headless V3D 验收仍为 `configure=2 frame=2 presented=3 images=1/1`，完整 GLES 构建及 CTest 11/11 通过。

四个旧入口应用与统一 host 使用此入口；音乐 demo 已转为 host 加业务模块，其原可执行名只保留启动适配器。旧 `prism_sdk`/ImGui 生产路径已删除。SDK 仍使用 xdg-shell；WM 对自己启动的三个 shell 进程按凭据授予角色，普通 app_id 不授予权限。Slider、blur 等尚未实现，具体迁移范围见 [APPLICATION_MIGRATION.md](APPLICATION_MIGRATION.md)。空 socket 使用当前 `WAYLAND_DISPLAY`；`LoadUiSource` 提供源码目录与安装目录的模板查找。

## 统一运行时与启动目标

统一 host 已接管音乐 demo 前端生命周期，初始化分为 PrepareFrontend/Bind/configure 后 EGL；应用以 DSL/资源和版本化业务模块提供状态/动作。ClientApplication 支持同 surface 的 ReplaceUi，以及严格 assets 根解析；SetBinding 返回值表示值已被接受，重复相同值不会触发重绘。底层 Scene 的 SetBinding 返回值仍表示是否发生变更。实际 FirstPresented 使用 PresentationCount，兼容 PresentedCount 仍仅计 swap。实现与约束见 [APP_HOST_RUNTIME.md](APP_HOST_RUNTIME.md)。其余四个应用及 invoker/launcher 的待命池/生产切换在后续两步完成；目标及 fork 边界见 [LAUNCH_RUNTIME_RESTORATION_PLAN.md](LAUNCH_RUNTIME_RESTORATION_PLAN.md)。

第三步现已接入 `prism_launch_client` 与 host 模块 Launch API；launcher 通过同一 host 的待命进程承载应用、报告真实退出并补池。其余四应用/Shell/Dock/session 统一切换属于下一步。规范见 [LAUNCHER_WORKER_POOL.md](LAUNCHER_WORKER_POOL.md)。

## 2026-09-26 第四步更新

五个应用已统一迁到 host 模块/目录包；可信 WM 控制通道、Shell 一次性登记与 pidfd、真实 Activated 事件、Dock 映射实例订阅和 session supervisor 已接入。此前未切换描述为历史检查点。当前物理安装仍待第五步部署；细则见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。
