# Prism Notepad

基于 Interface v2 + 标准 `prism-app-host` 的独立记事本包。业务模块不依赖 Wayland、
EGL、Skia、Qt 或 Scene；文件工作通过 Host work 线程执行。

## 操作

- 顶部四个图标依次为新建、打开、保存、文档总览；主要操作保持 32×32 点击区。
- 最多同时保留八份文档。标签显示文件名、未保存圆点、当前标签横线和独立关闭按钮。
  每页两个标签；超过两份时显示翻页箭头，总览可直接选择任意文档。
  翻页只浏览标签，不改变正在编辑的文档；选中文档会定位它所在的标签页。
- 打开图标进入路径页，填写**绝对路径**后确认。已有文件点击保存即可；新文档首次
  保存才请求路径。文档总览右上角保存图标进入另存为，避免常驻路径栏占据正文空间。
  路径输入支持水平查看完整名称；当前没有系统文件选择器。
- 另存路径已存在时拒绝覆盖；已打开文件保存前校验磁盘身份/大小/修改时间。
- 每个标签的 × 关闭对应文档；关闭后台的干净标签不切换当前文档。未保存文档显示
  Keep（取消）、Discard（放弃）和 Save（保存并关闭）。取消保留所有内容；保存失败
  不关闭文档；保存期间若出现更新的内容，重新确认，不丢弃新修改。
- 最后一份文档关闭后创建空白文档。关闭整个窗口遇到未保存文档会保留窗口并定位它；
  处理后可再次关闭窗口。文件工作期间暂不接受标签管理或退出，正文普通保存时仍可编辑。
- 点击正文编辑；方向键、Home/End、Ctrl+Home/End 移动光标，Shift 扩选，鼠标拖动选择。
  Ctrl+A 全选，Ctrl+Z 撤销，Ctrl+Shift+Z / Ctrl+Y 重做；每份文档最多 64 次历史。
- Ctrl+C/X/V 使用本窗口内部剪贴板。Tab / Shift+Tab 在控件间切换。
- 滚轮纵向滚动，水平滚轮横向滚动；键盘移动光标时自动滚入可见区。

## 首版边界

支持有效 UTF-8 的常规纯文本，包括 txt、Markdown、JSON、配置和代码文件的纯文本编辑，
不按扩展名限制。每份文件最多 48 KiB，符合现有 Host 的 64 KiB binding/work 输入边界。
不提供语法高亮、自动换行、搜索替换、大文件编辑、自动恢复或文件关联。

保留原文件字节（包括 BOM 和换行）；在含 CRLF 的文件中 Enter 插入 CRLF。
不进行旧编码猜测/转码。拒绝 NUL/二进制控制字符、无效 UTF-8、符号链接、设备、目录；
硬链接文件只能另存为新路径。保存使用同目录临时文件、fsync、原子替换，保留已有
文件权限；新文件使用 0600。不保留 ACL/xattr 等扩展元数据。外部进程与最终 rename
之间的极小竞争窗口尚不具有跨进程互斥保障。

编辑位置按 Unicode scalar 边界计算；尚无完整 grapheme、双向文本、跨字符 shaping
与字体 fallback。CJK 显示取决于 Host 字体覆盖。键盘字符来自 xkb 布局；尚未接入
Wayland text-input 输入法、系统剪贴板、按键长按重复。不要将本版本当作完整 Unicode
专业编辑器；这些是真实应用验证明确暴露的下一步通用平台能力。

## 开发运行

需要本次源码构建的 Host，旧安装版本不认识 TextField/TextArea 和新增 ABI 尾字段。

```sh
cmake --build build-gles --target prism_notepad_module prism-app-host notepad_test
ctest --test-dir build-gles -R '^notepad_test$' --output-on-failure
build-gles/bin/prism-app-host --package "$PWD/build-gles/share/prism/apps/prism_notepad" --wayland wayland-prism-0
```

无需安装或切换桌面会话。应用在正常安装规则中发布至 `share/prism/apps/prism_notepad`。
测试及诊断不进入应用包。

## 页面契约与视觉尺度

| 区域 | binding | action | 尺度 / 加载 |
| --- | --- | --- | --- |
| 工具栏 | normal | new/open-panel/save/documents | 32 高、18px 标题、18px 图标 / critical |
| 标签栏 | tab_0..7、label_0..7、selected_0..7、pages | select:0..7、close:0..7、page-previous/page-next | 36 高、11px 文件名、24×32 关闭范围、4px 选中条 / critical |
| 正文 | text_0..7、active_0..7 | edit:0..7 | flex、14px 正文、12px 内边距 / critical |
| 文档总览 | documents、used_0..7、label_0..7 | select:0..7、save-as、cancel | 两列四行，适应低矮 BSP 窗口 / critical |
| 路径页 | path_panel、path_title、path | path、apply-path、cancel | 18px 标题、32 高输入与操作 / critical |
| 未保存确认 | confirm、confirm_title | cancel/discard/save-close | 18px 标题、14px 提示、32 高操作 / critical |
| 状态 | summary、status、activity_opacity | 无 | 18 高、11px 辅助信息 / critical |

根使用 window 材质和 `@window_padding`，区域间距 8；正文使用 card 和 surfaceRaised。
操作与选中条沿用 text/accent/hover/accentSoft，不在应用内硬编码明暗色、窗口玻璃或阴影。
路径、总览和确认占据主体页并隐藏工具栏及正文输入；使用 visible 折叠，不以透明度冒充禁用。
零高度在当前布局中意味着自动布局，不能用于折叠 Slot。

首个空文档即可 ready。文件工作保留提交时的文本快照，只有该快照标记为已保存。
八个 TextArea 保留稳定节点和独立撤销历史；关闭时清除对应历史，避免槽位复用泄漏旧内容。
`title/details/busy` 等业务状态仍保留；原 previous/next、open、close 动作为模块兼容入口。

当前 DSL 没有通用横向滚动标签控件或 viewport 回调，故宽窄窗口均采用两标签分页。
没有标签拖拽排序、批量关闭或会话恢复。本轮重点是直接切换、逐标签关闭与安全保存。

## 动效与验证

悬停背景、按下轻缩放和键盘焦点描边使用 `control.feedback`，工作指示使用
`panel.visibility`。动画只改变 Visual；命中区域保持稳定。instant motion 立即完成，
无常驻动画循环，也不伪造布局动画。

独立测试覆盖 244×420、320×480、482×204、900×640，四种材质 × 明暗；检查工具栏、
标签、总览、路径和确认操作的真实命中，保存快照/新编辑、失败保留、后台关闭与八文档上限。
光栅图片用于检查字号和留白，不代表透明材质在真实壁纸上的最终合成效果。
