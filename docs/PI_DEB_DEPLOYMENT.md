# Pi Debian 安装包与物理显示会话

日期：2026-09-27。当前发布包为 **0.1.0-12 arm64**，在 v9–v11 的按需调度、空间依赖与事件等待上，加入 SDK buffer age 历史和安全局部像素修复。38 项测试、真实 V3D SDK/WM/启动链回归及包审计通过；已安装运行，DRM/实例/八种配色自动现场门槛和匹配短测通过；本轮人工点击验收待用户答复。统一 session/launcher/host 保持生产入口；v12 证据见第 9 节，v11 已完成的现场确认保留为第 8 节历史记录。

## 1. 发布规范

- `tools/package-deb.sh [输出目录]` 默认写入 `dist/deb`，固定 Release、PrismGLES、PRISM_ENABLE_GLES=ON、BUILD_TESTING=OFF、安装前缀 `/usr`。Skia 路径可用 PRISM_SKIA_ROOT 指定，并发可用 PRISM_BUILD_JOBS 指定。
- Skia 公共前端只进入 `prism-app-host`，五个应用发布为业务模块、manifest、DSL、资产目录包；模块只依赖契约，不维护 Wayland/EGL/绘制循环。WM 不链接前端渲染引擎。
- 九个平台入口为 prism-wm、prism-session、prism-session-runtime、prism-launcher、prism-invoker、prism-app-host、prism-msg、prism-pack、prism-compiler；后两个旧 AOT 工具不参与生产会话。
- 五个包位于 `/usr/share/prism/apps/`。不安装旧五客户端可执行入口，也不安装 tests、fixtures、probes、源码、ImGui 或 Skia 开发库。
- 动态运行依赖由 dpkg-shlibdeps 生成，另声明 fonts-dejavu-core 和 Mesa 驱动。安装不会自动启动或启用图形会话。生成物位于忽略目录 dist，打包源码和规范纳入 Git。

```sh
tools/package-deb.sh
sudo apt install ./dist/deb/prism-wm_0.1.0-12_arm64.deb
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

## 4. 0.1.0-7 Demo 视觉收尾

- 运行时主题改造先提交为 `c722c88`。本轮生产包 `dist/deb/prism-wm_0.1.0-7_arm64.deb`，7534170 字节，SHA-256：`f7c75754da6a756058c364c2a4870838a967290b6e975c9de5c6e2f6543b0dd1`。安装为 `install ok installed 0.1.0-7 arm64`，`dpkg -V prism-wm` 无差异。
- Topbar 仅保留 Prism 和时间文本；网络/供电改为图标。顶部横线使用深色细底座以对比当前壁纸亮部。Dock 按应用中心、固定 Music/Preferences、正在运行的两个 demo 分段，自然测量可见分组宽度，真实实例归并、最后实例退出后收起；没有文字标签/Active 状态。应用中心当前只预留图标，不展示应用列表。两 demo 改为图标控制、层次化标题、紧凑指标及主题选择。颜色、尺寸、间距、内外阴影仍来自 DSL 主题材料，静态横线共用冷白颜色和 4 px 厚度；没有新增动画。
- 通用 `visible` 与 Box 的明确左右/居中锚点由 SDK 实现，没有应用名绘制分支。完整 CTest 28/28 通过；最终顶线底座资源变更后的编译器/真实模板测试 2/2 通过。模板检查含 482×420、244×420、482×204 下操作命中，Dock 零/一/二运行分组，以及四主题引用/输入轮廓。
- 第一轮真实 V3D 统一会话完整四主题、保持 PID、错误主题恢复、新窗口继承通过（`first-session-results.json`，范围为最终锚点/Topbar 摆放修订前）。最终布局 headless 运行在 Clear 的截图发生一次 10 秒超时；当时 generation 3 已安装，WM 仍持续 commit=OK。失败日志/JSON 原样保留，没有自动重试；不得将旧成功结果或后续物理成功当作该捕获问题已修复。probe 已避免旧 results.json 冒充新一轮成功。
- Release 的第一次已安装 DRM 会话通过首帧/Ready、已有实例激活、NewInstance、取消退出与实例订阅等完整门槛；最终改动仅为顶线 DSL/主题资源，生产二进制相同。更新最终资源时再次回收九个 Prism 进程与三个 socket，停止约 0.353 秒；重启后直接启动两 demo 并获得真实 `GL renderer=V3D 4.2.14.0` FirstPresented/BackendReady。
- 最终物理 HDMI-A-1 为 1024×600、59.821 Hz，服务 MainPID 45029，HUD 关闭；依次切换 Tint、Clear、Square、Glass 均 Applied，WM/launcher generation 一致，两 BSP 窗口保持，4/4 截图成功且服务 PID 未变。实际截图为 `dist/validation/prism-v7-visual/physical-*.png`，本轮状态/包/回收/journal 均在同目录；前一版顶线资源结果单独保存在 `before-final-handle/`。没有启用开机自启。
- 用户现场答复“外观满意，点击正常”，确认图标、三段 Dock、横线与烟灰玻璃外观，以及播放、中间/右侧 Dock 激活和四主题点击。输出刷新率和状态中的瞬时 FPS 不作为性能分位数结论。

执行规范见 [VISUAL_TILING_REFINEMENT_PLAN.md](VISUAL_TILING_REFINEMENT_PLAN.md) 第 8 节。节点增量、分块缓存、实时 Layout 热切换与动画仍按原主线安排后续；本次不宣称 headless screencopy 间歇超时已解决。

## 历史：0.1.0-6 DSL 全局主题部署与现场确认

- 发布包 `dist/deb/prism-wm_0.1.0-6_arm64.deb`，7532324 字节，SHA-256：`049061b6e412fd33a2b619920375100a204d107c1a10800e75a8f06d5aa8a91e`。安装为 `install ok installed 0.1.0-6 arm64`，`dpkg -V prism-wm` 无差异。
- Release 构建使用 Skia GLES、`BUILD_TESTING=OFF` 和 `/usr` 前缀。安装清单包含四份 `themes/<id>/theme.prism`，不含旧主题 JSON、静态生成头、测试/probe/fixture、ImGui 或旧独立客户端。WM 符号检查不包含客户端 Scene、DSL/主题编译器、SkCanvas/SkSurface。
- 最终 CTest 28/28 通过；透明主题前景调色后重复相关编译器/模板测试 2/2 通过。诊断和 Release 两种真实 V3D 统一会话均通过四主题切换、现有 host PID 保持、预热新窗口继承当前 generation、编译失败、host 拒绝后恢复、请求 ID/订阅冲突检查。失败恢复使用更高 generation，保持原主题内容。
- 升级前停止旧服务约 0.476 秒。旧会话 journal 记录六个 host 回收及 WM 正常退出；从日志确认的九个旧 Prism PID 均已不存在，launcher/Wayland/IPC socket 在停止后已移除。详细来源见 `dist/validation/prism-v6-theme/upgrade-shutdown.json`，不以未读到 `/proc/<pid>/exe` 误判空进程列表。
- 安装后启动 `prism-demo@ss.service`，沿用 tty8/seat0，HDMI-A-1 为 1024×600、59.821 Hz。Shell、Music/Preferences 均由统一 host 承载，实际客户端上下文报告 `V3D 4.2.14.0`，HUD 关闭。真实 DRM 启动、首帧/Ready、已有实例激活、NewInstance、取消退出、保留 Shell 拒绝与实例流门槛再次通过。
- 已安装物理会话依次切换 `translucent`、`transparent`、`square`、`glass`，WM/launcher 返回一致 generation，服务未重启，两个普通 BSP 窗口保持。截图、协议结果位于 `dist/validation/prism-v6-theme/physical-*`。用户随后在 Preferences 现场操作，答复“四种主题切换及点击都正常”，确认顶部栏/Dock/两窗口效果切换，以及播放和 Dock 激活正常。保留用户最后选择，不强制切回默认主题；未启用开机自启。

规范与作者接口见 [DSL_THEME_RUNTIME.md](DSL_THEME_RUNTIME.md)、[THEME_AUTHORING.md](THEME_AUTHORING.md)。本次验收覆盖当前单输出上的外观热切换；当前热切换拒绝修改 Shell 保留带/BSP 间距的 `Layout`，需要后续 configure/commit 感知事务。主题选择尚不自动持久化。专项性能分位数、多屏/DPI 与零拷贝仍不据此作完成结论。

## 历史：0.1.0-5 BSP 与玻璃主题部署

- 发布包 `dist/deb/prism-wm_0.1.0-5_arm64.deb`，7259770 字节，SHA-256：`f731e8d0b8cb716c7cc8551265a721871ed4008ca639435a5620769f5144e3a9`。安装为 `install ok installed 0.1.0-5 arm64`，`dpkg -V` 无差异。
- Release 包清单检查通过：统一 host/模块/DSL/资产及共享主题 JSON，包含新材料协议运行代码；不含旧客户端入口、测试、probe、fixture 或 ImGui。解包前缀的真实统一会话与预存 PAM helper 回收检查通过。
- 最终 CTest 26/26 通过，包含无显式 window geometry 的真实客户端 BSP、提交尺寸/位置、嵌套分割、工作区与全屏，及透明/圆角/图标/局部阴影/裁剪材料契约。真实 host 双窗、三窗、四窗截图已保存，不能仅以模拟树代替真实客户端布局。
- V3D 背景回归的五个独立会话、40 次捕获通过；高频下层跨度 191、玻璃跨度 4，下层红/蓝变化透过玻璃产生均值差变化 23.5。前期捕获曾出现间歇性 8 秒超时，原因未定位，后续成功样本不视为修复；probe 保留超时诊断并失败，不自动重试。物理会话截图此次成功。
- 旧会话停止约 0.417 秒，九个旧 Prism 进程与 socket 均回收；安装新包后重新启动 `prism-demo@ss.service`。当前 tty8/seat0、HDMI-A-1 为 1024×600、59.821 Hz；五个应用真实 FirstPresented 均为 `GL renderer=V3D 4.2.14.0`，WM 的 GPU backdrop capability=1。新会话日志无 ERROR 或 commit=FAILED，HUD 关闭。
- 实机协议门槛再次通过：真实呈现/Ready、同实例激活、NewInstance、取消退出、保留 Shell 拒绝及实例快照。最终保留 Music/Preferences 两个普通 BSP 窗口和 Shell 供操作，没有启用开机自启。
- 实际输出截图为 `dist/validation/prism-v5-physical.png`，操作后截图为 `prism-v5-physical-confirmed.png`。版本、包、输出、树、状态、停止与启动记录均以 `prism-v5-*` 保存。用户答复“外观正常，以上点击都正常”，确认播放状态变化、Preferences 本窗口变色和 Dock Music/Pref 激活；结果记录在 `prism-v5-physical-final.json`。全部键盘快捷键尚未逐项现场复核，不把截图或 59.8 Hz 输出模式当作完整性能验收。

主题及职责规范见 [SURFACE_MATERIALS.md](SURFACE_MATERIALS.md)，快捷键与本轮边界见 [VISUAL_TILING_REFINEMENT_PLAN.md](VISUAL_TILING_REFINEMENT_PLAN.md)。0.1.0-5 的 Settings 仅切换本窗口真实配色，当时尚无全局主题服务；Music 仍为演示业务。实时拖动分割比例、动画与性能专项留待后续。

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

## 5. 0.1.0-8 设置及配色

- 最终包 `dist/deb/prism-wm_0.1.0-8_arm64.deb`，7594046 字节，SHA-256 `c3c4337d079f9e060d162eb281ec64eb0aa38ce3e0bb257d597af87825d32d1a`。安装为 `install ok installed 0.1.0-8 arm64`，`dpkg -V prism-wm` 无差异。旧会话十个 PID 已全部回收；沿用 tty8/seat0，不启用自启动。
- 诊断构建完整 CTest 30/30 通过；Release 包无 tests/probes/fixtures/ImGui，WM 链接与符号中无客户端 Scene、主题编译器或 Skia。
- 最终优化版的物理协议门槛通过：真实 V3D 首帧与 Ready、已有实例激活、新实例、取消退出、保留 Shell 拒绝和实例流。八种材质/配色组合、保留另一个选择维度、错误包不改当前主题、请求 ID 配色冲突关闭连接均通过；WM PID 和两个 BSP 窗口保持。
- 实际 UI 键盘选择 Appearance 与 Light 已生效，四个 Shell/demo 前景统一换色；性能/外观和播放状态截图保存在 `dist/validation/prism-v8-preferences/`。背景保持现有资源。GPU 显示真实时钟 MHz，不冒充利用率；监控暂停及采样设置经模块测试和实际 UI 检查。
- 根据第一轮测量修复隐藏子树无用重画，并通过接口将默认 Ganesh 缓存软预算设为 32 MiB。有效空闲、125 Hz 指针、Music 进度、Square 暗和 Glass 亮测试已记录；约 223–224 MiB PSS，CPU 约 4.4–8.7%（单核 100%），没有提交失败。样本边界、孤立呈现间隔及尚未解决的实鼠标体验问题见 [PREFERENCES_AND_PERFORMANCE.md](PREFERENCES_AND_PERFORMANCE.md)。

该版本用户现场答复“布局与配色满意，操作正常”，确认性能指标、监控/采样和明暗/材质操作；该新增功能验收与旧 0.1.0-7 分开记录。此确认不代表鼠标主观卡顿已完成定位。

## 6. 0.1.0-9 按需渲染

- 发布包 `dist/deb/prism-wm_0.1.0-9_arm64.deb`，7606126 字节，SHA-256 `564f7cec562b1195e03ead622aa48cd3c8c6741ed94039255b9b0ec0a53335c4`。安装为 `install ok installed 0.1.0-9 arm64`，`dpkg -V` 无差异；旧会话十个 PID 已回收。沿用 tty8/seat0 和现有服务参数，不开启自启动。
- 最终 CTest 32/32；独立真实 V3D 调度 probe 25 场景通过，包含无像素提交的 callback、子 surface 位置/层级/显隐和 XDG 局部原点。源码与测试位于各自目录，生产包九个平台入口/五个业务包，无 diagnostics/ImGui，WM 无 DSL/Skia 前端符号。
- 安装后的真实 V3D 首帧/Ready、激活、新实例、取消退出、Shell 拒绝、实例流和八种材质/配色组合再次通过。真实键盘 Play/Pause、进度、HUD 显隐及最终截图通过，诊断窗口已关闭，Music 暂停、Preferences 聚焦、Glass dark 保留供现场操作；新会话日志未见 `[ERROR]` / `commit=FAILED`。
- 同日同条件 12 秒新会话对照：空闲会话 CPU 4.97% → 2.15%、实际呈现 717 → 12；125Hz 指针 CPU 7.77% → 6.07%、呈现仍 718。CPU 均以单核 100% 计。指针下效果工作仅每秒一次，没有随 60Hz 输出增长。PSS 约 218.5MiB，未作为内存优化收益。
- 规范、计数解释、完整测量条件与后续阶段见 [RENDER_SCHEDULING_AND_INVALIDATION.md](RENDER_SCHEDULING_AND_INVALIDATION.md)。原始证据位于 `dist/validation/prism-v9-render-scheduling/`。用户静置后手动确认“操作正常，鼠标更顺畅”，覆盖 Play/Pause、Dock 激活及明暗切换；此主观改善与协议/CPU 证据一起记录，端到端延迟仍未量化。

## 7. 0.1.0-10 空间依赖优化发布

生产包 `dist/deb/prism-wm_0.1.0-10_arm64.deb`，7612778 字节，SHA256 `b2b97ede9ca9254828c631c650f6c6821fb61320f8f9f8b9ce54abf47cb65364`。

最终 33/33 CTest、V3D 50 功能场景（另 4 诊断记录）、安装后的物理启动/实例门槛、八种材质/明暗配色均通过。九个平台入口、五应用包，无 tests/probes/fixtures/ImGui；生产 WM 无客户端 Scene、DSL/theme 前端及 Skia 符号。升级前完整回收旧 session.scope 的十个 PID，安装后 `dpkg -V` 无差异。服务和启动参数沿用原入口，当前 MainPID 为 8329。

新会话匹配短测中，空闲及 Music 更新都复用全部背景效果缓存，capture/blur/material 为零，客户端与输出仍更新 12/36 次。Music 会话 CPU 观察值 5.29% → 4.49%（单核 100%），空闲会话 CPU 未改善；PSS 基本持平。测量条件、governor 差异及证据边界见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 8 节。记录保存在 `dist/validation/prism-v10-spatial-effects/`。用户现场确认“画面和操作都正常”：玻璃/阴影边缘、Music、Dock 及主题/明暗切换均已验收。

本会话在 11:08:25 手动操作期间记录一次 libinput 的 Razer Orochi V2 事件处理积压约 50ms。没有效果提交或区域失败；该输入积压尚未归因，不能据本轮零额外玻璃 pass 宣称所有交互阻塞已消除。

## 8. 0.1.0-11 SDK 提交与事件等待发布

生产包 `dist/deb/prism-wm_0.1.0-11_arm64.deb`，7624518 字节，SHA256 `4593c42c2ea4ab8742064933e21f608b4f2dac0f680c95d2f553197b0f0dda89`。安装为 0.1.0-11 arm64，`dpkg -V` 无差异；停止升级前 session.scope 的十个进程身份全部回收。服务沿用 tty8/seat0 与原参数，没有开启自启动；部署后 MainPID 12236，Result=success。

最终 36/36 CTest、V3D SDK 状态/像素/资源/多客户端清理门槛、WM 50 功能场景 + 4 诊断记录，以及 Host/launcher/session 回归通过。包含 Preview/Master 同 surface、启动/取消/watchdog、失败 worker 清理和带继承 helper 的四种退出场景。包仍九入口/五应用，不含 tests/probes/fixtures/ImGui；发布 WM 对应经符号审计的构建产物。

安装后的 DRM 真实 V3D 首帧/Ready、实例激活/新增/取消、Shell 身份拒绝、实例流、八种材质/配色及错误事务全部通过。Glass dark 物理截图确认双窗与装饰；本轮现场答复“画面和操作都正常”，覆盖 Music、Dock 和主题/明暗切换。新会话自动门槛与测试结束时 journal 未见 `[ERROR]`、`commit=FAILED`、SDK 提交失败或输入积压；这不代替持续输入延迟测量。

匹配新会话短测：空闲会话 CPU 2.10% → 0.76%，Music 模拟进度 CPU 4.56% → 2.98%（单核 100%）；输出提交保留 12/36 次，额外玻璃 pass 仍为零，PSS 基本持平。单独 12 秒观察中，supervisor/launcher、暂停 Music、Desktop、Dock 与预热 host 的主线程自愿上下文切换均为零。温度、频率、present 端点差、计数定义与一次短测限制见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 10 节。

证据位于 `dist/validation/prism-v11-sdk-event-wait/`；未将诊断或性能采样工具安装进生产环境。下一步按 buffer age/damage 历史与安全局部像素绘制主线推进，当前 Pixels 仍完整绘制 surface。

## 9. 0.1.0-12 Buffer age 与局部像素修复发布

生产包 `dist/deb/prism-wm_0.1.0-12_arm64.deb`，7648896 字节，SHA256 `37baa795b4ca424f68aabaf7596f36d48d531a863bb483026cb524f1d95ad754`。

### 实现与能力边界

- SDK 分别保存当前内容变化与当前 back buffer 所需的修复区域。有限历史只在 Swap 成功后推进；age 为零、未知、历史不足、目标 resize/epoch 变化时完整修复，不以 buffer 指针或主题 generation 冒充像素版本。业务模块与 WM 不解析客户端绘制命令。
- EGL 扩展使用完整 token 与可用入口检测，修复区域声明和交换内容损伤分别提交。空变化与全表面明确区分；交换失败不进行第二次 Swap。KHR-only 的 age 只有在 partial update 入口与 swap behavior 可用时才允许局部保存；否则完整回退。
- 真实 Pi 的 SDK 能力为 `buffer_age_supported=1`、`swap_damage_supported=1`、`partial_update_supported=0`，使用 EXT buffer age 的内容保存和损伤交换路径。本轮没有实际调用 KHR `eglSetDamageRegionKHR`，不能把通用接口实现作为该扩展的实机验收。
- GLES 仅清除与完整重放修复区域内的内容，区域外保留当前缓冲像素；不在 WSI 已声明修复区域后应用面积阈值而扩大绘制。identity 矩阵下普通非 AA 矩形裁剪统一向外取整到整数像素，圆角 AA 裁剪保持其覆盖规则；不确定结构/变换保守完整回退。
- 诊断 CPU 后端对非空局部修复采用完整临时重放再复制精确修复跨度，避免裁剪曲线造成的栅格化舍入差异。这是正确性回退，不宣称 CPU 已减少绘制工作；生产 GLES 没有采用该临时全帧路径。此阶段未实现节点增量布局、分块缓存、动画或零拷贝验收。

### 回归与包证据

- 最终 CTest **38/38** 通过。真实 V3D WM 调度门槛为 **50 个功能场景 + 4 个诊断记录**；真实 SDK 状态/像素、启动链与失败回收回归通过。
- CPU/GLES 使用共同的完整命令序列，对照各后端的完整重绘；1/2/3 缓冲轮转模拟每组 **75 帧**。模拟 age 由目标上次成功使用的序列计算，不冒充真实 WSI 查询。native Wayland 门槛包含 **74 个场景、75 次完整目标 readback**，实际 WSI 查询与显示路径结果和模拟轮转结果分别记录。
- 像素门槛要求内容覆盖一致、修复区域外逐字节一致。RGBA 各通道差值最多 1 仅允许在双方均非透明时作为量化差异；透明内容错位不能用该容差掩盖。包含同命令数量的移动、alpha/透明清除、重叠 source-over、描边、外/内阴影、字形 ink、全部图标、image fit/同 ID 资源 epoch、嵌套裁剪与回退；直接声明 80% 修复区域的独立用例检查区域外哨兵不被扩大清除。
- 相同的 24 次 SDK 像素提交中，Auto 为 **23 次局部 + 1 次完整**，Full 对照为 **24 次完整**。修复面积为 **1052815 / 22118400 像素**，约减少 **95.24%**。这是客户端修复工作量计数，不是 GPU 耗时、帧率或端到端延迟收益。
- 最终包审计仍为九个平台入口、五个业务模块及对应 DSL/资源；无 tests/probes/fixtures/ImGui。WM 没有 SDK Scene、DSL/主题编译器或 Skia 前端符号。上述回归覆盖最终源代码；Release 包另经符号/清单审计及已安装生产会话验证。

证据目录为 `dist/validation/prism-v12-buffer-damage/`，包括 `ctest-release-gate.log`、`pixels-rgba-policy.log`、`sdk-native-native-pixels.log`、`sdk-native-gates-final.log`、`wm-gles-release.log`、`runtime-gates.json` 与 `package-audit.json`。此前失败的严格像素日志原样保留；后续修正与最终门槛分别记录，不将失败日志改成成功。

**部署状态：** 已安装为 `0.1.0-12 arm64`，`dpkg -V prism-wm` 无差异；`prism-demo@ss.service` active，使用原 tty8/seat0 链，未启用自启动或改变默认 target。原 v11 十个进程身份与首次 v12 八个进程身份均回收，最终 supervisor PID 17350 / WM PID 17360。已安装 DRM 首帧/Ready、已有实例激活/新实例/取消、Shell 身份拒绝、实例流、八种材质/配色及错误事务通过；恢复 Glass dark、暂停 Music、焦点回到 Preferences。物理截图确认文字、图标、双窗、玻璃与阴影正常，当前会话 journal 未见 ERROR、失败提交或输入积压。已发起本轮人工现场确认，等待用户答复。

同条件一次 12 秒短测中，Music 更新会话 CPU 从 3.07% 降至 1.91%，WM 保持 0.50%；空闲会话 CPU 0.69% → 1.02%，没有改善。输出提交/实际呈现保留 12/36 次，玻璃额外 pass 均零；PSS 未下降。v12 空闲结束时 ondemand 频率为 700MHz，其他起止 1.8GHz；结果与限制见 [渲染调度与失效传播](RENDER_SCHEDULING_AND_INVALIDATION.md) 第 12 节。

第一次自动部署脚本过早在 launcher socket 就绪后检查 theme，实际主题提交尚未完成；初始 JSON/日志保留为 `.initial`，修正为等待非零主题 generation 后重新启动干净会话测量。最终记录为 `deployment.json`、`v12-physical.json`、`appearance/results.json`、`physical-glass-dark.png`、`journal-installed.log` 与四组逐秒性能报告。
