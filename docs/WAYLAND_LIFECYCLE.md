# 第三步进度：真实 xdg-shell 窗口

本分支已实现一个不读取 DSL 的 Wayland 客户端平台模块 `prism_wayland_client`。它独立于旧 `prism_core` 和 ImGui：连接 registry、绑定 compositor/xdg_wm_base/shm/seat、创建 `wl_surface` 与 xdg_toplevel、执行首次空提交、处理 configure/ack、提交 SHM buffer、等待 frame callback 与 buffer release，并处理 resize、close、seat capability 变化、指针与键盘事件。绘制内容由调用方提供；平台模块不解释 DSL，也不绘制业务 UI。

WM 现在监听 wlroots 的 `new_toplevel`，为真实客户端建立独立记录；在 map/unmap/destroy 时维护 scene 节点与焦点。WM 使用 scene 命中测试把指针事件发送到 surface 的局部坐标，把键盘事件送到已聚焦 surface。普通窗口会收到分屏几何的 xdg configure；最大化请求会收到输出可用区域大小。关闭请求通过 xdg_toplevel.close 发送。

## 验证

```sh
cmake --build build -j2
ctest --test-dir build -R '^wayland_lifecycle_test$' --output-on-failure
```

集成测试在 Pi 的 headless + pixman 后端上运行，不需要显示器：验证首次 configure、首次 buffer、frame callback、最大化 resize、指针进入和点击、测试键盘的按下/释放、客户端收到 close、unmap/destroy，以及两个独立客户端收到 640×614 的分屏尺寸。测试窗口只提交纯色诊断像素，未引入 Skia。

手动检查可先运行：

```sh
WLR_BACKENDS=headless WLR_RENDERER=pixman ./build/prism/prism-wm
```

再在另一个终端运行：

```sh
./build/tests/wayland_lifecycle_probe wayland-prism-0
```

看到 `configure`、`buffer commit` 和 `frame_done` 计数表示真实客户端已映射并收到帧回调。此 probe 会请求最大化并自行退出。

## 尚需完成

- 现有 `prism_sdk::Application` 仍连接旧共享内存通道并持有旧 Scene/FrameBuffer；真实 Wayland 平台模块尚未替换该生产入口。切换须与第四、五步的 DSL + Skia 客户端路径一起完成，避免发布两套 UI 路径。
- Desktop/TopBar/Dock 的 shell surface 角色、锚点、独占区域及可信授权尚未实现；不能把当前普通 xdg_toplevel 冒充 shell 角色。
- 当前诊断 buffer 使用 scale 1 的 `wl_shm`；fractional scale、GPU buffer 提交和实际 DRM/KMS 显示输出未验证。
- 键盘事件已送达客户端；平台层目前只规范化常用物理键为 USB HID usage，完整键位、文本输入和输入法需要后续补齐。

这些限制与下一阶段的模块边界以 [PROCESS_CONTRACTS.md](PROCESS_CONTRACTS.md) 为准。
