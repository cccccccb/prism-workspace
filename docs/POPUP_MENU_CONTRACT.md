# Popup / Menu：定位与生命周期契约

## 当前实施状态

对应 `design/interface-system/02-popup.png`。当前源码推进到 4k3；以下按阶段记录能力：

1. **已实现 4a**：`PlacePopup` 定位与 `PopupSession` 生命周期核心。
2. **已实现 4b**：同窗口单层 `Popup` DSL、Scene 浮层布局、快照门控、外点消费、Esc 和焦点返回。
3. **已实现 4c**：Menu/MenuItem/MenuBack，替换面板式子菜单与命令导航。
4. **已实现 4g**：显式局部 Contour，绘制/命中/输入/effect 使用同一不可变几何。
5. **已实现 4h**：按最终定位生成 attachedPanel 轮廓，主题控制圆角与连接颈尺寸。
6. **已实现 4i/4j/4k1**：同连接传输、共享 GPU target、不可变 Scene/Host 请求与最终配置计划。
7. **已实现 4k2**：Host 自动 native 生命周期、实际提交后的输入采用、root 退场及独立 child 调度。
8. **当前源码 4k3**：跨 surface backdrop/effect 与 parent 正文采样；级联侧向子菜单及触控另行设计。

匹配的 `0.1.0-26` Host/WM 已部署到 Pi 正式远程会话；旧 v22 不具备这些新增能力。
部署记录见 [PI_REMOTE_DESKTOP](PI_REMOTE_DESKTOP.md)，最终视觉仍待用户确认。
scale=1、可准备局部布局且 compositor 声明所需能力的面板自动使用 native
child；需要背景效果时另要求 v3 popup backdrop capability，Contour 要求 contour
capability。首个实际 child Pixels 与 Scene adoption 成功后才移除 root 面板，再等
clean-parent pixel baseline 开启 blur。旧能力、编辑器和不支持的裁剪/变换保留 root。
完整顺序与坐标规范见 [原生背景效果](POPUP_BACKDROP_CONTRACT.md)。

4i/4j 已提供内部同连接传输与共享 GPU target；4k1 的
[不可变 surface 计划](POPUP_SURFACE_PLAN_CONTRACT.md)接入 Scene/Host 请求，并支持
按最终配置准备子树。原生 window geometry 包含功能连接颈，buffer 另外保留阴影。
4k1 用独立 native probe 验证该计划。4k2 通过 `PopupFramePacket` 发布不可变计划与
自己的图片版本清单；公开局部输入仍为 `scene=0`，必须经 Host 成功提交身份和
prepared provenance 采用，不能直接送 root 输入入口。后续 State/checked None
仅在已有真实 pixel 基线后更新输入。各 target 独立 gate、callback、反馈和损伤历史；
当前实现、能力回退及关闭屏障见[子层生命周期规范](POPUP_TARGET_LIFECYCLE.md)。

## 设计与职责

- Popup 表达锚点相关的临时操作；Menu 可以组合命令、Segment、Slider 和选择项。
- 命令执行结束关闭整条菜单分支；连续调值与选择视图默认保留展开。
- 普通应用只能创建自己的临时层，不得借 Popup 模拟桌面级模态权限。
- DSL/主题表达内容、文字、间距、背景、边框与轮廓；运行时负责生命周期与几何。
- Host 提供可信 owner、场景身份及可用逻辑区域。WM 不解析具体应用 DSL。
- 下方优先，上方回退，再横向避让和限高；无法保持基本可操作尺寸时转贴边面板。
- 沿用正文 14、辅助 11、局部间距 4/8/12/16 的设计尺度，标题与右侧读数对齐。
  图稿 28px 轮廓只是视觉提案，不作为所有材质的固定圆角。Square 随主题保持直角。
- 连接颈最终需要轮廓、命中、裁剪和背景效果一致；不能先画出不可交互的假连接颈。

## 定位核心

`PopupPlacementRequest` 接收同一逻辑坐标空间的 available、anchor、desired、最小
可用宽高、margin、gap 和 horizontal_alignment（默认 Start，配方使用 Center）。available 是适配器提供的许可区域，不代表算法能查询或
突破窗口边界。它继续描述同窗口回退定位；4k2 native 路径由 Host 提交可信锚点和
positioner 偏好，再以 compositor 的最终 configure 准备正文与连接颈。

返回 `PopupPlacement`：bounds、Below/Above/EdgePanel、宽高是否受限及裁到 available 的
effective_anchor。Scene 在布局后保存该结果，几何准备不再从 authored width 或未裁剪
锚点推算定位。OpenPopup 的预检只决定可用性，不作为最终轮廓来源。

1. 输入必须有限且正尺寸；margin/gap 非负，minimum 不大于 desired。
2. 可用区域扣除 margin 后必须仍有正面积；完全不可见的 anchor 拒绝定位。
3. 下方放得下则使用下方，否则尝试完整上方。
4. 两侧都不足时选择空间较大的一侧并限高；水平夹紧至安全区域。
5. 安全宽度小于 minimum_width，或两侧都小于 minimum_height，使用底部贴边面板。
   面板采用安全区域宽度，高度不超过 desired 和安全区域；极短窗口仍只能使用现有空间。
6. 限高只返回几何约束。调用方需要实际 ScrollView，不能把内容缩放到无法操作。

算法不读取主题、不绘制、不查询屏幕、不做动画。使用负屏幕原点、小数逻辑尺寸时
规则相同；无有效位置返回空结果，由适配器取消本次展开。

## 生命周期核心

每个 owner 生命周期拥有一个 `PopupSession`。owner 是 Host 分配的非零生命周期
身份，不能复用可变窗口索引。`PopupIdentity` 进一步包含 scene 身份及带 generation
的锚点 NodeId。此处身份是内部类型，不是应用可提交的权限凭证。

- Open 无 parent：替换现有根与其子级。
- Open 指定有效 parent：保留祖先，替换 parent 下方已有分支。当前核心限制同一
  Scene 的锚点链；跨 Scene 子菜单需要适配层扩展，不能绕过校验。
- 每次 Open 生成不复用 token；旧 token 的命令/关闭不能影响新层。
- Escape 只关闭最深一层。Command 接受仍存活的 token 后关闭整条链。
- 连续值更新不调用 Command，因此 Slider 调值不会关闭面板。
- Invalidate 精确匹配身份，关闭该锚点及其所有后代；相同 index、不同 generation
  不视为同一锚点。owner 隐藏/退出通过 CloseAll(Unavailable) 结束全部层。
- 关闭输出按最深到最浅排列，含关闭原因及返回焦点候选。查询与排空在 owner 线程执行。
- 核心只记录 return_focus。适配层必须验证它仍属于正确 Scene 且可交互；批量关闭时
  只恢复最终保留层的候选，不能对每条关闭记录轮流抢焦点。无效候选回到稳定入口。

## 输入接入要求与后续范围

以下是跨阶段保留的交互要求；单层基础见历史 4b，菜单见 4c，当前 native 输入与
能力边界见[4k2 生命周期规范](POPUP_TARGET_LIFECYCLE.md)：

- 输入按照已提交的浮层快照命中，存活状态可否决旧 token；旧帧不能恢复已关闭的层。
- 外点按下关闭，并消费同一 source/button 的整次按下和释放，防止底层收到穿透点击。
- 拖动期间保留捕获；松开在浮层外仍结束值流，不作为新的外点关闭。
- Esc 逐级返回；关闭取消该层捕获、按键和预览，归还有效焦点。
- 命令菜单支持上下导航/Enter；左右进入和返回子菜单。组合面板使用 Tab，方向键交给
  当前值控件。禁用项保留外观与原因，但不激活。
- 非模态 Popup 不阻挡其他应用窗口；窗口失活如何处理由 Host 策略明确。
- 临时层从普通布局流分离，不能修改正文自然尺寸；避免被触发控件的 clip 裁掉。
- 根末尾 Popup/Menu 与普通内容裁剪容器并列，根保留 window 材质。原生子层可能
  连同阴影伸出父窗口，祖先 clip 必须可准确保持；不满足时回退，不能忽略 authored
  clip。Preferences 的真实菜单已采用此组合，示例见应用视觉规范 3.1。
- 显示、输入和损伤使用同一功能轮廓与坐标变换，外阴影不扩大输入。4h/4k1 已接
  连接颈；4k3 已把跨 surface 背景效果接入该边界，不以 demo 私有 Skia 分支实现。

## 4a 历史验证

`tests/popup_core_test.cpp` 覆盖上下翻转、横向避让、限高、贴边、失效锚点、非法数值、
负原点/小数边界，以及根替换、子菜单替换、逐级 Esc、整链命令、失效关闭、旧 token
拒绝和锚点 generation 隔离。该核心阶段记录不包含后续 Scene/Host 输入、GPU 或
视觉验收；新阶段结果由[执行文档](INTERFACE_SYSTEM_PLAN.md)单独记录。


## 4b：可用单层 Popup

本节记录首次同 surface 接入。当前保留其 DSL 与同窗口回退语义，native 交接规则以
4k2 为准；不要把下面当时的能力限制当作当前源码状态。

```prism
VStack {
    Button("Sound", action: "sound", height: 36)
    // 其他正文内容先声明，Popup 声明统一放在根节点的最后。
    Popup("sound", width: 280, height: 220, padding: 12,
          background: "@controlField", cornerRadius: "@control_state_radius") {
        ScrollView {
            VStack(spacing: 8) {
                Text("Sound", height: 24, font: "@font_body", foreground: "@text")
                Button("Output settings", action: "output", height: 36)
            }
        }
    }
}
```

- `Popup("sound", ...)` 等价于 `popupFor: "sound"`，身份为固定字面量；必须匹配
  正文中恰好一个 action 目标，不能匹配值控件、Visual 或另一个 Popup 内的控件。
- 声明必须为根的直接子节点，全部放在正文节点之后；必须给出正 width/height。
  内部为普通 Card 式组合容器，需要纵向流时显式放 VStack，可能限高时放 ScrollView。
- 默认关闭。触发 action 由前端消费，不再作为业务 on_action 发送；内部命令照常发送
  业务，并关闭 Popup。Checkbox/Slider 等值操作仍使用值事件并保留浮层。
- `visible`/`enabled` 表达可用性，不表示 open 状态；open/token 属于前端 owner 线程。
  `Scene::OpenPopup(anchor, seat)` 与 `ClosePopup(reason)` 提供内部 C++ 控制入口。
- 当前一次只显示一个根 Popup。普通布局测量跳过声明，随后按锚点位置单独测量/放置；
  Popup 使用强制裁剪，不突破 owner 根区域。文本、控件和背景仍走普通 DisplayList。
- 外点按下关闭并记录 source/button，直到相应释放前消费该按键；避免关闭后底层误点击。
  松开不作为外点。拖动控件的正常释放仍由控件流接收。
- 输入快照包含 popup token。快照与当前 token 不同的输入被拒绝；新 Popup 不接受旧层
  坐标。关闭后旧帧仍显示 Popup 时，也不把点击解释为正文操作。
- Tab 在当前 Popup 内循环；Esc 关闭并消费重复/释放。打开选择首个可交互目标，关闭
  返回触发控件；触发控件失效则清空焦点，后续 Tab 选择正常入口。
- owner 失焦/关闭、锚点隐藏/禁用/更换 action、位置不可用会关闭。锚点所在滚动内容
  移动时关闭，避免留下与锚点脱离的面板；Popup 内部自己的 ScrollView 不触发该规则。
- 同节点的主题/Preflight 更新保留 open token。只支持鼠标/键盘；展开期间不接受触控。
- Popup 开启时，owner 根区域纳入输入区域，使透明正文的外点也能被接收；不会扩大到
  其他应用窗口。圆角裁剪、模糊和命中复用既有通用区域能力。

示例 `tests/fixtures/interface-system/popup.prism` 参考图 02 的声音面板，保留标题、
右侧读数、辅助说明、细滑轨和紧凑留白。尚未制作连接颈、子菜单和新动效。

当前根声明约束也适用于组件加载：不能把 Popup 单独作为 Slot/region 片段安装，
也不能通过移动声明绕过根顺序校验。后续组件引用和子菜单需要显式扩展该契约。


## 4c：Menu 与替换面板式子菜单

本节记录替换面板模型；4k2 继续每次只显示一个逻辑面板，新 epoch 对应新的 native
lifetime，原生 parent 始终为 root，不把已隐藏的逻辑父菜单变成新的原生 parent。

本节扩展 4b 的单层限制。`Menu` 沿用 Popup 的根末尾声明、popupFor 关联、显式尺寸、
窗口内定位与主题契约；`Popup` 仍不支持嵌套。所有子菜单也在 Scene 根末尾声明。

```prism
VStack {
    Button("View", action: "view", height: 36)
    Menu("view", width: 280, height: 240) {
        VStack(spacing: 8) {
            MenuItem(action: "open", height: 36) { Text("Open file") }
            MenuItem(action: "sort", height: 36) { Text("Sort by") }
        }
    }
    Menu("sort", width: 280, height: 240) {
        VStack(spacing: 8) {
            MenuBack(height: 36) { Text("Back to view") }
            MenuItem(action: "name", height: 36) { Text("Name") }
        }
    }
}
```

上面仅展示结构，完整主题、焦点提示和图标组合见
`tests/fixtures/interface-system/menu.prism`。MenuItem/MenuBack 是交互容器，文字、图标和
状态 Visual 显式组合，推荐 36–44 高的行与 8 的局部间距；不要省略文字只留无解释图标。
MenuBack 是前端返回角色，不声明业务 action；每个子菜单必须包含可见可用的返回入口。
结构验证拒绝菜单锚点循环、菜单外的行、无命令 MenuItem，以及缺少 MenuBack 的子菜单。

### 导航与业务边界

- 打开后聚焦首个可交互目标；Tab 在当前面板的命令和值控件间切换。
- 当焦点在 MenuItem/MenuBack：上下循环命令行并跳过禁用项，Home/End 到首尾；
  右键进入关联子菜单，左键返回上一级。Enter/Space 沿既有按下/释放规则执行。
- 焦点在 Slider、Radio 等值控件时，方向键由控件处理，不被菜单抢走。
- 子菜单替换当前面板内容，使用最外层触发点重新定位；推荐各级采用一致尺寸。
  父层保留选择和滚动状态，返回时聚焦进入该子菜单的行。
- Esc 每次只返回一级；重复/释放被消费。外点、普通业务命令、owner 失效关闭整链。
- 返回按钮与进入子菜单的 action 在前端消费；普通命令才交业务。值交互不关闭菜单。
- 每次进入或返回都更新呈现 epoch，输入快照携带 epoch。旧父层截图即使对应同一
  菜单，也不能在返回后再次作为当前输入；PopupSession 内部 token 继续管理分支身份。
- 暂时隐藏的父层不参与绘制和命中，但其锚点/控件可用性仍参与整链校验；失效则关闭。
- 输入激活按原始事件的 seat 处理；不再从已有焦点列表猜测触发来源。

4c 检查点没有自动悬停展开、侧向级联面板、菜单搜索和桌面级全局快捷键，当时子菜单
不新增 Wayland surface。连接颈后由 4h/4k1 接入；跨 surface 背景效果属于 4k3。
raster 截图不能替代该验收。上述级联与自动展开功能当前仍未增加。

矩形/圆角几何统一规则参见 [轮廓契约](ROUNDED_REGION_CONTRACT.md)。4g 可在 Popup/Menu
显式声明静态局部路径，复用现有 token、外点关闭屏障和旧帧拒绝；配方与限制见
[通用轮廓契约](SURFACE_CONTOUR_CONTRACT.md)。未声明的菜单不改变形状；随锚点位置、
方向和最终尺寸生成连接颈在后续 4h/4k1 实现。不要用装饰形状替代一致的绘制、输入和效果轮廓。

## 4h：主题面板轮廓

本节为 4h 历史检查点，保留主题配方与同 surface 定位语义；native 最终配置下的
正文/连接颈和输入边界由 4k1/4k2 接续。

在 Popup/Menu 内声明：

```prism
Contour(recipe: "attachedPanel", radius: "@panel_radius",
        neckWidth: "@space_section", neckHeight: "@space_sm", fallback: "detached")
```

这是元数据，不是内容子节点。三项参数可使用有限数值或 Number theme token；配方与
静态 Contour 互斥。正文尺寸/padding 保持原语义；Below 的连接颈朝上、Above 朝下，
安全边距仍为 8，gap 增加有效 neckHeight，颈尖距锚点 8，不表示物理贴合。EdgePanel、
零颈部或安全范围不能对齐有效锚点时使用声明的独立面板回退，保留最终正文位置/间距。

圆角、肩部、颈尖和正文准备为同一闭合轮廓，Square 的 radius=0 全部使用直线。
外阴影不属于输入，连接颈内部属于当前 Popup scope，外点关闭沿用 owner/token 政策。
参数、缓存、旧快照及失败回滚的完整规则见
[通用轮廓契约的 4h](SURFACE_CONTOUR_CONTRACT.md#4h按最终定位准备面板轮廓)。
独立声音面板 fixture 位于 tests，运行 demo 和安装版本不因该测试而改变。

## 4i：跨 surface 的定位与生命周期基础

本节记录 4i 当时的实现边界：标准 `xdg_popup` 传输和 WM 接纳已经接入，Scene 的
Popup/Menu 当时仍在原 surface 呈现。后续 4j 拆分共享 GPU，4k1 导出 Scene 计划，
4k2 接入自动 Host 生命周期与提交输入。以下低层接口仍不作为第三方应用的新启动
入口，也不启动新的业务进程。

### 可信父对象与坐标

- 父对象是渲染所有者线程上的实际 `WaylandWindow`，子对象借用其 connection、
  compositor 和 xdg-shell。调用者不能用 app_id、pid、任意数字或另一连接的代理替换
  parent。父窗口保持唯一的 Wayland Pump；子层不创建独立 display、seat 或线程。
- `PopupPositionerRequest.anchor` 使用父 wl_surface 的逻辑坐标，desired 是整个
  子窗口 geometry 的尺寸。传输边界通过 `PreparePopupPositioner` 准备有类型整数。
  该纯函数不读取屏幕、主题、Scene 或 Wayland。
- 准备函数的 parent_geometry 必须来自实际的 xdg window geometry，是整数协议值。
  首版父窗口没有显式 geometry 或 subsurface，其值为已提交 buffer 的完整逻辑范围。
  configure 更新了待呈现尺寸但像素尚未提交时，不授权新 child；不能拿 ACK 的尺寸
  替代当前 window geometry。父 configure 的尺寸变化也关闭已有 children。
  anchor 先与此范围求交，再减 geometry 原点，向外 floor/ceil；子尺寸与 gap 向上取整。
  不能把 buffer 像素坐标、输出全局坐标或预期位置混入这个空间。
- 默认 Center/Below，支持 Start/End 和 Above。标准 positioner 请求翻转、平移和尺寸
  约束；它描述偏好，**不是最终位置**。WM 使用父 owner 所在输出的可用区域约束，
  先转成实际父 window geometry 的局部坐标再约束 positioner rules；不把输出全局
  矩形直接传给 wlroots，也不重复叠加 scene_xdg_surface 已应用的 Popup 位移。
  gap 是初始方向的 positioner offset；标准 flip 不保证翻转后有 4h 的对称间距，
  后续 Scene 适配必须依据最终 configure 决定正文和连接颈，不能把偏好当结果。
  此处可用区域是实际启用输出的完整逻辑矩形，不额外扣除 shell 保留边；若 owner
  中心未匹配输出，只能使用另一个实际启用输出。没有启用输出时关闭本次 Popup，
  不用调试用的默认 1280×720 冒充许可区域。输出 scale/transform/mode/enabled 变化
  也关闭现存浮层，使旧变换与输入失效。
- `xdg_popup.configure` 的坐标相对父 window geometry。只有后续
  `xdg_surface.configure` 才完成一次配置：ACK 后冻结其最终矩形与配置序号。客户端
  只能按该尺寸提交；后续 Scene 导出要在此时重新布局和准备 attachedPanel，不能沿用
  同窗口 PlacePopup 的预检结果作为跨 surface 最终轮廓。
- 每个轴的父尺寸和 desired 上限 8192，gap 为 0—256；拒绝非有限值、空交集、非法
  枚举、不能精确表示的 parent geometry 和整数端点溢出，不发送部分 positioner。

### 生命周期与 WM 政策

- 先创建子 wl_surface、xdg_surface 和 popup role，发送不带 buffer 的初次 commit。
  收到完整 configure/ACK 前禁止像素提交。首版不做隐式 grab 或 reposition，尤其不
  把普通按钮计数伪装成可信的 Wayland input serial。
- 父对象记录仍存活的子对象；父关闭时先关闭所有子对象，再释放连接与父代理。
  child 的 Close/析构可重复，parent 关闭期间不能通过回调重入创建新 child。
  `popup_done` 立即终止本层、撤销提交入口并报告关闭；不会关闭父应用。
  回调内请求父 Close 时先封闭 children，连接销毁延后到当前 dispatch 返回，禁止
  重入 Pump/Open。Popup configure 序号跨重开单调；已关闭代理的迟到事件不能复活。
  parent 必须存活到 Pump/回调返回；析构不通知业务所有者。Configure 保留最后一次
  响应用于诊断，是否可提交仍以 IsConfigured/Surface 为准。
- WM 按实际 xdg parent 链和同一个 wl_client 验证 owner。Popup 挂在可信父场景树，
  不进入 BSP 主窗口树，不消耗 launcher 注册，不创建独立窗口装饰。
- 主窗口 owner 失效、隐藏、工作区/布局位置尺寸变化或开始组几何过渡时，WM 从最深层关闭整链。
  本步骤选择关闭而非在旧坐标上继续显示；防止组动画 capture 与命中使用不同的变换。
  原有同窗口 Popup 的关闭、token、外点消费和 Esc 政策不因此改变。
- 父连接的 seat 输入不能把 child surface 的事件解释为 WindowId 1 正文事件。
  子 surface 的 Scene scope、可信 grab、外点消费与旧帧拒绝在后续 Host 快照适配中
  一起接入；当前低层 child 不能宣称已经具备应用控件交互。

WM 可以接纳原生客户端的 v3 reposition 并重新约束该层；已映射子层随父 scene node
移动。本步骤没有 reactive 子层的重新约束和级联布局策略，不把嵌套坐标正确当作完整
级联避让。Prism 的低层 transport 暂不暴露 reposition。WM 的同 client parent 验证
保证归属关系，不能代替 grab 输入 serial 验证；本步客户端不发送 grab。

WM 在 new_popup 通知栈中拒绝新对象时，先禁止 scene 输入再于 event-loop idle 发送
done/销毁；不立即释放仍被 wlroots 通知栈使用的 native object。隐藏但仍 mapped 的
owner 上创建新 Popup 是该关闭路径的独立验证门槛。

### GPU 与效果边界

4i 检查点时 `WaylandEglSurface` 独占 EGL 初始化/终止，一个 `ClientRenderOwner` 只有一套
WSI、提交基线和输入快照。子层不能简单再 Open 一套同 display 的 EGL owner：
child Close 会影响 parent。下一步骤应将 connection/context 生命周期和每 surface
的 WSI、buffer age、损伤、呈现基线拆开；复用一个 render worker 和资源所有者。
后续 4j 的实现边界以[多 surface GPU 契约](MULTI_SURFACE_GPU_CONTRACT.md)为准；
该资源拆分仍不等于 Scene 浮层已经导出；Host 导出必须使用 proxy 销毁前的 WSI 清理屏障。

本步骤不为 Popup 伪造 toplevel effect descriptor，也不承诺跨 surface 的磨砂/阴影。
测试中用于验证 map/unmap 的 SHM buffer 只存在 tests；不作为新的生产渲染后端。
同 surface 正文模糊仍按独立合成边界设计。

## 4k2：native 交接与输入边界（阶段记录）

本节记录 4k2；4k3 扩展见背景效果契约。4k2 自动 native 仅用于 scale=1、`requires_backdrop=false` 且能够准确导出局部布局的
面板；主题名称不是能力判断条件。请求和计划仍来自同一 Scene，应用继续声明
Popup/Menu、绑定和值动作，不直接创建原生子窗口。

child 首次真实 Pixels 成功后，Scene 按不可改造的准备记录及 Host identity 采用
child-local 输入，随后使 root Paint/输入版本失效并移除同一面板。root 和 child
提交独立，这一顺序保证先有 child 像素再撤 root，不保证跨 surface 原子呈现。
后续 State/checked None 只能在当前 target/layout 与真实 pixel 基线有效时更新
输入；adoption sequence 与 pixel ID 分开，子层反馈不推进 root UI 启动里程碑。

鼠标按实际 surface 分派；root 仍消费外点关闭，非 grab 菜单的 parent 键盘输入由
Host 转交当前已采用逻辑 scope。内部焦点转移不误关整个面板，真正失焦和设备移除
取消交互。菜单替换、返回、父配置/UI 改变和关闭撤销旧资格；旧输入、提交与关闭
不能控制新 epoch。root 输入提交不覆盖 child captures，Slider/Scroll 使用真实
配置下的局部几何；迟到提交不会倒退实时滚动语义。

各 target 有独立 callback、呈现反馈容量、buffer age、损伤与成功提交基线；共享
资源在同一 worker 串行管理。局部布局按 configure 与布局签名缓存，纯 Paint 和
滚动复用已配置几何，布局输入改变才重新 Measure/Place。详见
[计划契约](POPUP_SURFACE_PLAN_CONTRACT.md)和[生命周期规范](POPUP_TARGET_LIFECYCLE.md)。

毛玻璃仍在 root 呈现，4k3 才接 effect descriptor 与 parent 正文采样。构建、回归、
SDK 原生功能和部署结果以当次执行记录为准，历史 4i/4j/4k1 的通过记录不替代本阶段验证。
