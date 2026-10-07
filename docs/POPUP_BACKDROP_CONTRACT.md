# 原生 Popup 背景效果与父正文采样

阶段：4k3。接续 [Popup target 生命周期](POPUP_TARGET_LIFECYCLE.md)。
参考图 02 的轻盈 Popup / 可组合 Menu；本阶段增加背景效果，不改变应用布局与动效风格。

## 1. 边界与能力

- Scene 从最终 native configure 的同一份布局导出 DisplayList、InputSnapshot、input_regions
  与 effect_regions。DSL 不选择采样源，不操作 Wayland / EGL / wlroots。
- Host 单一 render owner 使用现有 display、EGL context、Ganesh 与图片注册表管理子目标。
  effects 是 target 的元数据，不增加 Scene、业务实例、worker 或独立图片缓存。
- WM 接收 typed region / Contour，按真实 scene 顺序解析下层内容。WM 不解释 DSL，
  不区分 Music、Preferences、Notes 等具体应用。
- `prism_surface_effect_manager_v1` **版本 3** 独立声明 `popup_backdrop_capabilities`。
  普通 backdrop / contour 能力不隐含 popup 父正文采样。旧版本 / 不支持的 renderer
  继续走根表面 fallback；需要 Contour 时也必须有 contour 能力，不能退化为矩形。

## 2. 几何与裁剪

`PopupSurfacePlan.effect_regions` 为 child **wl_surface-local 逻辑坐标**，含 shadow padding
造成的 surface 原点偏移；并非 parent、window_geometry 或屏幕坐标。它与 input/list 在最终
Measure / Place / Contour / TranslateSubtree 后导出。blur 改变属于 Composite 元数据，
不要求重新布局、文字 shaping 或提交 child pixels。

使用 root 与 child 共用的严格交集规则。不能用单一 rounded rectangle / Contour 表达的
裁剪、超过 8 个 region、非法 / 超预算 Contour，拒绝整份 native plan，保留 root fallback。
预算按最终 child configure 可见区域重新核对；原 root clip 可能遮住部分 region，
不能用 root 的数量代表最终子层。阴影不扩张 effect mask 或 input shape；功能连接颈
与正文共用规范化 Contour。

WM 用真实根 surface 对应的 scene surface leaf 得到 global surface 原点；window geometry
的偏移由 wlroots xdg adapter 已处理，不能再次叠加。paint 作为 popup tree 的同级前驱，
其位置为 global paint origin 减去 parent tree global origin。嵌套 popup 遵守同一转换。
输出 scale / transform 由最终 scene 输出处理，capture 保持 global logical 坐标。

## 3. 采样顺序与缓存

capture 从 scene 根按真实 lower-to-upper 顺序遍历，在目标 popup tree 前停止：

1. 包含后方桌面、下层窗口 / 材质以及父窗口已提交正文。
2. 排除整个当前 popup tree、该目标自身的 effect paints 和全部上层节点。
3. 下层 popup 可以作为后开的 popup 背景；不会重新采到当前目标形成递归。
4. 客户端 Skia 绘制 tint / alpha、边框与内外阴影；WM 只提供规定 mask 内的背景模糊，
   不再给 popup 叠加顶层窗口装饰。

复用 SurfaceContent identity、mapping epoch 与 DamageHistory；每次 surface commit 的真实
buffer damage 在 scene listener 更新前记录。父正文损伤命中含 blur kernel padding 的
采样 footprint 时重算；外部 damage / callback / input-only commit 可复用缓存。不能
用 buffer 地址、输入事件、frame callback 或 UI paint revision 代替内容版本。

paint tree 监听原生 tree 销毁。父 tree 先消失时清空 scene 指针，避免 role 与 wl_surface
寿命不同导致悬空；隐藏 / 关闭目标在下一次 effects 解析前退役其 paint。

## 4. 首次交接屏障

独立 wl_surface 没有跨目标的原子 swap。本协议明确使用以下顺序：

1. 先提交 child 真正的 Pixels，effect 列表暂为空；此时 root 仍保留旧 fallback。
2. UI 验证实际成功提交后采用 child input，生成移除 fallback 的 root 不可变 packet，
   携带被移除的 exact native identity。没有真实 child Pixels 不得采用。
3. render owner 等该 root packet 成功提交；若是 checked-identical None / State，必须有
   现存实际 root pixel baseline 且列表及资源与它相同，才可确认清洁正文。
4. 再向同一 child lifetime / configure stage effects，通过 State commit 开启背景采样。
   首次等待不能用来跳过平台能力验证或容忍旧 target。

excluded 的 adoption sequence 必须非零且不晚于当前成功 child adoption，native
worker / lifetime / configure 精确匹配。子层 metadata 可能先于 root 提交前进，不能
要求两个 adoption sequence 永远相等。same-size parent configure 可保留旧实际像素，
但成功 root metadata 的 parent configure 必须为当前值。ready 首次转真时发出一次队列
唤醒，使 checked None 后也能推进 child State，不依赖鼠标事件或固定轮询。

该屏障防止 child 模糊采到尚未撤出的 root fallback。configure / 关闭 / UI 更换撤销
屏障；主题或 blur-only 更新只更新当前 target descriptor。元数据 State / None 不增加
pixel callback、presentation 计数或 Preview / Master 根里程碑。

## 5. 验证范围

测试保持在 tests / probes，包中不安装。验证 Scene final geometry、provenance / 预算、
协议版本与能力矩阵、Pixels / State / None、旧 configure / lifetime、真实 GLES readback
的父正文与顺序、damage 命中 / 不命中、Contour mask 与父 tree 先销毁。SDK 原生门槛
验证 Glass 与 Square 都能保持 child、交接后 metadata 不强制像素以及独立动画节拍。

构建 / 验证结果在 INTERFACE_SYSTEM_PLAN 与 dist/validation 的当轮记录中填写；源代码
完成不等于已替换用户 VNC 会话。触控、输出缩放 native SDK 接管、跨目标原子呈现及
native popup 的独立移动动画不在本阶段增加。
