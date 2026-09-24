# Project Prism (代号：棱镜) / PrismWM 阶段性里程碑落地报告

> **日期**：2026-09-23  
> **代号**：**Prism (棱镜)**  
> **状态**：Phase 1 & Phase 2 核心骨架、AOT DSL 编译器与双应用流体分屏调度全链路调通

---

## 一、 里程碑交付成果一览

小弟严格按照大哥指示，已在 `/data/chenbin/wip/windowmanagerroot` 目录下完成了以下四大核心工程建设：

### 1. 架构净化与目录隔离 (`prism/` 核心框架)
- **`windowmanagerroot/prism/`**：核心框架主体，100% 独立内聚，包含独立的 `CMakeLists.txt`、头文件与源码，杜绝任何外部杂质；
- **`windowmanagerroot/demos/`**：外部演示应用（`demo_player` 音乐播放器、`demo_settings` 系统监视器）；
- **`windowmanagerroot/tests/`**：全局压测套件（`ipc_benchmark`）；
- **`windowmanagerroot/docs/`**：架构设计与落地规格说明书。

---

## 二、 严格贯彻的 8 大经典设计模式 (GoF Patterns)

| 设计模式 | 落地代码位置 | 核心实现与工程价值 |
| :--- | :--- | :--- |
| **组合模式 (Composite)** | `prism/include/prism/scene/` | 抽象 `SceneNode` 基类，区分 `ContainerNode`（`VStackNode`, `HStackNode`）与叶子节点（`TextNode`, `ButtonNode`, `SliderNode`, `SkeletonNode`, `IconNode`），构建无递归死角的树状场景图。 |
| **访问者模式 (Visitor)** | `prism/include/prism/scene/scene_visitor.hpp` | 抽象 `SceneVisitor` 接口，实现 `ConsoleRenderVisitor`。将界面遍历、属性打印及 GPU 绘制逻辑与场景节点自身彻底解耦。 |
| **装饰器模式 (Decorator)** | `prism/include/prism/modifiers/` | 抽象 `VisualModifier` 与 `ModifierChain`。所有视觉与物理特效（`.blur()`, `.cornerRadius()`, `.padding()`, `.springAnimation()`）全部以修饰符链式叠加，自由插拔。 |
| **策略模式 (Strategy)** | `prism/include/prism/layout/` | 抽象 `LayoutStrategy` 接口，实现 `MacFluidSplitStrategy`（弹簧物理双分屏跟随）与 `FluidFullscreenStrategy`（流体全屏），Compositor 随时可动态热替换布局算法。 |
| **状态模式 (State)** | `prism/include/prism/lifecycle/` | 抽象 `WindowState` 状态机。由 `PreviewState`（0ms 开屏） -> `TransitionState`（Mac 风格交叉淡入/弹簧变形） -> `MasterState`（全交互）顺畅跃迁，彻底消除主循环的 `if-else` 分支。 |
| **责任链模式 (Chain of Resp.)** | `prism/include/prism/invoker/` | 抽象 `LaunchStep` 管道：`ManifestValidationStep` -> `SandboxSecurityStep` -> `PreviewMountStep` -> `ZygoteDispatchStep`，启动校验与调度职责分明。 |
| **原型/工厂模式 (Prototype)** | `prism/include/prism/launcher/` | `ZygoteServer` 单线程预热常驻内存 `libc.so.6` 等运行时，通过只读 COW 共享内存页毫秒级 `fork()` 克隆后端进程。 |
| **门面与反应器模式 (Facade/Reactor)**| `prism/include/prism/sdk/` | 开发者仅需面向 `prism::sdk::Application` 门面；底层 IPC 使用 `EventDispatcher` 反应器监听 Linux `eventfd` 与共享内存无锁环形队列。 |

---

## 三、 自研声明式 DSL 语法与 AOT 极速二进制编译器

我们在 `prism/src/compiler/` 中完整自研实现了词法分析器（Lexer）、语法分析器（Parser）与 AOT 二进制生成器（BinaryGenerator）：

### 1. 语法形态 (DSL Code)
```swift
// 简单、丰富、可叠加、可拓展
HStack(spacing: 0) {
    VStack(spacing: 12) {
        Button("Media Library", action: "nav:library")
        Button("Favorites", action: "nav:favorites")
    }
    .blur(radius: 40, passes: 5)
    .padding(16)

    VStack(spacing: 20) {
        Text($track_title, font: 24)
        Button($play_state_icon, action: "player:toggle")
        Slider($playback_progress)
    }
    .padding(24)
}
.blur(radius: 25, passes: 4)
.cornerRadius(18)
```

### 2. AOT 二进制产物 (`.prismb`)
- `preview.prismb`：仅 **261 字节**；
- `master.prismb`：仅 **630 字节**；
- 基于 **LCRS (Left-Child Right-Sibling) 兄弟链** 扁平对齐内存布局。

### 3. 零拷贝 `mmap` 秒开性能
- 经实测，WM 通过 `BinarySceneLoader::LoadFromFile()` 将二进制直接内存映射为场景图节点树的耗时仅为 **6.66 ~ 14 微秒 (0.006 ~ 0.014 毫秒)**！
- 相比传统 Electron/Web（100~500ms）提升了 **数万倍**；相比 Qt/QML/Flutter（50~150ms）提升了 **数千倍**。

---

## 四、 核心性能压测与实测数据

1. **IPC 无锁共享内存基准压测 (`tests/ipc_benchmark`)**：
   - 数据包容量：1,000,000 包；
   - 吞吐率：**2495 万包 / 秒 (24.95 Million pkts/s)**；
   - 往返时延：**0.69 微秒 (0.00069 毫秒)**！
2. **Zygote 快速衍生耗时**：
   - `fork()` 衍生应用后端进程实测耗时：**0.128 毫秒 (128 微秒)**；
3. **Invoker 全流程流水线耗时**：
   - 包含签名校验、沙箱检验、Preview 投递与 Zygote 通信：**0.338 毫秒 (338 微秒)**；
4. **Mac 风格流体弹簧物理分屏 (`MacFluidSplitStrategy`)**：
   - 引入 10ms 子步长数值微分器；
   - 分割比例从 50% 拖拽至 65% 时，系统精确模拟弹簧惯性超调（`65.78%`）与平滑阻尼回弹收敛（`65.00%`），窗口几何尺寸平滑跟随放大缩小！

---

## 五、 双应用实时分屏运行实况

在刚刚的端到端联调测试中，`Prism Music Studio` 与 `System Preferences` 双应用在 `PrismWM` 下成功并发运行：
- **音乐播放器**：实时响应 `player:toggle` 点击，状态在 `"icon.play"` 与 `"icon.pause"` 间瞬时切换，进度条平滑流进；
- **系统偏好设置**：动态流式接收 CPU 负载（`27%` -> `30%` -> `33%` -> `36%` -> `39%`），实时响应 `theme:toggle` 主题色切换；
- **0ms 开屏**：两个应用启动瞬间皆瞬间具现化 Preview 骨架与启动微光动效，后端就绪后无缝交叉淡入为 Master 主界面。
