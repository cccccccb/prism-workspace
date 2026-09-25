# Prism 客户端渲染架构迁移计划与树莓派基线

> 状态：架构实施约束；2026-09-25 首次硬件基线。本文记录已测事实和迁移验收标准，不代表 Skia 性能已经验证。

## 1. 目标与边界

最终交付只有一条生产 UI 路径：Prism DSL → 客户端 retained-mode 运行时 → Skia → Wayland surface → WM 合成。WM 管理窗口、输出、输入焦点、布局和自身窗口装饰；不解析应用 DSL，不保存应用控件树，不处理应用 `$slot`，不绘制 topbar/dock/desktop 的业务内容。客户端在自己的进程中处理 UI 状态、布局、命中测试、文字和绘制。

Skia 是绘图后端，不是控件系统。核心 Scene、布局、事件和 DSL API 不出现 `Sk*`、`Vk*` 或 `wl_*` 类型。Wayland 与图形后端通过明确的接口连接运行时。普通应用使用 xdg-shell；desktop/topbar/dock 使用受 WM 管理的 shell surface 角色，确定锚点、保留区域、输入策略和权限。跨窗口的 backdrop blur 属于合成器能力，不能假定客户端能够读取其身后的内容。

迁移在独立开发分支完成。合并到主线时必须一次性切换所有生产目标：删除 ImGui 源文件及链接、WM 中 topbar/dock 的 DSL 与专用绘制、WM 中应用控件树镜像及状态差分渲染。任何中间阶段都不作为同时包含两套生产 UI 路径的版本交付。开发分支可以使用测试桩和临时探针，但不能将它们变成长期兼容层。

## 2. 本机硬件与测量事实

本次测量在项目所在的 Raspberry Pi 4 Model B Rev 1.5 上进行：AArch64、4 个 CPU 核、3.7 GiB 内存，Debian 13，内核 `6.18.34+rpt-rpi-v8`。SSH/TTY 会话没有运行 Wayland 图形会话。采样前负载为 `0.23/0.25/0.37`，CPU0 当前频率与上限均为 1.8 GHz，温度约 40–43 °C。系统存在 `/dev/dri/renderD128`；Vulkan 枚举到硬件 `V3D 4.2.14.0`（API 1.3）和软件 `llvmpipe`。后者不可用来代表 GPU 性能。

直接编译项目的 `FrameBuffer` 实现，在本机 `g++ -O2` 下测量；每项有 3 次预热，统计中位数。此测试不包含 Wayland、Skia、GPU 提交或显示扫描输出。

| 当前软件绘制操作 | 尺寸 | 样本 | 中位数 | p90 |
| --- | ---: | ---: | ---: | ---: |
| `DrawDesktopGradient` | 1920×1080 | 11 | 28.273 ms | 28.390 ms |
| `DrawTopMenuBar` | 1920×38 | 21 | 0.611 ms | 0.616 ms |
| `DrawMacDock` | 612×104 | 21 | 1.298 ms | 1.309 ms |

这些值只说明现有 CPU 绘制函数的成本。壁纸若缓存而不逐帧重算，28 ms 不构成每帧成本；topbar/dock 的结果也不包括 ImGui DSL 叠加、纹理上传或合成器提交。测试程序仅在 `/tmp`，未加入项目。

已安装 `vkmark 2025.01-1` 作为设备测试工具，明确选择 Broadcom Vulkan ICD。其无头 swapchain 在 1920×1080 和 800×600 均于 `vk::Device::createSwapchainKHR` 返回 `ErrorOutOfDeviceMemory`。原因尚未确认；这不能推断 V3D 在实际 Wayland 会话中无法工作，也不能给出可信的 GPU 吞吐量。下一次性能门槛必须在真实 Wayland 会话中测量 Skia 首帧、稳定帧、resize、文字、阴影和模糊。

首次测量时仓库缺少 `prism/core` 的三个头文件及 `logging.cpp`；这些文件已依据调用点重建。根目录 `.gitignore` 曾用裸 `core` 规则忽略所有同名源码目录，现已限定为根目录的 crash dump。固定的 `x86_64-linux-gnu` wlroots 路径已删除，Pi 上使用系统 wlroots 0.18；完整构建与 6 项测试通过，headless 后端持续提交画面并正常退出。构建步骤见 [PI_BUILD.md](PI_BUILD.md)。物理 DRM/KMS 显示输出仍需在真实会话中验收。

## 3. 六个实施步骤

### 第一步：恢复可构建基线，冻结验收样例

补齐仓库缺失的核心源码与头文件，清理架构相关硬编码路径，建立 ARM64 依赖锁定与构建说明。固定一个普通应用、topbar、dock、desktop 的 DSL 样例和窗口行为。产物是能在树莓派上复现的构建与测试命令，以及可比较的截图/时序记录。此步不改变生产渲染路径。

当前结果：Pi 构建、测试与 headless 运行检查已完成；样例哈希、旧软件快照和窗口行为记录见 [BASELINE_ACCEPTANCE.md](BASELINE_ACCEPTANCE.md)。真实 DRM/KMS 截图与呈现时序仍在第六步的实机验收范围内。

### 第二步：固定进程协议与模块依赖

定义 DSL/运行时、平台、渲染器和 WM 之间的数据契约：WindowId、NodeId、逻辑尺寸与 buffer 尺寸、输入事件、资源句柄、绘制命令及窗口角色。SDK 只向业务暴露应用状态、动作、窗口和资源接口。业务状态更新留在客户端；WM IPC 只承载窗口管理命令和明确授权的 shell 服务。用 CMake target 依赖与 include 检查阻止 WM 链接 DSL、Skia 或 ImGui。

进程边界与首版无后端值契约见 [PROCESS_CONTRACTS.md](PROCESS_CONTRACTS.md)。由于旧 `prism_core` 仍是单体，最终 WM 链接禁令在第五步删除旧渲染路径时启用；新的契约 target 从本步起独立构建。

### 第三步：完成真实 Wayland 客户端生命周期

SDK 连接 registry，创建 `wl_surface` 与 xdg-shell toplevel，处理首次空提交、configure/ack、buffer 提交、frame callback、resize、关闭和输入。shell 客户端的层级、锚点与独占区域另行实现并限制授权。WM 为每个真实 surface 建立窗口记录，管理映射/解除映射、焦点、键盘和指针命中，并把 tiling 几何通过 configure 发给客户端。验收条件：不读取 DSL 的普通测试窗口可显示、调整大小并接收输入。

### 第四步：实现客户端 DSL 运行时与 Skia 后端

AST 只表示语法；长期 Scene 保存节点状态、绑定、焦点与布局；渲染数据通过独立后端接口送给 Skia。首版先覆盖固定尺寸、row/column、文字、图片、矩形、圆角和基本裁剪。属性变更至少区分 Layout、Paint、Composite；静止页面不持续提交。先用一个单线程端到端路径保证正确性，再依据测量引入 Render Tree、DisplayList chunk、缓存或渲染线程。Skia Vulkan 与 OpenGL ES 在目标设备上做功能与帧时对比后，确定实际发布后端；CPU Skia 可作为诊断路径，但不能用软件结果宣称 GPU 性能。

### 第五步：迁移全部应用并一次性切换生产路径

普通 demo、desktop、topbar、dock 全部通过 SDK 自主解析 DSL、绘制与提交 surface。启动器按包元数据启动客户端；dock 的启动动作必须真正调用启动服务。WM 只合成 surface 并绘制自身装饰。此步骤完成时，删除 ImGui 及 `imgui_sw`、WM 对应用 AST/`$slot` 的持有、WM 内 topbar/dock 解析与绘制、演示应用的硬编码路径。构建产物中不得再出现生产 ImGui 引用。

### 第六步：树莓派真实会话验收与收尾

在目标分辨率、目标刷新率和实际驱动下，分别测首帧延迟、空闲 CPU/GPU 占用、静态页面唤醒率、输入到呈现延迟、动态列表、文字、透明合成、阴影、blur、resize、内存峰值和温度/降频。记录 p50/p95/p99 与测试场景，区分 CPU 准备、Skia 提交、GPU 完成和显示呈现。若有失败，先定位具体阶段，再优化。完成后更新设计文档中与实现不符的性能数字，并删除旧路径与测试桩。

## 4. 完成定义

- `prism-wm` 不链接 DSL 解析器、ImGui 或 Skia；只处理客户端 surface 和 WM 自身视觉。
- `prism_sdk` 负责客户端 Scene/布局/事件/渲染，并能提交真实 Wayland buffer。
- desktop/topbar/dock 与普通 demo 均使用同一 SDK 渲染契约；角色和权限差异只在平台层。
- 主线仅有 Skia 生产 UI 路径，构建与发布包不包含 ImGui。
- 树莓派真机有可重复的功能测试、性能记录和失败场景记录；GPU 结论来自硬件驱动，而不是 llvmpipe。

长期模块划分及性能优化顺序以附带的《Wayland DSL 客户端渲染引擎总体架构设计》为参考；本计划的阶段门槛和本机测量事实优先用于实施判断。
