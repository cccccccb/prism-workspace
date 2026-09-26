# Preferences、明暗配色及 Pi 性能测试

日期：2026-09-26。执行顺序：设置页 → 明暗配色 → 完整会话性能基线 → 根据测量专项修复。

## 1. 设置页职责

- 前端布局仍由 demo_settings/master.prism 描述，业务 module 只提交 bindings 和平台请求。
- 性能页显示真实 CPU、内存、GPU 时钟及 SoC 温度。GPU MHz 是频率，不是利用率；数据源不可用显示不可用，不编造百分比。
- 监控暂停和 0.5 / 1 / 2 秒采样选项必须影响实际读取及 one-shot timer。暂停后可消费已排队 tick，但不继续读取或排队。
- 外观页区分材质和配色。当前主题与配色使用选中标记；成功后不常驻 Theme 文本，失败保留简短提示。
- 分页使用 SDK 的通用 visible 属性，不能在 Scene、布局或渲染器中加入 Settings 专属规则。

## 2. 配色契约

- Theme ID 控制材料和形状；color_scheme 控制 light/dark 配色，两者独立。
- schema 2 的 Palette 覆盖已有同类型 token，然后重新解析别名、材料、控件和装饰。完整语法见 THEME_AUTHORING.md。
- Launcher 负责生成完整快照，沿用参与方 ACK、单事务和失败回滚。选择 theme ID 保留配色；选择配色保留 theme ID。
- Host API 以可选尾字段 select_color_scheme 扩展 V1；ThemeEvent 以可选尾字段公布确认的配色。旧 module 保持 ABI 兼容。
- 控制入口：prism-msg set_color_scheme light / dark；get_theme 同时返回 id、color_scheme、generation。
- WM 只接收 typed resolved snapshot，Skia 只绘制解析后的值；不识别 Palette AST、主题 ID 或应用名。

## 3. 测量边界

- get_status.performance 使用固定 240 个样本环；录入无分配和排序，查询时才汇总。
- frame_cpu 是 HandleOutputFrame 回调在 CPU 侧的经过时间，包含该回调的 Tick、效果更新、提交与 frame_done；不包含全部输入事件处理。effects_cpu 单独计材料/毛玻璃处理，commit_cpu 是 CPU 端提交调用耗时，均不是 GPU 时间。
- presentation.interval 来自每个输出的 wlroots present 时间戳，只统计成功呈现；不把显示刷新率或 frame 事件数当作客户端 FPS。
- pointer_event_age 是输入事件毫秒时间戳至 WM 开始处理的差，含毫秒量化误差；不代表鼠标至屏幕的端到端延迟。
- 独立 tests/probes/performance_session_probe.py 采样会话进程 CPU、PSS/RSS、DRM engine 累积时间与显存。DRM 按 driver/device/client-id 去重，避免同一 client 的多个 FD 重复；共享 buffer 跨 client 仍可能重复计入 resident，不代表唯一物理显存，不能与 CPU PSS 简单相加。
- 100% CPU 表示占满一核，四核 Pi 整机上限为 400%。GPU 引擎可并行，不能相加为单一 GPU 利用率。
- 测试前停止构建，条件包括：空闲、Music 播放、125Hz 相对鼠标移动、Glass 对比无模糊主题、明暗配色。控制采样周期，记录温度、CPU/GPU 时钟、throttled 位和背景负载。
- 输入探针仅产生移动，不点击；探针及测试代码不进入生产包。截图不与计时同时运行。

## 4. Pi 数据源

本机受限 /dev/vcio_gencmd 可由 video 组访问，通过固定只读 firmware 命令读取 v3d 时钟及 throttled 位，无需启动外部进程或扩大 /dev/vcio 权限。实现需校验 ioctl 响应码、长度及终止符，失败只降级对应指标。

参考官方实现：[vcgencmd](https://github.com/raspberrypi/utils/blob/master/vcgencmd/vcgencmd.c)、[Pi 内核 vcio](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/char/broadcom/vcio.c)。频率和温度变化不能单独证明瓶颈；历史降频位不能当作当前正在降频。

## 5. 实测结果

### 条件及证据

2026-09-26，Pi 4 Model B Rev 1.5，四核 ARM64，HDMI-A-1 1024×600 / 59.821 Hz，真实 V3D 4.2.14.0。完整生产会话包含 WM、launcher、三个 Shell host、Music、Preferences 和一个预热 host；两个 BSP 窗口。每个有效样本覆盖约 12 秒；指针对照为 11 秒，前后实际输入均约 125 Hz。未并行构建或截图。

CPU 按 MainPID 实际所属的 logind session.scope 读取，不能用空的 systemd service cgroup。PSS 是会话进程的采样峰值；表中回调 P95 是结束时最近 240 帧约四秒的窗口，不是整段测试分位数。端点 CPU 计数覆盖整段；Glass 暗和亮的逐秒观察分别有三个和两个中间采样缺口，不用于声称全程无尖峰。

原始 JSON、逐秒采样、输入发生器及截图保存于 `dist/validation/prism-v8-preferences/`。`performance/pointer-before` 未与输入发生器重叠、事件增量为零，已注明无效并排除；有效对照使用 `pointer-baseline/measure`。

### 已执行的修正

1. SDK 对祖先隐藏子树的 binding 更新保存最新数据，但不再触发无用重画；重新显示和可见共享目标仍正常失效。没有引入增量布局或分块缓存。
2. GLES 后端通过 `GlesRendererOptions.resource_cache_bytes` 提供通用 Ganesh 缓存预算接口，SDK/Host 可覆盖，默认 32 MiB。预算约束可回收缓存，不是 EGL、驱动或存活资源的硬上限；不逐帧清缓存、不强制 GPU 同步。

### 第一轮结果

| 条件 | 会话 CPU（单核 100%） | PSS 峰值 MiB | 末窗口 frame_cpu P95 ms |
| --- | ---: | ---: | ---: |
| 修正前 Glass 暗、外观页、暂停播放 | 6.72% | 229.37 | 1.813 |
| 修正后 Glass 暗、外观页、暂停播放 | 5.15% | 223.12 | 0.490 |
| 修正前 125 Hz 指针 | 8.32% | 233.45 | 1.728 |
| 修正后 125 Hz 指针 | 7.23% | 223.22 | 0.725 |
| 修正后 Square 暗、暂停播放 | 4.43% | 223.48 | 0.607 |
| 修正后 Glass 亮、暂停播放 | 4.71% | 223.85 | 0.659 |
| 修正后 Glass 暗、Music 播放进度更新 | 8.69% | 224.22 | 2.995 |
| 修正后 Glass 暗、性能页、监控开启 1s | 6.61% | 224.51 | 1.854 |
| 修正后 Glass 暗、性能页、监控暂停 | 5.00% | 224.57 | 0.689 |

Music 为业务进度 demo，不包含真实音频解码。各次修正后测试的 commit_failures 和 discarded 增量均为零，12 秒内实际呈现 717–718 帧。呈现间隔 P95 约 16.7 ms；Square 有一次末窗口最大 33.433 ms 间隔，尚未定位原因，不宣称所有帧均无抖动。最初约四秒的滚动窗口还包含测量前主题/分页切换历史，与末窗口分开解释。

Glass 暗空闲的去重 client resident 采样峰值从约 279.05 降至 115.05 MiB，指针对照从约 378.17 降至 114.29 MiB；这是各 client 的 resident 引用统计，不是释放的唯一物理内存。前后还同时改变隐藏页失效逻辑、会话及缓存预热历程，温度约 57–58°C 对 45–48°C，不能把全部差值单独归因缓存预算。短样本的 GPU 忙时未见显著回归；亮暗配色的 render/TFU 工作量接近，Square 无 TFU 工作量。

125 Hz 复测收到 1501 个事件 / 12 秒，末 240 个输入事件的到达 WM 年龄 P95 为 0 ms、最大 1 ms，受毫秒量化限制；这是处理入口指标，不是零端到端延迟。当前测量未复现持续输入排队，不能据此宣称现场鼠标卡顿已修复。起止 CPU 时钟 1.8 GHz，mask 0x50000 仅包含历史低压/降频位，起止未见当前降频位。

下一项专项应先记录真实鼠标的设备报告率、实际卡顿发生时输入年龄和 present 间隔，再检查光标硬件平面、客户端提交阻塞及显示链路；依据同一时间轴定位，不从主观卡顿直接推断 Pi 或毛玻璃是瓶颈。

## 6. 实机验收

最终 0.1.0-8 包已安装；30/30 CTest、真实 V3D 启动/呈现/激活协议检查与八种材料/配色组合通过。实际键盘 UI 检查覆盖分页、Light 全局切换、Music 播放及暂停、监控暂停/恢复和采样选择，截图与测量分开进行。用户现场答复“布局与配色满意，操作正常”，确认性能指标、采样/开关和全局明暗/材料切换清晰可用。主题和配色当前仍为会话级选择，尚不自动持久化。
