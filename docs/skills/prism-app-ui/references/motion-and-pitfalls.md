# 主题动效、版本边界与常见陷阱

适用日期：2026-10-04。目标是仅凭这份 skill 和匹配版本的 SDK/Host 编写应用，源码
链接用于维护和深入审计，不是完成普通 UI 的前置阅读要求。

## 1. 先声明所需能力

| 能力 | 当前可依赖的范围 |
| --- | --- |
| 已部署 v19 | Interface v2、typed bindings、Host 业务 work、主题 schema 3、命名 Transition、InteractionTarget/Visual/.state、鼠标手势 |
| 系统空间运动 | 单输出单窗全屏、普通 BSP 组沉浸往返、窗口控制面板开合；由 WM 执行 |
| 工作区新增文本能力 | TextField/TextArea、typed edit/close 回调及 document/save 等新图标；需要匹配此次源码构建，不能假定 v19 已包含 |
| 尚不能假定存在 | 通用滚动/虚拟列表、viewport binding/断点、tooltip、自动无障碍名称、通用 modal、完整 IME/系统剪贴板、声明式 spring/keyframes、自动入场退场动画 |

发布说明必须注明测试过的 Host/包版本以及 theme/motion 依赖。Interface(version:2)
只声明加载格式，不能证明运行环境包含之后新增的控件。C ABI 尾字段用 struct_size 和
非空函数指针检查；DSL 未知组件在准备期失败，不会被 C ABI 能力检查自动修复。

## 2. 主题、动效、业务的所有权

- 主题提供颜色、材料、字号和尺寸；light/dark 与材料独立。
- MotionSet 提供语义名对应的时长和缓动。应用引用语义，不读取/修改系统 motion 文件。
- 业务只发布目标状态。UI 所有者按单调时间计算动画；渲染消费已准备的帧，不靠业务
  schedule_tick 每帧推值。迟到帧直接取当前时刻的样本，动画时长不依赖帧数。
- WM 负责系统窗口空间运动。应用不能借 `window.geometry` 名称改变 BSP，也不能
  在自己根节点缩放一遍来配合 WM 全屏，造成两套相互冲突的几何动画。

当前系统 motion 包：prism 空间 240ms/反馈 120ms；subtle 160ms/90ms；instant 0ms。
它们是可替换配置，不是引擎里的固定 ID 分支，也不是操作系统级减弱动效偏好 API。
Preferences 暂无独立 motion 选择器。应用作者不要要求用户手工改系统文件才能正常启动。

| 语义名 | 合理用途 | 边界 |
| --- | --- | --- |
| control.feedback | 悬停、按下、键盘焦点的轻反馈 | 首选，避免每个按钮独立定义曲线 |
| panel.visibility | 适用属性上的内容显隐反馈；系统控制面板开合 | 不会自动监听组件 mount/unmount 或延迟 visible=false |
| window.geometry | 系统单窗全屏/恢复 | 普通业务无布局权限 |
| group.geometry | 系统组沉浸过渡 | 普通业务无组控制权限 |

命名引用缺失会导致 Scene 准备或主题事务失败，不能暗中回退另一曲线。跨自定义主题
交付时把所需名称列入契约；有意使用独立时长可以采用旧字面量形式，但应说明为什么
不跟随系统。一次 transition 不能同时写 motion 与 durationMs/easing。

## 3. 推荐动效幅度与实现方法

这些是界面推荐值，不是新增 schema 属性：

| 情况 | 建议表现 | 实现 |
| --- | --- | --- |
| hover | 背景轻变，或图标缩放到 1.04 | Visual + .state + control.feedback |
| press | 图标缩放到 0.96 | 与 hover 使用同一属性，由状态优先级决策 |
| 键盘焦点 | 独立颜色/轮廓，持续可辨认 | focusVisible 使用与 press 不冲突的属性 |
| 选中 | 4px 横线或清晰背景 | bool binding + visible，保留指示槽尺寸 |
| 任务进行中 | 静态状态图标/真实进度，必要时轻过渡 | 有真实数据才发布 value，不伪造循环 |
| 结果更新 | 保留布局，局部状态变化 | 字符串替换不自动成为数字滚动或文字交叉淡化 |

完整可用的固定目标示例见 [compact-library.prism](../assets/examples/compact-library.prism)。
其外层 InteractionTarget 32×32 永远不缩小，内部 Visual 18×18 变化；foreground
处理焦点，scaleX/Y 处理 hover/press，避免状态竞争。

Visual 可动画 opacity、translateX/Y、scaleX/Y、background；指定绘制节点可以动画
foreground，Progress 可动画 value。width/height/padding/spacing/font、材料、blur、
shadow、资源 source、visible 不是当前通用 Transition 目标。Visual 不是交互容器，
里面不能放 action、InteractionTarget、Slot、material、backdropBlur 或 inputShape。

visible=false 会立即退出布局、绘制和命中；opacity=0 不会自动禁用外层点击区。
不能写一个 opacity transition 就声称有可靠退出动画。需要真实退场生命周期时应设计
通用呈现完成/输入撤销接口，不能在业务中硬等 120ms 再删界面。

首次组件安装通常直接呈现初值，deferred 完成不自动触发入场。主题切换按既有事务
取消/归位，不把“所有颜色平滑跨主题渐变”当作已实现能力。当前客户端局部动画仍有
Paint/DisplayList/Skia 回放成本，不等同 GPU 保留层合成动画。

## 4. 常见问题与正确处理

| 现象/错误假设 | 处理方式 |
| --- | --- |
| Glass 好看，Light/Square 看不清 | 用语义 token/material；四材质×明暗实际检查，不硬写白字、圆角和白蒙版 |
| 窗口阴影很重、边框像画了两遍 | 根 window，WM 外框负责外阴影；应用只画局部分组层次 |
| 模糊区域过多、移动时成本高 | 根尽量只请求一份背景效果，内容用 card/control；当前每 surface 最多 8 区域不是推荐数量 |
| 缩放按钮后点不中/抖动 | 固定 InteractionTarget，Visual 只负责装饰运动 |
| 隐藏视觉后点击仍生效 | 区分 visible 与 opacity；业务也需拒绝 pending/失效动作，不能仅靠淡色 |
| Text 周围 padding 没效果 | Text 无 padding；用容器负责留白 |
| clip 后以为内容可滚动 | overflow:clip 只裁剪；新 TextArea 的内部滚动也不代表普通 Card 能滚动 |
| 想用 CSS、百分比、字体加粗 | 使用真实属性与布局盒；不发明 fontWeight、lineHeight、百分比 width |
| 改主题后应用自己恢复旧色 | 应用不复制全局色表，不用应用级缓存覆盖 Host 新快照 |
| 请求主题后立刻高亮新项 | 等 Current/Applied；Rejected 保留实际旧选择 |
| 图标名拼对意思却报错 | 只用合法名称；业务 binding 更新后也必须合法 |
| Preview 已显示但主功能不能用 | Preview、Master、BackendReady、deferred 就绪分别表达，不假成功 |
| create 首次加载失败 | 不在静态初始化/create 同步读大文件或加载重库；查询/create 有返回后预算检查，当前默认 20ms，不是抢占式超时 |
| 用 tick 驱动按钮动画 | 状态/目标绑定交给动画引擎；tick 只做有需要的业务采样并能停止 |
| work 完成后窗口已关闭 | 使用 Host work 的复制数据、取消与 owner 完成回调；不捕获悬空实例或直接操作 Scene |
| 选中条切换导致布局跳动 | 保留固定高度槽，仅隐藏槽里的条 |
| 中文显示方块 | 检查实际字体覆盖；文本编码有效不能代替字形和 IME 验证 |

## 5. 第三方交付验收

1. 仅用 skill、公开 SDK/Host 和指定主题包即可解释每个组件、binding、action；
   不要求另一个开发者通过阅读 Music 源码猜测参数含义。
2. 说明字号、间距与图标/点击范围；关键动作在窄窗和矮窗都存在且能点击。
3. 声明所需材料/token/motion，不依赖 demo 专用 musicAccent/settingsAccent。
4. 检查四材料、明暗、instant；动作结果来自真实业务，快速重复操作和取消能收敛。
5. 将语法/主题校验、布局命中、业务反馈和真实 GPU/显示验收分别报告。字体近似度量
   的布局测试不能证明真实字形不溢出，截图也不能证明点击链路正确。
6. 测试和示例不进入生产包，未实现的能力在交付说明中明确；不安装新包或改当前桌面
   来验证文档示例，除非当前任务已经要求部署。
