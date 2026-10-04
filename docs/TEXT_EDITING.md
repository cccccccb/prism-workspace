# 通用文本编辑与记事本应用（2026-10-04）

本次为真实文本应用补齐了正式 runtime DSL 的 TextField / TextArea。旧 scene/AOT 的
TextInputNode 不是本次实现路径；WM 没有应用名或编辑器专用分支。

```prism
TextField($path, action: "path", height: 36, padding: 8,
          material: "control", font: "@font_body", foreground: "@text")
TextArea($body, action: "body", flex: 1, padding: 12,
         font: "@font_body", foreground: "@text", clip: true)
```

在 Interface v2 中声明 string binding。action 是必需的非空编辑标识；不是将文本
拼接进 action。输入经 Scene 生成拥有值的 TextEdit，再经 ClientApplication、Host、
ModuleSession，交给可选 C ABI 尾字段 `on_text_edit(instance, action, text)`。参数为
调用期间借用的 UTF-8 切片，模块需复制并发布对应 binding。相同值回送不重置光标、
选区及历史；不同的业务值替换正文并清空本控件历史。控件本地编辑有界为 48 KiB，
文档不通过 JSON 文本拼接或寻找字段来更新。

编辑器复用提交后 InputSnapshot 的命中和焦点规则。隐藏/失能/未提交节点不接收输入。
编辑状态不进入 WM，也不进入渲染线程：UI owner 生成 glyph、选区和光标图元，既有
DisplayList 通道负责裁剪/提交。无光标常驻 timer 或固定刷新循环，闲置时不提交。

`on_close_requested(instance)` 是另一可选 ABI 尾字段，零表示保留窗口，非零接受。
统一 SDK 的 Wayland 关闭请求先送 owner，再用有序 AcceptCloseCommand 回到平台线程。
没有回调的应用默认接受关闭，直接使用 WaylandWindow 的旧原生客户端保留原行为。
它不拦截强制终止、会话退出或系统故障，不等价于自动恢复。

完整的界面契约、操作和当前 Unicode/输入法/剪贴板边界见
[记事本说明](../prism-notepad/README.md)。应用模块仅链接纯契约、纯文本值逻辑及
nlohmann JSON 的 typed CBOR 编解码，Host 管理文件 work 的取消和 join。
