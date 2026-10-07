# Prism Notepad

基于 Interface v2 + 标准 `prism-app-host` 的独立记事本包。业务模块不依赖 Wayland、
EGL、Skia、Qt 或 Scene；文件工作通过 Host work 线程执行。

## 操作

- 工具栏提供新建、打开、保存、文档总览，图标点击区为32×32。宽至少420逻辑单位
  时额外显示带文字的“Save as”；窄窗仍可从文档总览右上角进入另存为。
- 最多保留八份文档。标签显示文件名、未保存圆点、当前标签横线和独立关闭按钮。
  每页两个标签；超过两份时显示翻页箭头，总览直接选择文档。翻页不改变正在编辑的
  文档，选择文档会定位相应标签页；再次打开同一路径直接激活已有文档。
- 打开、首次保存及另存为使用Host共享文件面板，目录枚举与选择校验不阻塞编辑线程。
  不保留旧内联路径页。provider不可用或准备失败时保留文档并提供状态提示。
- 另存为选择现存目标时，由provider在同一个任务内确认覆盖。业务收到规范路径与
  覆盖意图后才提交真正读写；意图不是冻结版本，worker再次检查目标及提交前变化。
  已加载文档普通保存使用完整device/inode/size/mtime/ctime stamp，外部变化不会被覆盖。
- 标签×使用共享Unsaved changes确认，业务提供Save/Discard，系统提供Cancel。
  取消、文件选择失败、写入失败保留草稿。保存期间继续编辑仅将提交快照标记为已保存；
  newer edits再次确认，不能自动丢弃。最后一份标签关闭后创建空白文档。
- 关闭窗口使用typed异步关闭续接，一次关闭请求顺序处理所有未保存文档，完成后自动
  退出，无需再次关闭。整个流程确认完成前不删除任何草稿；中途Cancel保留全部标签
  和先前选择Discard的内容。正在进行文件工作时等待完成后续接；工作失败则拒绝退出。
  已有文件选择或标签确认任务时拒绝新增窗口关闭请求，先完成/取消当前任务再关闭。
- 文件任务、工作或关闭流程期间暂停标签管理；保存worker运行期间仍允许当前正文编辑。
  不支持异步关闭的旧Host只接受干净、无工作状态退出，未保存文档保持打开。
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

需要匹配5d源码的Host/SDK；正式v26尚不提供共享文件任务和异步关闭续接。
不在旧版本中复制另一套本地选择器或确认页。

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
| 工具栏 | normal、available | new/open-panel/save/save-as/documents | 32 高、18px 标题、18px 图标 / critical |
| 标签栏 | tab_0..7、label_0..7、selected_0..7、pages | select:0..7、close:0..7、page-previous/page-next | 36 高、11px 文件名、24×32 关闭范围、4px 选中条 / critical |
| 正文 | text_0..7、active_0..7、editable | edit:0..7 | flex、14px 正文、12px 内边距 / critical |
| 文档总览 | documents、used_0..7、label_0..7 | select:0..7、save-as、cancel | 两列四行，适应低矮 BSP 窗口 / critical |
| 共享文件/确认任务 | Host保留绑定，不进入应用Interface | typed request_task / on_task_completed | 共享DSL、owner局部模态 / Host |
| 状态 | summary、status、activity_opacity | 无 | 18 高、11px 辅助信息 / critical |

根使用 window 材质和 `@window_padding`，区域间距 8；正文使用 card 和 surfaceRaised。
操作与选中条沿用 text/accent/hover/accentSoft，不在应用内硬编码明暗色、窗口玻璃或阴影。
文档总览在正文区域显示；共享文件和确认任务保留背后的工作上下文，由Host掌握输入作用域。
available/ editable使用enabled控制真实输入，visible只折叠总览，不以透明度冒充禁用。
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

独立布局测试覆盖244×420、320×480、482×204、900×640及四种材质×明暗；检查工具栏、
标签、总览和disabled状态真实命中，光栅图片用于字号和留白检查，不代表真实壁纸的透明合成。
业务测试使用typed fake Host深复制请求，再执行真实临时文件读写，覆盖共享任务取消、
陈旧/损坏结果、保存快照/newer edits、另存覆盖、重复打开、八文档上限及顺序异步关闭。
共享面板尺寸及原生GPU验证由框架独立测试负责；本源码改动不自动替换正式VNC。
