# Demo 布局演进

## 当前：Concept B

用户已确认第二张效果稿，实施依据见 [Concept B](design/prism-concept-b/README.md)。
新增通用[视口条件](DSL_VIEWPORT_CONDITIONS.md)完成正常/紧凑布局；
Preferences 改为 Overview/Appearance/Monitoring，移除无数据的 Display 占位页。
Music 恢复有文字的导航、元数据与曲库；收藏过滤后的封面跟随实际曲目。

## 历史：v21 功能修复（视觉未获认可）

以下记录仅描述 v21，不能作为当前视觉设计范例。

# Demo 紧凑布局修订（2026-10-05）

> 状态：v21 功能/尺寸验证记录，视觉效果已被用户否定，不是已认可的设计基线。
> 尤其不应沿用拉伸的监控卡片和无文字的页面导航。后续以用户确认的效果稿及
> [视觉方案与密度验收](skills/prism-app-ui/references/visual-approval.md) 为准。

依据 prism-app-ui SKILL 修复 master 的 Bento demo 在 BSP 下的溢出。业务模块仍由统一
Host 托管；WM 不感知具体应用布局。此次不新增 viewport 断点或滚动协议。

## 尺度与页面

根使用 window 材质与 window_padding（当前 14），正文使用 card/control。
字号沿用标题18、正文14、辅助11；主要按钮32高，选中横线使用 indicator_height。

Preferences：32高标题、36高四页导航、8间距，剩余区域随窗口伸缩。

- Performance：CPU、RAM、GPU 三行，来自现有 LinuxMetrics；未知数据保留“—”。
- Appearance：四材质图标、明暗图标，选中状态仍由平台确认事件发布。
- Display：明确提示输出信息尚不可用，不展示伪造分辨率、驱动或扫描输出状态。
- System：监控开关与 0.5s/1s/2s 采样设置。采样操作从性能页移至此页。

Music：32高标题导航、主体 flex、44高播放区，间距6。

- 封面按可用区域 contain，曲名/歌手/时间与之并排；去掉重复黑胶装饰和无损音频宣传。
- 播放区保留循环、上一首、播放/暂停、下一首、收藏、静音，共212逻辑像素。
- 收藏与循环显示真实状态横线；音量是现有 demo 的静音/恢复状态，不代表真实音频输出。
- 曲库三行保留曲名与直接播放；为低矮窗口省去重复图标、歌手及每行时长。
  小窗口的曲库播放范围为32×26，是鼠标紧凑布局的明确例外，不声称适合触屏。
- 收藏为空显示提示。文件准备、错误和重试继续沿用原业务工作链。

当前不支持自动断点：统一单栏与切页优先保证244×420和482×204可操作。宽窗仍可扩大
内容区域；将来有通用尺寸适配契约后，可在充足空间恢复多列 Bento，不能写应用专属 WM 分支。

## 验证

`demo_layout_test` 用真实字体布局检查四材质×明暗×四尺寸下的四个设置页、播放区、
错误重试入口及曲库控件边界与中心命中。`demo_player_work_test` 覆盖收藏、循环和静音
状态变化；`client_app_templates_test` 检查真实模块与主题/DSL契约。
隔离 V3D 双窗/四窗截图保留在 dist/validation/demo-layout-fix-20261005。
测试及截图不进入生产包；此次不改变用户正在运行的远程桌面。
