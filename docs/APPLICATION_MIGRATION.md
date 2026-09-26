# 应用切换至客户端 Skia

demo_player、demo_settings、desktop、topbar、dock 现统一链接 `prism_client_app`，在各自进程内解析 DSL、维护绑定、shaping、解码资源并通过 Ganesh GLES/EGL 提交 Wayland surface。模板从源码目录或安装目录加载；安装包包含五个应用与模板。

WM 已删除 ImGui 源码和链接、topbar/dock DSL 解析与绘制、内置壁纸像素缓冲、应用 Scene/slot 镜像、软件 UI 回放、旧 `prism_sdk`。`prism_core` 只编译 WM、布局、装饰、窗口模型及窗口管理 IPC；CMake 检查其源文件不能 include 客户端 parser、Scene、SDK、Skia 或 ImGui。旧 AOT 编译器和包工具单独链接 `prism_aot_tools`，不会进入 WM。

旧 demo `.prismb`、`.prismpkg` 和 preview 模板已删除，manifest 改为声明源码 UI 与可执行文件名。当前新客户端按独立二进制和安装模板部署，尚未接入新的包内资源读取；旧 AOT 包工具不是新 SDK 的运行入口。

## shell 身份与分层

WM 从自身可执行文件的安装目录或同一构建目录启动三个 shell 程序，保存其子进程 PID。接收 XDG surface 时使用 Wayland 服务端的进程凭据匹配授权；每个身份只允许一个 shell surface。desktop 放背景树，topbar/dock 放 chrome 树；普通 surface 放应用树。app_id 和标题不授予权限；单独运行 shell 程序只获得普通窗口。

这是当前单会话受信任启动模型：shell 由 WM 持有并在 WM 退出时终止。没有开放 shell 注册 IPC，也未实现第三方 shell、角色委托或自动崩溃重启。仍采用 xdg_toplevel 的 configure 生命周期，由 WM 决定授权 shell 的固定几何；后续若需要标准 layer-shell，应保持相同授权边界。

## 当前功能范围

- 五个应用使用文字、按钮、颜色、图片和绑定。时钟和 demo 的周期更新在客户端事件线程内执行，移除了脱离管理的后台线程。
- 壁纸通过客户端图片资源提交，Box 内无显式尺寸的图片填充可用空间。当前默认 PNG 壁纸；旧壁纸轮换和 JPEG 预设尚未迁入。
- dock 点击调用 `posix_spawnp` 实际启动应用，失败时不会标记已运行。当前计数仅为 dock 发起的启动记录，不是 WM 全局进程/窗口列表，也未实现激活已有窗口。
- 原示例中的 Slider、Icon、backdrop blur、hover spring 尚未接入新运行时；进度暂以文字显示，shell 暂用纯色底。不得把这些效果写成已完成。
- 新 XDG 客户端的布局目前仍由 `ArrangeXdgViews` 平铺；旧 BSP/装饰模型保留但尚未接通全部真实 surface 的窗口管理命令。

生产桌面必须使用 `PRISM_ENABLE_GLES=ON` 和 `PrismGLES` Skia 配置。CPU 配置保留为诊断测试构建，不构建这些生产客户端。

## 验证

单测仍在 `tests/`。旧 WM 控件镜像和软件帧测试随旧路径删除，保留树布局、装饰主题及层级仲裁测试，新增五个生产模板的解析/构建检查。

Pi V3D 应用套件探针：

```sh
python3 tests/probes/client_suite_probe.py build-gles
```

探针启动独立 headless 会话，检查五个 surface 的映射，并让一个普通客户端伪造 `prism_topbar` app_id，检查其仍是普通角色；测试结束后清理进程。该探针不进入安装包。

Pi 实测：CPU 诊断配置完整构建、CTest 12/12；GLES 完整构建、CTest 13/13。构建目录与临时安装前缀均通过五应用套件探针，客户端使用 `V3D 4.2.14.0`。安装套件探针报告 `configure=2 frame=2 presented=3 images=1/1`。WM 二进制符号检查未发现 ImGui、客户端 compiler/Scene/runtime 或 Skia；安装清单不包含测试与 probe。验收范围为 headless 会话，尚未验证物理显示的外观和帧时。
