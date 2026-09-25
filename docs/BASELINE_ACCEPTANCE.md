# 第一步验收基线：现有 DSL 与窗口行为

本文件固定迁移前的观察对象，供客户端 DSL + Skia 路径逐项对照。哈希清单在 [baseline.sha256](baseline.sha256)；需要有意修改样例时，一并更新清单与此处的行为说明。PNG 是旧软件绘制路径的视觉参照，不是实际 Wayland 输出截图，也不用于像素级回归或 Skia 性能判断。

## 固定样例

| 类别 | DSL 源文件 | 需要保留的可见行为 |
| --- | --- | --- |
| 普通应用：播放器 | `demos/demo_player/preview.prism`、`master.prism` | Preview 过渡到 Master；曲名与播放按钮状态可更新，播放按钮发送 `player:toggle`。 |
| 普通应用：设置 | `demos/demo_settings/preview.prism`、`master.prism` | Preview 过渡到 Master；主题按钮发送 `theme:toggle`。 |
| Desktop | `prism-desktop/ui/desktop.prism` | 桌面层显示壁纸信息；`wallpaper_title` 随切换更新。 |
| TopBar | `prism-topbar/ui/topbar.prism` | 顶栏固定在上缘；时钟、网络、电量状态更新；按钮保留 `launcher:toggle`、`notifications:toggle` 动作。 |
| Dock | `prism-dock/ui/dock.prism` | Dock 固定在下缘；运行数量通过 `running_badge` 更新；应用按钮发送 `app:launch:*`。 |

当前 WM 预建 Desktop、TopBar、Dock 的窗口与 IPC 通道；普通应用的示例窗口是 WM 演示模式创建的。真实 xdg-shell surface 目前只直接附着在 scene tree，尚未完整纳入窗口记录、configure、焦点与输入命中管理。这是第三步的待完成事项，不能把现有演示行为误认为 Wayland 客户端生命周期已完成。

## 视觉与交互参照

已有 `snapshots/frame_00_preview.png`、`frame_05_master_split50.png`、`frame_08_divider_drag.png`、`frame_11_mission_control.png`、`frame_14_master_split65.png`、`frame_15_master_settled.png` 记录 Preview、分屏、拖动与 Mission Control 的旧软件渲染状态。`snapshots/shell_experimental_preview.png` 记录 shell 外观。迁移验收时比较层级、内容、动作与布局语义；字体光栅化和 GPU 合成像素可以不同。

## 当前可重复检查

在仓库根目录、完成 [Pi 构建](PI_BUILD.md)后执行：

```sh
sha256sum -c docs/baseline.sha256
ctest --test-dir build --output-on-failure
for app in prism-desktop prism-topbar prism-dock; do
  ./build/$app/$app --test
done
```

再将上表 7 个 DSL 文件分别传给 `./build/prism/prism-compiler <input.prism> <output.prismb>`，输出放在临时目录。本机 2026-09-25 验证结果：7/7 编译成功、6/6 CTest 成功、3/3 shell 离线测试成功。测试环境为 Pi 4B ARM64、GCC 14.2.0、CMake 3.31.6、Wayland 1.24.0、wlroots 0.18.2、pixman 0.46.4。

headless compositor 已验证 socket 创建、输出帧提交与正常退出；物理显示器上的 DRM/KMS、真实输入设备和客户端 surface 交互仍需后续会话验收。性能数字与测量边界见 [SKIA_MIGRATION_PLAN.md](SKIA_MIGRATION_PLAN.md)。
