# Surface 材料、透明与 GPU 背景效果

日期：2026-09-26。本轮实现规范，已部署 0.1.0-5；实际运行/截图与用户验收结果在 PI_DEB_DEPLOYMENT.md 和 VISUAL_TILING_REFINEMENT_PLAN.md 记录。普通生产应用只通过统一 host 使用 SDK，WM 不接收 DSL、DisplayList、业务动作或 Skia 对象。

## 1. 单一主题与职责

resources/themes/prism-glass.json 是颜色、尺寸、圆角、间距、模糊与阴影参数的唯一源。构建生成 prism/contracts/theme_tokens.hpp，WM 读取类型化常量，SDK 在 Schema 类型检查之前解析 `@token`。应用模板仅选择组件与绑定，不自行实现材质渲染。此版支持 Prism Glass；旧 WM Nordic/Minimal 的切换请求明确返回 unsupported。Settings 切换自身的配色，并未提供跨应用全局主题服务。

SDK 绘制客户端 tint、控件、图标、局部卡片阴影与内部边线。WM 绘制窗口轮廓外的聚焦边线、外阴影与跨 surface backdrop。SDK 根圆角裁剪保证本家应用像素/命中轮廓一致；外部传统客户端的内容仍维持其自身轮廓，不宣称通用第三方 surface 圆角遮罩已经实现。

## 2. 透明与输入

DisplayList 每帧清为透明。Color/PNG 输入为 straight alpha，CPU BGRA8888 与 GLES RGBA8888 目标为 premultiplied alpha；CPU Wayland 诊断使用 ARGB8888，不再与 GLES 透明契约分歧。客户端不声明整个 surface 不透明。圆角 clip 在 Skia 执行，不能仅绘制圆角背景而保留不透明清屏。

Scene 产生 SurfaceInputRegion；平台按逻辑像素生成 wl_region，随下一次 surface commit 生效。Topbar/Dock 的透明留白不吞鼠标事件。祖先 clip 限制子内容命中；圆角边缘不接受不可见的点击。Hover、Tab/Enter/Space 状态由通用 SDK 管理，不把测试逻辑或应用分支混入 WM。

## 3. prism-surface-effects-v1

协议源：protocols/prism-surface-effects-v1.xml。Manager capabilities 明确报告真实 GPU backdrop 能力；当前需要 wlroots GLES2 renderer 与成功创建的 shader。无扩展/能力为零时，客户端保留正常 alpha/tint，不伪造已启用毛玻璃。

每个 wl_surface 最多一个 effect 对象。clear/add_region 修改 pending 状态，仅随该 surface 下一次 commit 原子更新 current；destroy 清空 pending，surface 销毁立即终止其效果。Effect 对象与 surface 必须属于同一 Wayland client，不能描述其他客户端 surface。

每个 surface 最多八个圆角矩形区域，单位为 surface-local logical pixels，坐标范围 ±8192、宽高 0..8192（不含零）、圆角 0..256、blur 0..48。SDK 限制非法 DSL 和过量区域；WM 再做协议校验。区域数据只有 bounds/radius/blur，不包含控件或业务语义。

SDK 提交之前与 viewport/祖先 clip 求交，完全不可见的区域跳过；v1 只接受可准确表达的统一圆角矩形交集。非对称曲线交集或非法最终范围产生明确诊断，不发送近似掩码或非法 Wayland 请求。

## 4. 合成顺序与资源

1. 按真实场景顺序遍历目标 surface 以下的可见节点；到目标 view 时终止，排除该 surface 自己的全部效果节点。
2. 将低层纹理与几何 GPU 合成到带扩展采样边界的离屏 buffer。保留 source box、transform、opacity 与真实位置。
3. 在一半分辨率执行水平/垂直 Gaussian 采样，然后按圆角区域回写完整尺寸材质 buffer；外部绘制主题阴影和边线。
4. 效果节点位于对应 view 之前；其后由常规 wlr_scene 合成客户端 tint/前景以及更高层 surface。

结果是实时读取下方合成内容，不模糊客户端自己的画面、不复制一张预先处理的壁纸作为替代。Effects 保持 wlroots presentation/frame_done/output 提交流程。缓存键记录低层 buffer、surface commit seq、几何、状态和区域参数，低层内容改变时刷新；这属于 compositor 材料结果缓存，不引入延后的客户端节点增量布局或 DisplayList 分块缓存。

生产路径使用 wlroots allocator、GLES render pass 与采样纹理，模糊不做 CPU 像素读回。Capture 测试工具可以读回截图，但位于 tests 或临时工具目录，不安装到生产包。此处不宣称零拷贝已验收。

Buffer/texture 在提交之前保留；effect scene 节点先于 renderer/allocator 销毁。EGL 上下文进入失败会关闭能力，私有 GL pass 恢复程序/attribute/texture 与 EGL 状态。GPU 错误不保留误导的旧材质。正常 fullscreen 不绘制 WM 外阴影/边框/backdrop，Shell chrome 隐藏；客户端自身的样式仍由客户端定义。

## 5. 验证入口

- CTest：透明预乘、圆角 clip、矢量图标、图片 fit、局部阴影、布局与输入、模板/业务绑定、真实四窗口 BSP。
- tests/probes/backdrop_probe.py：独立 V3D 会话，用测试包替换 Desktop 为高频棋盘格与变化颜色；截图检查模糊衰减及实际低层更新传播。测试素材不进入发布包。
- 实际 Pi 截图、输入/工作区/分割、重启回收与输出提交另行验收。动画首版只保留静态 hover/focus 反馈，不启用缩放、弹簧或 Dock 放大。

## 6. 本轮 V3D 验证记录

2026-09-26，隔离的 headless 会话仍使用 Pi 的实际 V3D GPU。高频下层测试图的红通道跨度为 191，Topbar 玻璃区域为 4；下层红/蓝切换后，玻璃区域的红蓝均值差变化为 23.5。结果保存在 `dist/validation/visual-backdrop/backdrop-report.json`，证明真实下层内容被模糊且更新传播，不以半透明填色作为毛玻璃验收。

Raw GLES 采样必须显式设置 MIN/MAG 为 LINEAR、WRAP 为 CLAMP_TO_EDGE；allocator 导入的纹理只有 level 0，不能依赖默认 mipmap 过滤。场景中的 buffer 可能已经释放而缓存纹理仍有效，必须优先借用 scene/client 的缓存纹理；新导入纹理保持到 pass submit 完成。

`tests/probes/visual_session_probe.py` 已通过统一 launcher/host 创建真实业务应用，保存 1024×600 的双窗、三窗、四窗截图与对应树，真实提交尺寸与树一致。正常退出及 WM/launcher/Shell 崩溃后的进程回收检查通过。最终 CTest 26/26 通过；五个独立 V3D 会话共 40 次背景捕获通过。前期曾有间歇捕获超时未复现/未归因，probe 保留诊断并失败、不重试，不能称作已修复。

0.1.0-5 已安装到 Pi 的物理 DRM 输出，五应用均报告真实 V3D FirstPresented，WM GPU backdrop 开启，截图无圆角黑底/遮挡。用户确认外观及播放、配色切换、Dock 激活点击均正常；详情见 Pi 部署文档。节点增量布局、分块缓存、花哨动画与零拷贝专项仍不列入本轮完成范围。
