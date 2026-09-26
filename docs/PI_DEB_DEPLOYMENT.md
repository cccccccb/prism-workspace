# Pi Debian 安装包与物理显示会话

## 发布规范

- 发布构建固定 `PrismGLES`、`PRISM_ENABLE_GLES=ON`、`BUILD_TESTING=OFF`、`CMAKE_INSTALL_PREFIX=/usr`。CPU 诊断配置不能打桌面安装包。
- `tools/package-deb.sh [输出目录]` 默认写入 `dist/deb`；Skia 路径可用 `PRISM_SKIA_ROOT` 指定，构建并发可用 `PRISM_BUILD_JOBS` 指定。
- Skia 静态链接进入客户端；WM 只链接 wlroots 等合成依赖。动态库运行依赖由 `dpkg-shlibdeps` 生成，另声明 `fonts-dejavu-core` 和 Mesa 驱动依赖。
- 包包含 WM、五个客户端、DSL 模板、壁纸、会话入口及服务文件；不安装测试、probe、源码或 Skia 开发库。
- 保留的 `prism-compiler` / `prism-pack` 等工具属于旧 AOT 工具链，不参与新客户端会话。`prism-session` 不再启动 Zygote；WM 自己启动并授权 Shell 客户端，dock 直接启动应用。
- 安装包不会自动启动或启用任何图形会话。生成物放在被忽略的 `dist/` 中，打包源码和部署文档纳入 Git。

```sh
tools/package-deb.sh
sudo apt install ./dist/deb/prism-wm_0.1.0-1_arm64.deb
sudo systemctl daemon-reload
systemctl --user daemon-reload
```

## 启动入口

### 显示管理器或本地登录

登录界面选择 Prism，运行 `/usr/bin/prism-session`。无显示管理器时，在本地 VT 登录后运行 `prism-session --drm`。需要有效、归当前用户所有且权限为 0700 的 `XDG_RUNTIME_DIR`；不创建 `/tmp` 替代目录，不以 root 运行桌面。

默认交由 wlroots 选择后端，默认合成器 renderer 为 `gles2`。`--drm` 显式使用 `drm,libinput` 并清除外部 DISPLAY/Wayland socket；`--nested` 使用现有 Wayland/X11 桌面。客户端始终走 Skia GLES，不在此切换 CPU UI。

Pi 的 VC4 显示设备与 V3D 渲染设备分离，参见 [Mesa V3D 文档](https://docs.mesa3d.org/drivers/v3d.html)。本机 DRM 自动选择下，WM 已报告 V3D，但客户端探针实际退回 llvmpipe；显式选择 V3D render node 后探针报告 V3D。会话入口因此仅在物理启动且没有用户指定 `WLR_RENDER_DRM_DEVICE` 时，从 sysfs 查找 v3d 驱动对应的 render node，不写死 `renderD128`，不改变 nested/headless 配置。此修正用于 GPU 设备选择，不构成零拷贝验证。

`prism-session` 使用 `exec` 运行同目录下 WM，WM 的失败退出码及信号直接传给服务管理器。WM 创建自己的 Wayland socket 后启动三个 Shell，避免额外 supervisor/readiness/Zygote 启动链。

### 用户服务

`prism-session.service` 是手动启动入口，没有全局自启动依赖；避免每个桌面登录都另起一个 Prism。可用 `~/.config/prism/session.env` 设置 wlroots 环境变量。用户服务需要自身拥有可访问的显示/seat；仅在 SSH 中执行 `systemctl --user start` 不会自动获得本地 DRM seat。

不要在显示管理器已经启动 Prism 时再启动用户服务，两者使用同一个 Wayland socket。

### 从 SSH 启动物理显示演示

安装包提供手动的系统服务模板 `prism-demo@.service`。它通过 `PAMName=login` 给指定普通用户创建 tty8/seat0 登录会话，用 logind 获取 DRM/input 设备；图形进程依然以该用户运行。此模板占用固定 tty8，仅允许一个用户实例运行。

当前机器没有其他图形桌面，tty8 可用于演示。其他机器先检查 tty8 与 seat0 上现有会话；切换 VT 会切换本地显示。

```sh
sudo systemctl start prism-demo@ss.service
sudo chvt 8
sudo journalctl -t prism-demo-ss -b -f
```

停止演示并回到控制台（若启动了本轮两个独立 demo 服务，先停止它们）：

```sh
systemctl --user stop prism-demo-player.service prism-demo-settings.service
sudo systemctl stop prism-demo@ss.service
sudo chvt 1
```

不需要 `enable`，不更改默认启动目标。WM 在正常退出时停止三个 Shell 客户端。PAM 将桌面进程放入 logind session scope，不能仅靠原 service 的 `KillMode` 声称覆盖所有应用；彻底结束该 seat 会话可先用 `loginctl list-sessions` 找到 tty8 的 session ID，再执行 `loginctl terminate-session ID`。独立用户服务中启动的 demo 则单独停止其服务。

独立启动演示应用时显式连接 Prism：

```sh
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-prism-0 demo_player
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-prism-0 demo_settings
```

以上命令以用户 ss 运行。也可通过 dock 点击对应应用入口。

## 验收范围

先确认安装资源能脱离源码目录加载、真实 HDMI output 启用、Shell 与 demo surface 映射、客户端 GL renderer 为 V3D。headless 安装探针继续位于 `tests/probes/`，不进入包。

本轮属于迁移计划第六步的安装与物理会话检查点，不代表已经完成性能验收。功能缺口仍以 [APPLICATION_MIGRATION.md](APPLICATION_MIGRATION.md) 为准；p50/p95/p99、输入延迟、温度/降频等还需后续独立测量。

## 2026-09-26 本机安装与实机检查记录

- 发布包：`dist/deb/prism-wm_0.1.0-1_arm64.deb`，约 29 MiB；SHA-256 文件保存在同目录。已安装到本机，`dpkg-query` 为 `install ok installed 0.1.0-1 arm64`，`dpkg -V prism-wm` 无差异。
- 包内容检查通过，不含测试/probe/ImGui，不使用 `/usr/local`。发布配置关闭 `BUILD_TESTING`，打包门槛拒绝无客户端和错误安装前缀的配置。
- Bash 语法、systemd 单元校验通过。安装后的五客户端 headless 检查通过，伪造 topbar app_id 保持普通窗口权限；探针输出 `V3D 4.2.14.0`、`configure=3 frame=2 presented=4 images=1/1`。
- 真正物理显示路径：tty8 / seat0 / logind，`HDMI-A-1` 首选模式为 1024×600、59.821 Hz。最终会话没有临时 systemd drop-in；由安装的脚本自动选择本机 V3D render node。
- 物理会话客户端探针通过：`GL renderer=V3D 4.2.14.0`、`configure=2 frame=2 presented=3 images=1/1`。探针仍从源码树中的诊断构建执行，未安装到系统。
- 修复初始 modeset 后立即提交造成的 pending page-flip：由 `wlr_output_schedule_frame()` 调度首帧。最终启动日志 `Native GPU Frame 1 ... commit=OK`，检查期间没有 `ERROR` 或 `commit=FAILED`。
- 最终日志确认 desktop/topbar/dock、音乐和设置五个生产 surface 映射，六个生产进程均以用户 ss 运行。WM 服务为 `prism-demo@ss.service`；两个示例由用户 transient 服务 `prism-demo-player.service` / `prism-demo-settings.service` 运行，工作目录为 `/`，验证安装资源无需源码目录。
- 当前留下实机会话供现场查看；未开启开机自启。远程日志确认映射和提交，显示器上的实际视觉效果及物理鼠标/键盘交互仍需现场确认；本轮不声称已完成完整性能验收或零拷贝验收。

本轮独立示例服务的可重复启动命令（结束已有同名服务后再执行）：

```sh
systemd-run --user --collect --unit=prism-demo-player \
  --property=WorkingDirectory=/ --setenv=WAYLAND_DISPLAY=wayland-prism-0 \
  /usr/bin/demo_player
systemd-run --user --collect --unit=prism-demo-settings \
  --property=WorkingDirectory=/ --setenv=WAYLAND_DISPLAY=wayland-prism-0 \
  /usr/bin/demo_settings
```
