# 客户端 SDK：DSL 到 Wayland 窗口

日期：2026-09-26。当前运行时主题接口见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)；本轮测试与部署结果另行记录。

`prism_client_app` 是新架构普通应用的复用入口。调用方给出 DSL 源码、Wayland socket、应用 ID、标题、字体与初始尺寸；SDK 在客户端进程内持有 Scene、图片资源、字体整形、Skia Ganesh GLES、EGL 窗口和 Wayland 对象。WM 不接触 DSL、Scene 或应用 `$slot`。

平台 host 在 `Open` 前调用 `ApplyTheme` 安装初始快照，随后解析 UI 并创建窗口；在事件循环中调用 `Pump`。业务状态通过带类型的 `SetBinding` 更新，文字可用 `SetSlot`；按钮动作由 `OnAction` 回调交给业务。`Pump` 接收 Wayland 输入与 configure、轮询异步图片解码，资源就绪后标记 Scene 并请求绘制。静止页面不自行连续提交。`Close` 按 Skia → EGL → Wayland 的顺序释放图形对象。接口见 [client_application.hpp](../prism/include/prism/sdk/client_application.hpp)。

`tests/probes/skia_gles_wayland_probe.cpp` 使用同一 SDK 生命周期，测试和诊断程序留在 tests，不安装到生产包。早期 GLES 检查点的计数与验收记录见 [SKIA_GLES_PI.md](SKIA_GLES_PI.md)，不作为当前主题功能的验证结论。

五个生产应用均由统一 host 加业务模块/DSL 包运行，旧独立入口、音乐 exec 适配器和 ImGui 生产路径已删除。SDK 使用 xdg-shell；Shell 角色由 launcher 与 WM 的可信控制通道按真实进程登记，普通 app_id 不授予权限。背景 blur 通过通用 surface effects 契约请求 compositor；SDK 绘制 tint 与客户端内容。Slider 拖动语义尚未实现。空 socket 使用当前 `WAYLAND_DISPLAY`；包入口使用严格 assets 根解析，`LoadUiSource` 的模板查找只服务直接 SDK 调用场景。

## 统一运行时与启动目标

统一 host 的初始化分为 PrepareFrontend、初始主题安装、Bind 与 configure 后 EGL。应用以 DSL/资源和版本化业务模块提供状态/动作。ClientApplication 支持同 surface 的 ReplaceUi 和严格 assets 根解析。SetBinding 返回值表示值已被接受，重复相同值不触发重绘；底层 Scene 的 SetBinding 返回值仍表示是否发生变更。实际 FirstPresented 使用 PresentationCount，兼容 PresentedCount 仍仅计 swap。实现与约束见 [APP_HOST_RUNTIME.md](APP_HOST_RUNTIME.md)。

`prism_launch_client` 与 host 模块 Launch API 使用同一个 launcher。待命 worker 已完成公共 CPU 前端准备，收到并确认当前 ThemeSnapshot 后才分配应用；EGL/GPU 仍在绑定窗口后初始化。池规范见 [LAUNCHER_WORKER_POOL.md](LAUNCHER_WORKER_POOL.md)，生产会话与授权规则见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。

## 运行时主题

`ApplyTheme(const ThemeSnapshot&, std::string* diagnostic = nullptr)` 可在 Open 前安装初始主题，也可更新当前 Scene。SDK 保留当前快照供后续 Preview/Master 的 ReplaceUi 使用；切换主题本身不调用 ReplaceUi、不重建 Scene、不关闭 surface/EGL 或业务模块。`ThemeGeneration()` 返回已接受快照的 generation。

返回 true 表示已接受，包括相同合法快照；返回 false 时，旧主题和 Scene 保持。Scene 会先验证引用、布局、效果区域与输入轮廓，再更新节点样式并请求重绘。`@token` 是保留的主题引用，`material` 选择通用材料，`inputShape` 明确控制透明材料的输入范围。接口与优先级见 [CLIENT_SCENE_RUNTIME.md](CLIENT_SCENE_RUNTIME.md)。

下一次呈现将同一 Scene 的 DisplayList、SurfaceEffects 和 InputRegions 应用于该 surface，WM 不接收 UI 语法或业务 binding。全局主题包由 launcher 编译与分发，SDK 不读取第二份 JSON 或静态样式头；包、ACK、失败恢复和跨进程呈现边界见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)。应用模块仅通过可选 `select_theme` 请求包 ID，并用 `on_theme_event` 接收实际结果。

0.1.0-5 的物理 Pi 外观与输入验收属于此前静态视觉版本，见 [PI_DEB_DEPLOYMENT.md](PI_DEB_DEPLOYMENT.md)；本轮运行时主题的测试与实机结果不能由该记录替代。
