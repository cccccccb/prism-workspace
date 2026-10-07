# Popup 子层生命周期与已提交输入

日期：2026-10-06。界面系统 4k2 / 4k3，承接
[不可变计划与最终配置布局](POPUP_SURFACE_PLAN_CONTRACT.md)。应用仍通过原有 DSL、
主题和业务 module 构建一个 Scene；WM 不读取 DSL。当前视觉参考图 02，Preferences
已提供真实菜单入口。匹配的 `0.1.0-26` Host/WM 已部署到 Pi 远程会话，见
[最新部署记录](PI_REMOTE_DESKTOP.md)；最终视觉仍待用户确认。

## 1. 所有者与身份

UI 所有者捕获请求、准备最终配置下的局部计划、处理动作和属性动画。一个 render
worker 持有 root/child Wayland 代理、共享 EGL Context/Ganesh/图片及各自 WSI。
parent 唯一 Pump 和 seat 按真实 surface 路由事件，未知或退役 surface 不能进入 root。

原生 `WaylandPopupTarget` 是不复用的 surface lifetime ID 与 configure generation。
GPU target identity 另行验证 Context/WSI/storage。Host 的 `PopupSurfaceIdentity` 结合
worker、target/lifetime、configure 和独立 adoption sequence；UI load、Scene/popup
epoch、父 configure 由不可变 packet/request 校验。整数身份本身不是原生授权。

候选 adoption sequence 为零。实际成功 Pixels/State 或经过有效提交基线校验的 None
才产生新 adoption sequence。平台 pixel ID 表示最后真实像素提交，与 metadata adoption
序号分离，不能用更新 metadata 冒充新像素。子层提交/反馈不推进根 UI 的 Preview、
Master 或 OnUiSubmitted 里程碑。

## 2. 配置与首次交接

1. render owner 校验当前父配置、稳定逻辑 epoch 与 positioner 请求，建立同连接 child。
2. 原生 configure/ACK 通过有序反向事件送 UI；UI 核对当前 load、worker、epoch 和
   parent configure，再准备 owning plan 与 child 自己的 exact image-use 清单。
3. worker 校验源请求、native/GPU 身份、布局及资源，安装 window geometry/input mask，
   按 child permit 绘制和 Swap。失败保留所属窗口呈现，不上报成功。
4. 首次实际 Pixels 通过有序事件送 UI，Scene 验证 prepared provenance 与身份后采用
   局部输入，根像素失效并生成不含该面板的新列表、输入与 effect 区域。
5. 后续 State/None 仅在已有真实 Pixels 与当前 target/layout 匹配时更新输入基线。

每个时刻只有一个逻辑 Popup 输入 scope；root 外点仍负责关闭且消费首次按下。
root 与 xdg_popup 是独立提交，当前协议不提供跨 surface 原子呈现。交接顺序保证先有
成功 child 像素再撤 root；不能将同一 worker turn 或 Swap 成功称为共同已呈现。

## 3. 输入与焦点

公开局部描述保持 `scene=0`，root 输入 API 继续拒绝。Scene 的采用接口核对不可改造的
prepared provenance 后生成内部可信副本；Host 的输入事件必须匹配当前 UI、native
target、configure、adoption identity 和 descriptor。输入不临时改写根节点几何。

root 输入提交只更新 root captures，不能覆盖 child captures。Slider 和 Scroll 使用
已提交局部几何；Scroll 语义偏移可比已显示帧更新，迟到的成功提交不能将它倒退。
菜单替换/返回创建新逻辑 epoch，旧输入、关闭与提交不能控制替换后的面板。

鼠标与键盘按原生 surface 路由；非 grab 菜单的键盘仍可能在 parent，Host 在同 UI、
同 logical epoch 的已采用 scope 中处理这些按键。内部 root↔child 焦点转移和 native
scope 撤销标记 `internal_transfer`，不误作整个应用失焦。已采用 native scope 后，
root→child 与 child→root 两个方向均保留同 seat 的鼠标捕获，不能只保护 root 方向。
外部失焦/设备移除取消交互；
Esc、外点与旧释放消费保持现有语义。child 触控继续拒绝，等待后续专项设计。

## 4. 调度、损伤与资源

各 target 有独立 callback、呈现反馈容量、buffer age、损伤历史与成功提交基线。
共享 Scene 每次只保留一个聚合动画机会，两个 target 的 callback 完成后驱动 UI
取当前单调时间；render worker 不计算动画，不重放未获准的旧时间样本。root 的
候选 list 与成功基线相同不能证明 child 持有未来回调：child 可能只提交了 State/None。
因此 root prepare 和 child ready/advance 均按真实 callback 判断是否发下一次机会，
防止 metadata 最后消费后丢失唤醒。Root prepare 还遵守反馈容量；child 独立变化
不要求 root GPU Swap，也不能在 root callback 尚未完成时提前抢新许可。
同一个获准 packet 记录 root/child 各自是否完成；仅成功 Pixels/State/checked None
或已满足的同一提交基线才能消费。一个 target 尚在等 callback 时，另一个 target 的
metadata 成功不能释放整个许可；双方完成后再取下一份时间样本。UI 暂时撤销候选
只表示等待 replacement packet，不关闭已有 native surface 或丢弃其输入基线。

最终局部布局按 configure 与布局签名缓存。纯颜色、交互和滚动更新替换当前值、复用
已配置几何；文本、字体、尺寸、padding 或拓扑变化重新布局。统计
`GetPopupSurfacePreparationStats()` 分别记录准备、实际布局和复用，不混入根布局计数。

child 自己携带图片版本；注册/释放仍由唯一 worker 串行处理，图片不重复上传。UI
安装与候选失效是顺序屏障；资源撤销阻止旧 child 候选/已提交包重放。关闭顺序为
child wrapper/WSI → shared Ganesh → root WSI → Context → 原生代理/连接；根关闭
hook 仅释放 GPU，不递归调用 Wayland Close/Pump。取消通知保留旧 scope 的 owning 值。

## 5. 能力回退与重试

4k2 最初仅支持无 backdrop；4k3 在 scale=1、可准备局部布局的面板接入 native
背景效果。普通 backdrop、v3 popup backdrop 与所需 contour 必须分别声明；无能力、
编辑器及不支持的祖先裁剪/变换仍在 root 呈现。首次清洁正文屏障与采样规范见
[背景效果契约](POPUP_BACKDROP_CONTRACT.md)。能力不足撤采用并恢复根列表。

Requested/native 准备失败撤采用、恢复 root；CompositorDismissed 匹配当前 epoch 时
关闭逻辑 Popup。UI 安装/父变化/菜单替换撤旧 lifetime。失败 intent 按 logical epoch、
positioner/父配置及主题代次抑制重复创建；普通 paint/动画更新不产生重试循环。
新的 epoch、定位或主题代次可重试，不能让迟到 Requested 关闭新面板。

## 6. 验证范围

纯 Scene 回归验证 provenance、根退场/恢复、输入版本、真实局部控件/Scroll 几何、
旧提交、焦点与布局缓存；协议 fixture 验证 child 节流/反馈、metadata、失效身份、
seat 路由、设备移除和关闭重入。独立 SDK V3D probe 从真实业务回调验证鼠标动作、
metadata、root/child 动画、能力回退与 UI 替换。测试/probe 不安装进生产包。
4k3 的父正文/下层毛玻璃采样已完成自动 GPU 验证，`0.1.0-26` 已安装到正式远程
会话，实际菜单与交互检查见 [最新部署记录](PI_REMOTE_DESKTOP.md)。触控与帧率测量
仍待专项；自动验证和部署截图不代替用户对最终视觉的确认。
