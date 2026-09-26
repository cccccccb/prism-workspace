# Pi Debian 安装包与物理显示会话

日期：2026-09-26。当前安装版本为 **0.1.0-5 arm64**，已接入真实 BSP、图标界面和 GPU 玻璃主题，统一 session/launcher/host 保持生产入口。1024×600 物理输出截图与协议检查通过；用户确认新版外观、播放图标、Preferences 配色切换和 Dock 激活均正常。

## 1. 发布规范

- `tools/package-deb.sh [输出目录]` 默认写入 `dist/deb`，固定 Release、PrismGLES、PRISM_ENABLE_GLES=ON、BUILD_TESTING=OFF、安装前缀 `/usr`。Skia 路径可用 PRISM_SKIA_ROOT 指定，并发可用 PRISM_BUILD_JOBS 指定。
- Skia 公共前端只进入 `prism-app-host`，五个应用发布为业务模块、manifest、DSL、资产目录包；模块只依赖契约，不维护 Wayland/EGL/绘制循环。WM 不链接前端渲染引擎。
- 九个平台入口为 prism-wm、prism-session、prism-session-runtime、prism-launcher、prism-invoker、prism-app-host、prism-msg、prism-pack、prism-compiler；后两个旧 AOT 工具不参与生产会话。
- 五个包位于 `/usr/share/prism/apps/`。不安装旧五客户端可执行入口，也不安装 tests、fixtures、probes、源码、ImGui 或 Skia 开发库。
- 动态运行依赖由 dpkg-shlibdeps 生成，另声明 fonts-dejavu-core 和 Mesa 驱动。安装不会自动启动或启用图形会话。生成物位于忽略目录 dist，打包源码和规范纳入 Git。

```sh
tools/package-deb.sh
sudo apt install ./dist/deb/prism-wm_0.1.0-5_arm64.deb
sudo systemctl daemon-reload
systemctl --user daemon-reload
```

升级之前应停止已有 Prism 会话。由 0.1.0-1 升级时，还要停止其独立的 prism-demo-player.service / prism-demo-settings.service；新版本不再创建这些服务。

## 2. 统一启动与设备准备

唯一生产链为 `prism-session → prism-session-runtime → WM + launcher → prism-app-host`，业务模块在 host 内加载。WM 通过私有可信通道授权 Shell；Dock 经 host Launch API 请求 launcher，不自行派生应用。

显示管理器选择 Prism；本地 VT 可运行 `prism-session --drm`；嵌套会话使用 `--nested`。XDG_RUNTIME_DIR 必须归运行用户所有且权限为 0700，不使用 /tmp 替代目录、不以 root 运行桌面。用户服务 prism-session.service 仍需有效本地 seat，SSH 用户服务不会自动获得 DRM 权限。

Pi 会话在用户未指定 WLR_RENDER_DRM_DEVICE 时，通过 sysfs 的 v3d 驱动查找 render node，不写死 renderD128；默认合成器 renderer 为 gles2。VC4 显示设备和 V3D 渲染设备分离，设备选择不能用来证明零拷贝。脚本 exec 同目录下的 supervisor，服务沿用原 ExecStart，无需额外 launcher 服务。

## 3. 从 SSH 运行物理会话

手动系统模板 prism-demo@.service 使用 PAMName=login，在 tty8/seat0 为普通用户建立 logind 会话，图形进程以该用户运行。固定 tty8 仅允许一个实例；不与已经运行的 Prism 显示管理器会话同时启动。

```sh
sudo systemctl start prism-demo@ss.service
sudo chvt 8
sudo journalctl -t prism-demo-ss -b -f
```

以用户 ss 启动或激活已安装应用；无需独立 demo 服务，也无需指定应用自己的 Wayland socket：

```sh
XDG_RUNTIME_DIR=/run/user/1000 prism-invoker demo_player
XDG_RUNTIME_DIR=/run/user/1000 prism-invoker demo_settings
```

Dock 的 Music/Pref 使用同一 API，默认激活已有实例。显式 NewInstance 由启动 API 提供。

```sh
sudo systemctl stop prism-demo@ss.service
sudo chvt 1
```

不 enable 服务、不改变默认启动目标。Supervisor 回收 WM、launcher 及接管的 host；不能仅依赖 service KillMode，因为 PAM 进程可能进入 logind scope。启动前已经存在的 systemd PAM helper 归 systemd 管理，监督器不得等待它退出：helper 本身等待父监督器退出，否则会形成停止死锁。0.1.0-2 实机测试发现这一问题，0.1.0-3 修复，0.1.0-4 延续该修复；试验版 2 不作为当前部署版本。

## 4. 0.1.0-5 BSP 与玻璃主题部署

- 发布包 `dist/deb/prism-wm_0.1.0-5_arm64.deb`，7259770 字节，SHA-256：`f731e8d0b8cb716c7cc8551265a721871ed4008ca639435a5620769f5144e3a9`。安装为 `install ok installed 0.1.0-5 arm64`，`dpkg -V` 无差异。
- Release 包清单检查通过：统一 host/模块/DSL/资产及共享主题 JSON，包含新材料协议运行代码；不含旧客户端入口、测试、probe、fixture 或 ImGui。解包前缀的真实统一会话与预存 PAM helper 回收检查通过。
- 最终 CTest 26/26 通过，包含无显式 window geometry 的真实客户端 BSP、提交尺寸/位置、嵌套分割、工作区与全屏，及透明/圆角/图标/局部阴影/裁剪材料契约。真实 host 双窗、三窗、四窗截图已保存，不能仅以模拟树代替真实客户端布局。
- V3D 背景回归的五个独立会话、40 次捕获通过；高频下层跨度 191、玻璃跨度 4，下层红/蓝变化透过玻璃产生均值差变化 23.5。前期捕获曾出现间歇性 8 秒超时，原因未定位，后续成功样本不视为修复；probe 保留超时诊断并失败，不自动重试。物理会话截图此次成功。
- 旧会话停止约 0.417 秒，九个旧 Prism 进程与 socket 均回收；安装新包后重新启动 `prism-demo@ss.service`。当前 tty8/seat0、HDMI-A-1 为 1024×600、59.821 Hz；五个应用真实 FirstPresented 均为 `GL renderer=V3D 4.2.14.0`，WM 的 GPU backdrop capability=1。新会话日志无 ERROR 或 commit=FAILED，HUD 关闭。
- 实机协议门槛再次通过：真实呈现/Ready、同实例激活、NewInstance、取消退出、保留 Shell 拒绝及实例快照。最终保留 Music/Preferences 两个普通 BSP 窗口和 Shell 供操作，没有启用开机自启。
- 实际输出截图为 `dist/validation/prism-v5-physical.png`，操作后截图为 `prism-v5-physical-confirmed.png`。版本、包、输出、树、状态、停止与启动记录均以 `prism-v5-*` 保存。用户答复“外观正常，以上点击都正常”，确认播放状态变化、Preferences 本窗口变色和 Dock Music/Pref 激活；结果记录在 `prism-v5-physical-final.json`。全部键盘快捷键尚未逐项现场复核，不把截图或 59.8 Hz 输出模式当作完整性能验收。

主题及职责规范见 [SURFACE_MATERIALS.md](SURFACE_MATERIALS.md)，快捷键与本轮边界见 [VISUAL_TILING_REFINEMENT_PLAN.md](VISUAL_TILING_REFINEMENT_PLAN.md)。Settings 现在可切换本窗口真实配色，全局主题服务未实现；Music 仍为演示业务。实时拖动分割比例、动画与性能专项留待后续。

## 5. 历史：0.1.0-3 实机记录

- 发布文件 dist/deb/prism-wm_0.1.0-3_arm64.deb，约 6.9 MiB。SHA-256：`bd8337de74478f96f4dfe2c40fc104012a444089da594f90041f717784a65b94`，同目录保存校验文件。dpkg-query 为 install ok installed 0.1.0-3 arm64，dpkg -V 无差异。
- 包清单、Bash 语法与 systemd 单元检查通过。解包后的安装前缀启动正常 headless 和带预存 helper 的会话检查通过；测试未安装。
- 实际 tty8/seat0/DRM 输出 HDMI-A-1，1024×600，59.821 Hz；五个生产应用真实 FirstPresented 上报 `GL renderer=V3D 4.2.14.0`，来自各自 host 的实际上下文。日志中的 WM renderer 不能代替客户端证据。
- 实机协议检查通过创建、同 PID/instance 激活、NewInstance、取消后真实退出、实例快照与变化、公共保留 Shell 包拒绝；请求者断开后 Music/Settings 继续运行。验证入口在 tests/probes/physical_session_probe.py，不部署到 /usr。
- 一轮停止实测约 0.347 秒，service Result=success，九个 Prism 进程已回收，launcher 和 Wayland socket 清理完成；之后重新启动并留下 Music/Settings 供现场操作。预存 PAM helper 由 systemd 管理。
- 单次进程快照的九个 Prism 进程均为 UID 1000、使用安装的 /usr 可执行文件，PSS 合计 200324 KiB。此记录不是 GPU 总内存或性能分位数，不作收益结论。
- **现场反馈：画面正常；Music 状态实际变化但被默认 HUD 遮挡，Dock 激活缺少可见反馈。0.1.0-4 已修复，现场复核已通过。** 当前会话未开启开机自启。玻璃、阴影与 BSP 仍按视觉计划推进，不能把当前文本 demo 视为效果图已完成。

详细 JSON 记录保存在 dist/validation/：prism-v3-physical-final.json、prism-v3-stop.json、prism-v3-outputs.json、prism-v3-processes.json；journal 单独保存。冷/热启动 p50/p95/p99、输入延迟、空闲 CPU、温度、最优池容量与零拷贝仍未验收。

## 历史记录：0.1.0-1 独立客户端部署（已被替代）

- 发布包：`dist/deb/prism-wm_0.1.0-1_arm64.deb`，约 29 MiB；SHA-256 文件保存在同目录。已安装到本机，`dpkg-query` 为 `install ok installed 0.1.0-1 arm64`，`dpkg -V prism-wm` 无差异。
- 包内容检查通过，不含测试/probe/ImGui，不使用 `/usr/local`。发布配置关闭 `BUILD_TESTING`，打包门槛拒绝无客户端和错误安装前缀的配置。
- Bash 语法、systemd 单元校验通过。安装后的五客户端 headless 检查通过，伪造 topbar app_id 保持普通窗口权限；探针输出 `V3D 4.2.14.0`、`configure=3 frame=2 presented=4 images=1/1`。
- 真正物理显示路径：tty8 / seat0 / logind，`HDMI-A-1` 首选模式为 1024×600、59.821 Hz。最终会话没有临时 systemd drop-in；由安装的脚本自动选择本机 V3D render node。
- 物理会话客户端探针通过：`GL renderer=V3D 4.2.14.0`、`configure=2 frame=2 presented=3 images=1/1`。探针仍从源码树中的诊断构建执行，未安装到系统。
- 修复初始 modeset 后立即提交造成的 pending page-flip：由 `wlr_output_schedule_frame()` 调度首帧。最终启动日志 `Native GPU Frame 1 ... commit=OK`，检查期间没有 `ERROR` 或 `commit=FAILED`。
- 最终日志确认 desktop/topbar/dock、音乐和设置五个生产 surface 映射，六个生产进程均以用户 ss 运行。WM 服务为 `prism-demo@ss.service`；两个示例由用户 transient 服务 `prism-demo-player.service` / `prism-demo-settings.service` 运行，工作目录为 `/`，验证安装资源无需源码目录。
- 当时留下实机会话供现场查看；未开启开机自启。远程日志确认映射和提交；用户现场确认显示器画面正常，能够看到顶部栏、底部 Dock、音乐和设置两个应用。物理鼠标/键盘交互仍需验证；本轮不声称已完成完整性能验收或零拷贝验收。

### 实机交互问题修订（0.1.0-4）

现场反馈定位到默认开启的 WM HUD：其 x=18/y=44/440×160 覆盖 Music 控件，输入仍到达下面的客户端，导致状态实际变化但看不见。生产默认关闭 HUD，场景节点在首次提交前就禁用，不闪现。诊断开关仅显式启用。Dock 增加 Opening/active/ready/failed 文字反馈：active 只由 WM Activated 驱动，ready 必须同时收到真实 FirstPresented 与 BackendReady，Accepted 不视为完成。该反馈是 Dock 业务绑定，WM 不接收应用 UI。Settings Theme 仍仅切换 demo 文字，不宣称全局换色。新版已安装，用户确认遮挡消失、播放切换和 Dock 反馈正常。

0.1.0-4 发布文件为 dist/deb/prism-wm_0.1.0-4_arm64.deb（7195062 字节），SHA-256 `50c169c30e51a210e62c71d319e1e0df03ad5d43f4ef47ad6bc9d3ced13baa37`。本轮 CTest 24/24 通过，包含 Dock 不把 Accepted 当作激活成功以及真实首帧与 Ready 双门槛的回归。临时关闭旧会话 HUD 后已重新打包安装。

0.1.0-4 安装后：dpkg-query 为 install ok installed 0.1.0-4 arm64、dpkg -V 无差异；get_status 确认 debug_hud=false。新的物理 probe 再次通过，五应用 V3D 实际首帧日志通过，未见 ERROR 或 commit=FAILED。用户确认遮挡消失、Play/Pause 和进度变化正常、Music active / Pref active 反馈正常；当前保留新版会话供继续演示。对应记录 prism-v4-physical-final.json、prism-v4-status.json、prism-v4-outputs.json、prism-v4-journal.log 位于 dist/validation。
