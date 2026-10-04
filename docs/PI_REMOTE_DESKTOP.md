# Pi 远程桌面：Headless Prism + WayVNC

此方案创建独立的 Prism headless 会话，供 Windows 上的 TigerVNC 通过 SSH 隧道访问。
已安装 v18 的现场输出 `HEADLESS-1` 为 1280×720、90 Hz。
它不是 HDMI 画面的镜像。`prism-demo@.service` 物理显示单元保持原样；同一用户启动
`prism-remote@.service` 时，systemd 的 `Conflicts=` 会停止该用户的物理 demo 单元。
不要同时从显示管理器或命令行另起同一用户的 Prism 会话；固定的 Wayland socket 和
launcher endpoint lock 也会拒绝并发生产会话。

## 1. 服务与启动

包安装两个系统模板单元，但不自动启用或启动。`prism-remote@USER.service` 以普通用户和
`PAMName=login` 取得私有 `XDG_RUNTIME_DIR`，运行 `WLR_BACKENDS=headless`、
`WLR_RENDERER=gles2` 的统一 `prism-session`。它沿用已有 supervisor 清理 WM、launcher
和应用。`prism-remote-vnc@USER.service` 随对应会话启动和停止，最多等待 30 秒让
`wayland-prism-0` socket 出现；WayVNC 固定抓取 `HEADLESS-1`，禁用 Viewer 发起的自动改分辨率，
仅绑定 `127.0.0.1:5900`，并忽略用户的 WayVNC 配置文件。v18 起不将鼠标箭头
叠加到视频帧，而由 TigerVNC 在 Windows 本地绘制箭头。
WayVNC 是单独安装的运行工具，
不是 `prism-wm` deb 的强制依赖。
从 v16 起将 WayVNC 的发送上限设为 `120 FPS`，为持续变化画面的 60 FPS 目标留余量。
这是速率上限，不会强制静止桌面重复发送相同画面；实际帧率还取决于损伤、编码和网络。
从 v17 起，在启动 VNC 前将 `HEADLESS-1` 的虚拟输出设置为 1280×720、90 Hz，
使输出计时不再限制 60 FPS 的送帧目标。该设置只用于远程服务，不改变物理显示单元。

此 Pi 的发行版 WayVNC 包安装时曾自动启动 `wayvnc.service` 和
`wayvnc-control.service`，其中服务实测监听 `*:5900`。先停用这些发行版单元，
再启动仅监听 loopback 的 Prism sidecar。以实际桌面用户名替换以下 `ss`：

```sh
sudo systemctl disable --now wayvnc.service wayvnc-control.service
sudo ss -ltnp '( sport = :5900 )'
sudo systemctl daemon-reload
sudo systemctl start prism-remote@ss.service prism-remote-vnc@ss.service
systemctl status prism-remote@ss.service prism-remote-vnc@ss.service
sudo ss -ltnp '( sport = :5900 )'
```

监听地址应只有 `127.0.0.1:5900`，不得是 `0.0.0.0:5900` 或外网地址。启动异常时查看：

```sh
sudo journalctl -b -u prism-remote@ss.service -u prism-remote-vnc@ss.service --no-pager
journalctl -b -t prism-remote-ss -t prism-remote-vnc-ss --no-pager
```

需要开机启动时，明确启用这两个实例；VNC 实例挂在对应 Prism 会话下：

```sh
sudo systemctl enable prism-remote@ss.service prism-remote-vnc@ss.service
```

停用远程桌面及其开机启动：

```sh
sudo systemctl disable --now prism-remote-vnc@ss.service prism-remote@ss.service
```

## 2. Windows 连接

在 Windows PowerShell 保持以下 SSH 命令运行。示例 Pi WLAN 地址为
`192.168.137.6`；地址变化后改为当前地址。首次连接时核对 SSH 主机密钥。

```powershell
ssh -o ExitOnForwardFailure=yes -N -L 127.0.0.1:5900:127.0.0.1:5900 ss@192.168.137.6
```

随后从 Windows PowerShell 启动 TigerVNC Viewer，强制使用本地系统箭头：

```powershell
& 'C:\Program Files\TigerVNC\vncviewer.exe' -AlwaysCursor=1 -CursorType=System '127.0.0.1::5900'
```

若安装位置不同，替换可执行文件路径。双冒号表示 TCP 端口 5900；单冒号后接的是
VNC display 编号。当前 Prism headless 会话的远程箭头形状传输不可用，省略
`-AlwaysCursor=1` 可能导致箭头不可见。SSH 断开后，重新建立隧道再连接 Viewer。
若 Windows 的系统快捷键留在本机，可先点入 Viewer，再用 `Ctrl+Alt+G` 抓取键盘；
`Ctrl+Alt` 释放。`Ctrl+Alt+M` 可打开 Viewer 菜单，检查 `View only` 未勾选。
需要诊断 Windows 收到的图像更新数时，可临时用以下命令打开 TigerVNC 的
[调试图表](https://github.com/TigerVNC/tigervnc/wiki/Debug-Logs)：

```powershell
New-Item -ItemType Directory -Force C:\temp | Out-Null
& 'C:\Program Files\TigerVNC\vncviewer.exe' -Log '*:file:100' -AlwaysCursor=1 -CursorType=System '127.0.0.1::5900'
```

右下角绿色 `upd/s` 表示 Viewer 收到的 RFB 更新数。静止桌面与本地绘制箭头
不会持续制造图像变化，因此该值可能很低；测试连续画面变化时再读取。
本轮 WayVNC 首次连接曾短暂显示灰色占位帧，同一连接约半秒后的下一帧已有完整桌面；
若持续灰屏，检查 VNC 日志，并在同一连接等待后续画面更新。
WayVNC 本身通过 loopback 提供未加密的 RFB，远程链路的认证和加密由 SSH 提供。
WayVNC 的[上游说明](https://github.com/any1/wayvnc/blob/master/README.md)建议
loopback + SSH；TigerVNC 的[查看器手册](https://tigervnc.org/doc/vncviewer.html)
说明了双冒号端口格式。

## 3. 验证范围与问题定位

- 在 Pi 上检查 `systemctl is-active` 均为 `active`，journal 中有 Prism headless 输出、
  Shell 映射和 WayVNC 监听消息。`ss` 必须显示 loopback 监听；Windows Viewer 应看到
  桌面并能移动指针、点击和键入。若只有画面没有输入，检查 compositor 是否向
  WayVNC 公布 virtual pointer / virtual keyboard 协议及对应输入设备事件。
- `WLR_RENDERER=gles2` 是合成器的渲染请求。只有实际 WM/客户端日志中的
  `V3D 4.2.14.0` 等硬件 renderer 证据才能证明这次会话使用 GPU；
  已安装 v14/v15 的客户端首帧记录和 WM 日志均确认了 V3D；RFB 画面另行实测。
  `/dev/dri/renderD*` 的编号不能写死，也不能用设备存在本身代替运行时确认。
  运行用户需具备所选 render node 的权限。WayVNC 将帧编码并通过 VNC/SSH 发送；
  本方案不启用 `wayvnc --gpu`，不声称 VNC 视频硬件编码或零拷贝。
- Headless 画面不验证 HDMI/DRM 呈现时序、物理输入延迟或网络传输帧率。
  既有一次 headless screencopy 超时记录见
  [PI_DEB_DEPLOYMENT](PI_DEB_DEPLOYMENT.md)。本轮四种主题切换后的抓图均成功，
  但短时成功不能证明长期不会再出现超时。

## 4. 2026-09-28 Pi 部署记录

- 安装 `prism-wm 0.1.0-14` arm64，包的 SHA-256 为
  `438c8f6460da67cbde82161e0723fe32852040f7bdc6397727ded51429197a79`；
  `dpkg -V prism-wm` 无差异。WayVNC 为 Pi 仓库的 `0.10.1-1+rpt1`。
- `prism-remote@ss.service` 和 `prism-remote-vnc@ss.service` 均已启用且运行；发行版
  `wayvnc.service`、`wayvnc-control.service` 已停用。VNC 实测只监听
  `127.0.0.1:5900`，Pi WLAN 地址当时为 `192.168.137.6`，SSH 服务正在运行。
- WM 的 headless 输出为 `1280×720`，客户端首帧报告 `GL renderer=V3D 4.2.14.0`。
  `grim` 和 RFB 接收端均抓到带 Topbar、Dock、Music、Preferences 的完整画面。
  通过 RFB 鼠标点击 Music 的播放按钮，画面中的图标从播放变为暂停；再次点击恢复。
  虚拟键盘和鼠标的独立协议测试及原有 Wayland 生命周期测试通过。
- 本轮证据在忽略目录 `dist/validation/prism-remote/`，其中
`v14-grim.png`、`v14-music-click.png` 和 `v14-music-click-after.png` 分别记录
  合成器抓图与 RFB 点击前后画面。Windows Viewer 的真实连接结果见下方 v15 记录。

### v15 光标可见性修复

Windows TigerVNC 用户实测 v14 画面正常，但看不到可操作的远程光标。WM 的
`performance.pointer_events` 在其触摸板移动时增长，服务端 RFB 探针也能点击按钮；
排查范围收敛到远程光标显示。临时以 WayVNC `--render-cursor` 叠加箭头后，RFB
抓帧中能看到准确位置的光标，用户确认“能看到箭头，点击正常”。此外，通过 RFB
发送 Super+2、Super+1，工作区实际从 1 切到 2 再切回 1，证实服务端键盘链路正常。

正式包已升级到 `prism-wm 0.1.0-15` arm64，SHA-256 为
`105223acd5bc4b8bf6ff25c735bed697dc7ec036e2add1d4012c3db93f28ce77`。
持久 VNC 单元启用 `--render-cursor`，已安装运行，`dpkg -V prism-wm` 无差异；
`prism-remote@ss.service` 和 `prism-remote-vnc@ss.service` 均为 active/enabled，
仍只监听 `127.0.0.1:5900`。包内不含探针。服务端抓帧证据为
`dist/validation/prism-remote/v15-service-cursor.png`。Windows 用户重新连接最终
持久服务后，确认触摸板移动、Music 点击及 Super+2/Super+1 键盘工作区切换均正常。

### v16/v17 远程帧率诊断

`prism-wm 0.1.0-17` arm64 已安装运行，包的 SHA-256 为
`0a59619882e0414a238300f1f4dd5fb688e18826f6fc87d2c4af6a43afeec610`。
从 v16 起，WayVNC `--max-fps=120`；从 v17 起，远程单元在 VNC 启动前将
`HEADLESS-1` 设置为 90 Hz。原始 RFB 客户端在 Pi loopback 上连续模拟指针移动、
按每次帧更新计数，旧的 30 FPS 上限实测 29.87 更新/秒；v16 约 57.12 更新/秒；
v17 在 120 次/秒的指针输入下约 72.75 更新/秒。这些是局部压测结果，不能代表
Windows TigerVNC 实际显示帧率，也不表示静止桌面应该持续发送 60 帧/秒。

Windows 用户确认 v17 的画面和点击正常，但仍感觉移动不够流畅。Pi 到 Windows
热点的 20 次 ping 当时无丢包，Wi-Fi 为 5 GHz，信号约 −41 dBm；没有发现明显的
链路带宽上限。WayVNC `--render-cursor` 会把箭头绘制进远程图像，箭头跟随图像更新，
因此指针体感也可能受到抓屏、编码和网络帧间隔影响。继续定位时应分别测量
WayVNC 捕获/发送更新与 TigerVNC 实际接收更新，不以 `prism-msg get_outputs` 中的
`current_fps` 代替传输帧率；该值是有画面工作时帧间隔的平滑估计。

### v18 Windows 本地箭头

同一 Pi 会话上建立临时对照端口 5901，不使用 WayVNC `--render-cursor`，
TigerVNC 使用 `-AlwaysCursor=1 -CursorType=System`。Windows 用户确认箭头比
原 5900 更流畅，Music 和 Dock 点击正常。对照期间 WayVNC 选择 Tight 编码，
连续移动时每秒捕获并送出约 27–32 次更新；这说明该试验条件下图像更新仍低于
60 次/秒，但箭头可以由 Windows 即时显示。v18 将此配置用于正式 5900 服务；
仅由 TigerVNC 本地绘制箭头，不把指针移动当作必须编码整张画面的理由。
正式安装包 `prism-wm 0.1.0-18` arm64 的 SHA-256 为
`e97490fc32c37da687dd3b6ec08435ff3389db34fa92b1102f3ef052814102b3`。
`dpkg -V prism-wm` 无差异；Prism 和 VNC 服务仍为 active，正式服务仅监听
`127.0.0.1:5900`，Windows Viewer 已连接。用户接受正式 5900 的当前效果。
临时 5901 服务已停止；排查时临时关闭的 Wi-Fi 省电模式也已恢复原设置。

### v19 动画与布局控制部署（2026-10-04）

已安装 `prism-wm 0.1.0-19 arm64`，基于 `5e3a319` 加发布版本号更新。包位于
`dist/deb/prism-wm_0.1.0-19_arm64.deb`，SHA-256 为
`c8efa194150001860d1c4a0f5aaa712f1bd57854802b4cf63d5bc4fa9c3619fb`。
Release/GLES、BUILD_TESTING=OFF；包含三套 motion.prism 和 LayoutControls 模块，
不包含测试、probe 或 ImGui。`dpkg -V prism-wm` 无差异。

沿用 v18 的服务参数：1280×720、虚拟输出 90 Hz、WayVNC 上限 120 FPS，仅监听
127.0.0.1:5900，Windows Viewer 使用本地系统箭头。两个 remote 服务继续 active/enabled。
这些配置值不是实际网络呈现帧率保证。Windows 连接命令仍见本文件第 3 节。

当前六个 Host 均 Ready；Desktop、Topbar、Dock、Music、Preferences 均完成 V3D
首帧呈现。LayoutControls 默认隐藏，不能以未上屏误判未就绪。最终 WM 提交失败计数 0，
VNC RFB 3.8 握手通过。外观与手感由用户连接后确认。

体验入口：Topbar 中央横线向下拖动至少 24 个逻辑像素后释放，切换组沉浸；中央顶边
可唤回恢复入口，Super+Shift+F 恢复整组。Super+F 切换单窗全屏；Super+右键打开
窗口控制面板。Topbar/Dock 显隐仍即时，组窗口已有共享边界动画。

构建初次遇到旧 wayland_window.cpp.o 的损坏重定位信息，保留该生成物并重编后打包
成功。首次会话 Desktop 模块触发既有 cooperative entry budget，日志保留；重启后
全部就绪，未修改预算或加入生产延时。初轮就绪检查误要求隐藏 LayoutControls 的
FirstPresented，后改为 Ready + 可见 Host 首帧检查。该次重启恢复不代表已解决首次
加载预算的根因。证据目录：`dist/validation/prism-v19-deploy/`，最终为
`deployment.json`、`journal-restarted.log`、`status-final.json`、`tree-final.json`。

### v20 多标签记事本部署（2026-10-04）

源码提交 `44733c8`，正式包 `dist/deb/prism-wm_0.1.0-20_arm64.deb`，SHA-256：
`7803551f04caa9bd984e2b9944f4d1dd0bdcab9e044f7f633007a6bb04bcc5d5`。
包含通用 TextField/TextArea、业务编辑/关闭契约和 Prism Notepad；无测试、probe 或 ImGui。

已安装并重启两个 remote 服务，继续 active/enabled；`dpkg -V prism-wm` 无差异。
沿用 1280×720@90Hz、WayVNC 上限 120 FPS、loopback 5900 和 Viewer 本地箭头。
Desktop/Topbar/Dock 和 Music/Preferences/Notepad 均完成 V3D 首帧；记事本经正式
launcher 启动，位于工作区 1 右侧并获得焦点。七个应用实例（包含隐藏的 LayoutControls）
之外的空闲预热 Host 不属于可见窗口。WM commit_failures 为 0，VNC RFB 3.8 握手通过。

Windows 保持原 SSH 隧道与 Viewer 参数，断开后重新连接即可查看。记事本顶部新建
图标创建标签，点击标签切换，× 关闭对应标签；有修改时可取消、放弃或保存并关闭。
首次保存填写绝对路径。实际鼠标、键盘及最终视觉效果仍待用户通过 VNC 确认。

打包初轮发现旧 `layout_control.cpp.o` 调试信息重定位损坏，保留该生成物并重新编译
后成功；未修改生产源码绕过链接错误。构建日志、包清单、服务日志、窗口树和状态记录
位于 `dist/validation/prism-v20-deploy/`。业务与 32 组主题/布局检查结果见
`dist/validation/notepad-ui-20261004/`。
