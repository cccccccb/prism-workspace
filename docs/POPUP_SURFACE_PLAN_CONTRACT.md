# Popup surface 计划与最终配置布局

日期：2026-10-06。界面系统 4k1 建立不可变计划，当前实现已接入 4k3，承接
[多 surface GPU 所有权](MULTI_SURFACE_GPU_CONTRACT.md)。视觉仍参考
[图 02](design/interface-system/02-popup.png) 的紧凑面板、标题与读数、分区和轻量层次。

## 1. 分步接入与当前边界

4k 分为三个连续检查点：

1. **4k1**：Scene 产出可信锚点请求，按最终 configure 准备不可变子层绘制/几何计划；
   Host 的 FramePacket 携带请求。平台区分含正文/功能连接颈的 window geometry 与含阴影的
   buffer。独立真实 Wayland/GPU probe 消费该计划，验证坐标、布局及关闭顺序。
2. **4k2**：RenderOwner 消费请求并创建/撤销 child；建立 UI 安装、配置应答、每 target
   提交/损伤基线和资源屏障。输入携带原生 surface 归属、UI/Scene/popup lifetime 与
   实际已提交快照；每 child 独立 callback/呈现反馈。全部就绪后才自动切生产路径。
3. **4k3**：接 Popup 通用 effect descriptor 与 parent 正文采样边界，核对背景效果和
   配方，再做生产部署与视觉验收。复杂侧向级联和触控另行设计。

当前的 **4k3 自动 native 路径**处理 scale=1、能够准确准备局部布局的 Popup/Menu。
需要背景采样时，compositor 必须分别声明普通 backdrop 和协议 v3 独立的
`popup_backdrop_capabilities`；轮廓效果同时要求 contour capability。Host 使用同一
应用 Scene、业务模块和 render worker 创建 child，不根据主题名称猜测能力。
首次实际 child Pixels 暂不启用背景效果；成功提交并完成 Scene adoption 后，才撤下
root 的同一面板。后续 State 或经过已提交基线核对的 None 可以更新局部输入基线，
不冒充新的像素提交。完整所有权、关闭和调度规则见
[子层生命周期与已提交输入](POPUP_TARGET_LIFECYCLE.md)。

背景效果必须再经过 clean-parent 屏障：root 成功提交移除 fallback 的内容，或以现存
真实 pixel 基线核对相同内容的 State/checked None 后，才通过 child State 开启采样。
这避免把旧面板采入自身背景；两个 surface 的 Swap 不代表跨目标原子呈现。
effect descriptor、父正文采样与首次交接的完整规则见
[原生背景效果契约](POPUP_BACKDROP_CONTRACT.md)。

能力不足、编辑器或不支持的祖先裁剪/变换继续在 root 呈现。4k1 的历史交付仅发布
请求与独立 probe 计划，当时未切换生产输入路径。当前实现没有增加应用 DSL 属性、
业务进程、第二个 Scene 或渲染线程；匹配的 `0.1.0-26` Host/WM 已部署到 Pi 远程
会话，最终视觉仍待用户确认，见 [最新部署记录](PI_REMOTE_DESKTOP.md)。

4k2 建立、4k3 沿用的生产交接遵循：

- child 首次成功像素提交与输入 adoption 就绪后，才撤下 root 的同一面板；切换期间
  只允许一个有效输入 scope。失败/关闭时按原因恢复回退或关闭逻辑 Popup。
- parent 唯一 seat 的鼠标/键盘焦点路由；关闭、设备移除时取消交互，Esc/外点消费及
  陈旧已提交快照拒绝。触控继续另行设计。
- child 自身图片版本清单及资源释放屏障；root 移除面板后，不能继续借 root 的
  image-use 清单证明 child 资源仍有效。

## 2. 权限与数据所有权

逻辑 Popup 是现有 Scene 的一个子树。其 NodeId、绑定、资源和状态来自同一应用 UI。
生产 Host 在 UI 所有者线程捕获请求；4k2 将原生 configure 应答通过有序事件交回
该线程，核对源请求后准备局部计划。render worker 消费值数据，不能读取 Scene、调用文本
shaping、布局或业务动作。共享准备池也不拥有 Wayland 代理。

请求包含 Scene 身份、popup token/活动节点/原始触发节点、父 configure 代次和准备时
的 Scene 状态。token 用于一致性；原生 connection/parent 权限仍由 Host 与平台验证，
不能把可构造的整数当作系统授权。菜单子页沿用当前替换面板模型，原生 parent 始终为
root；不把已隐藏的上一级逻辑菜单作为新的 xdg_popup 父窗口。

请求及计划所含快照、绘制列表、输入描述使用 `shared_ptr<const ...>` 共享。计划本身
为 owning 值，4k2 经不可变 `PopupFramePacket` 和既有双向有序队列发布。计划保存
opaque prepared provenance；公开字段和输入指针必须与该准备记录一致，不能自行
改造一份局部快照后要求 Scene 采用。
旧计划可以被读完，但不授予向已关闭或已替换 target 提交的权利。实际提交还必须匹配
UI load、worker、原生 popup lifetime、configure generation 与 GPU target identity。

## 3. 坐标空间与正文尺寸

必须明确区分：

| 空间 | 用途 |
| --- | --- |
| parent surface 逻辑空间 | 可见锚点、原有 Scene 内容与当前 root 输入 |
| parent xdg window geometry 空间 | positioner 与 compositor 最终 popup configure |
| child 正文空间 | 子树布局、内容 padding、连接颈配方依据 |
| child surface/buffer 空间 | EGL 尺寸、绘制、输入区域以及正文在 buffer 内的偏移 |

`Configure.bounds` 表示包含正文与功能连接颈的 window geometry 在 parent window
geometry 中的位置。Positioner 的 desired geometry 包含颈高；请求 gap 为 8，实际
间距以最终 configure 为准，标准 flip/slide 调整不保证两侧保持对称间距。
最终配置决定整个功能区域的宽高：Below 的正文位于颈下，Above 的正文位于颈上，
两者都从配置高度中扣除实际颈高；无法连接则 detached 正文使用该配置。不能继续
使用 DSL authored size 或先前同窗口回退尺寸。正文局部重新布局，再生成裁剪、轮廓、
文字和控件 Visual。

连接颈、边框和外阴影可以伸出正文。计划给出含这些绘制范围的整数 bufferSize、
包含正文与功能颈的 windowGeometry，以及实际正文在 buffer 内的 bodyGeometry。
它们不是另一份窗口布局；不会重复扩大正文 padding。外阴影不扩大输入。原生
adapter 设置 windowGeometry，保持 compositor 的功能区域约束；功能连接颈不能
被排除在原生 window geometry 外，避免其他 compositor 的裁剪或命中与画面不一致。

当前平台为 scale=1；坐标及 buffer 轴上限遵循现有 4096 限制。未来分数缩放必须另行
定义逻辑到物理映射，不能把本轮整数协议规则写成通用 HiDPI 能力。

## 4. 最终配置准备与失效

请求必须来自已解析布局和当前可见锚点。最终配置准备在 UI 线程完成；准备不修改
已提交 root 的 bounds、输入快照、滚动位置或 PopupSession。它只对拥有的快照值做
局部布局，不复制 Scene，不重解析 DSL，不改变业务绑定或偷偷关闭逻辑面板。

关闭/替换、锚点或内容状态变化、主题变化、父 configure 变化后，旧请求必须重新捕获。
最终配置来源不能是应用自行指定的输出区域。连接颈方向及连接中心取实际配置与锚点；
若最终几何不能满足连接条件，按已有 detached 配方回退，不能绘制悬空的假连接颈。

局部 InputSnapshot 是子树描述，保留 NodeId/token，排除 owner 正文与其他菜单；
其 `scene=0`，真实 Scene 身份保留在 plan.request 中。这使 root 输入入口明确
拒绝局部坐标描述，不能误把它当成已提交输入许可。4k2 在 `AdoptPopupSurface` 中核对
prepared provenance、当前 Scene/popup/parent configure 和 Host 提供的 target 身份，
生成不公开的可信副本；`HandlePopupSurfaceInput` 再匹配 identity 与公开 descriptor
指针才进入同一 Scene 的控制逻辑。root `ApplyInputSnapshot` 不能直接采用
child-local 快照。root 与 child 各有自己的 adoption 基线，不能用全 Scene 单一
版本比较替代该协议。当前局部 Input.version 沿用 root 样本版本；不同 configure
可以产生不同局部几何，版本相同不代表几何相同。4k2 联合 worker、target、lifetime、
configure 与独立 adoption sequence 校验；该序号与最后真实 pixel ID 分离，State
更新也能产生新 adoption。首次 adoption 必须已有真实 child Pixels。

采用后，Scene 内部保留完整输入源供子树导出，对 root 发布剔除该子树的快照。
root 与恢复后的输入版本严格单调；root 提交不覆盖 child 的指针/Slider captures。
菜单替换、返回和关闭撤销旧 scope。可接受同 epoch 稍旧的成功提交，但 live action、
enabled、节点代次和滚动状态继续否决陈旧激活；迟到提交不能倒退当前滚动语义。

### 内部 API 与线程顺序

| 接口/数据 | 所有者与调用时机 |
| --- | --- |
| `Scene::CapturePopupSurfaceRequest(parent_generation)` | UI 所有者，Build/输入几何解析后；默认 parent geometry 为根 surface 全尺寸 |
| Capture 的显式 parent geometry 重载 | Host 提供实际已提交的 parent window geometry，不能以应用自报输出区域替代 |
| `FramePacket::popup_surface_request` | 与 root 绘制/输入样本一起发布；相邻帧可替换，UI 安装及资源命令仍为顺序屏障 |
| `PopupSurfaceConfigure` | render owner 将原生 configure 转为值；包含父代次、child 代次及功能区域 `window_bounds` |
| `Scene::PreparePopupSurface(request, configure, diagnostic)` | UI 所有者核对仍有效的请求，再产生不可变计划；拒绝时保留 root 回退 |
| `PopupFramePacket` | Host 发布局部计划、UI/worker/native 身份和 child 自己的图片版本清单 |
| `Scene::AdoptPopupSurface(plan, identity)` | UI 消费成功 child 提交后调用；返回 accepted，首次采用使 root Paint/输入版本失效 |
| `Scene::HandlePopupSurfaceInput(event, identity, snapshot)` | UI 验证当前 native 提交归属后，用实际 child-local 坐标处理同一 Scene |
| `Scene::RevokePopupSurface(identity)` | 使用最近成功采用的完整身份撤销 scope，恢复 root 回退并重新布局 |
| `WaylandPopup::SetBufferLayout` | Wayland 所有者使用 plan.buffer_size/window_geometry/configure_generation 设置待提交几何 |

示例为内部值协议；不是应用创建原生窗口的入口：

```cpp
runtime::PopupSurfaceConfigure final_configure{
    request.parent_configure_generation,
    native_configure.generation,
    native_configure.bounds // window geometry，包含功能连接颈
};

// 在 UI 所有者线程准备，返回值经既有队列交给 Wayland 所有者。
std::string diagnostic;
auto plan = scene.PreparePopupSurface(request, final_configure, &diagnostic);
```

普通 Capture 共享缓存，不逐帧复制完整树；有活动面板的 Build 保留当次准备快照。
仅主题代次等状态变化、像素未变化时，Capture 更新已解析的快照值及输入 metadata，
不调用 Build、布局或绘制。该路径也适用于 Host 不执行 Build 的 metadata-only packet。
最终配置准备复制快照值。准备结果和旧快照可独立读完，存活共享指针不绕过
lifetime/configure 校验。

### 当前局部准备能力

普通容器、文字、图标及已解析控件随最终正文尺寸局部布局。Slider 使用当前呈现值
重建 track/fill/thumb；ScrollView 使用局部可滚动范围钳制偏移，不修改 root 的偏移。
TextField/TextArea 的编辑状态与局部输入映射尚未接入；此类内容返回 diagnostic，
保留现有窗口内呈现。不能精确保持祖先裁剪或输入变换语义时也明确拒绝。

原生 window geometry 为整数；正文尺寸可以是分数。连接颈高度遵循 Contour 的
1/256 量化，避免功能轮廓超出整数配置。外阴影按各轴 `ceil(3 * sigma) + 4` 加
offset 保守留白，另留抗锯齿范围；输入区域只覆盖功能轮廓，不覆盖阴影。

4k1 每次 Prepare 都执行局部 Measure/Place。4k2 按 popup epoch、最终 configure、
正文几何和布局签名缓存已解析布局；纯颜色、交互和滚动内容更新替换当前值并复用
几何，滚动只移动局部内容，文字、字体、尺寸、padding 或拓扑变化才重新布局。
`GetPopupSurfacePreparationStats()` 分别统计成功准备中的 plans、实际布局与复用，
不混入根 `GetRenderStats()`；这不是通用节点增量布局或 DisplayList 分块缓存。
当前计划的 DisplayList generation 标记 configure 代次，不能
代替每 target 内容提交代次、损伤基线或实际已提交输入样本。

## 5. 平台 buffer layout

WaylandPopup 的 buffer layout 绑定当前已 ACK 的 configure generation。window geometry
原点非负，宽高精确匹配 configure，完整位于 buffer 中；所有值必须为有效整数。
不合法或旧代次的 layout 不发送协议请求，也不覆盖当前有效 layout。

新 configure 撤销旧 layout，恢复该代次默认 window geometry 原点 0、buffer 等于配置尺寸。
显式 Set/Reset 只设置 window geometry，不 commit。旧 AttachBuffer 调用仍支持
默认布局；设置过扩展布局后，buffer 宽高必须匹配该布局。正文 bodyGeometry
只用于布局/坐标映射，不代替原生 windowGeometry。

直接 EGL 调用者必须先安装匹配当前 configure 的 layout，再用计划 bufferSize
创建/resize WSI，最后绘制并 Swap。平台无法拦截任意原生 EGL 调用，这个 layout
接口本身不足以提供完整 production submit gate。4k2 的所有者状态机验证 native/GPU 身份、已配置
布局、提交许可及资源版本，封闭 Host 的调用路径。root/child 各有 callback、反馈
容量、buffer-age 损伤历史和成功提交基线；稳定 child 不强制 root GPU Swap。

## 6. 验证范围与历史交付

纯 Scene 用例覆盖最终尺寸和方向、窄面板、连接颈/阴影留白、控件布局、局部输入、
请求失效、主题/菜单替换，以及不污染 root 几何/状态。平台协议用例检查非零正文
geometry、错误/旧代次拒绝、新 configure 撤销与原有 AttachBuffer 时序。

4k1 独立 native probe 用真实同连接 xdg_popup、共享 Context/Ganesh 和 Scene/DSL 导出
计划渲染。记录实际 GPU、CPU/GPU 像素比较、buffer/body 配置与资源关闭。测试内容、
SHM bootstrap 和读回留在 tests，不安装。功能验证不代表输入、毛玻璃、帧率或 VNC
视觉已验收；当前安装桌面版本不随本步自动变化。

4k2 新门槛还覆盖实际 adoption/root 退场与恢复、State/None 输入更新、局部控件和
Scroll 几何、旧提交拒绝、键盘内部焦点转移、独立 child callback/反馈/损伤以及
资源和关闭屏障。结果由[执行文档](INTERFACE_SYSTEM_PLAN.md)的当次记录给出；本节
下面的数量属于 4k1 历史验证，不用作 4k2 已通过或已部署的证明。

**4k1 历史验证（2026-10-06）**已通过：WM/Host 构建、相关 CTest 最终 42/42、隔离原生
GPU 检查 8/8。Scene 导出检查实际使用 `V3D 4.2.14.0`，覆盖 24 个材质/配色/定位
组合、72 次提交和 240 次像素对照，验证更新后的控件像素及 24 次 child 关闭释放。
初始构建依赖遗漏与测试夹具 action 遗漏均已修正，失败日志保留。详细范围与证据索引
见 [执行文档第 27 节](INTERFACE_SYSTEM_PLAN.md#27-第四阶段-4k1不可变-popup-surface-计划2026-10-06)。
