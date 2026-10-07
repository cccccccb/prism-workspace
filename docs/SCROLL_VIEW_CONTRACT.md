# ScrollView：纵向内容视口

本契约对应界面系统第三阶段 3c。ScrollView 属于客户端 Scene；WM、业务模块与
Skia 后端不需要识别应用的滚动内容。当前能力需使用本轮源码构建的 Host，既有 v22
运行包不能据此视为支持。

## 结构与布局

ScrollView 恰好包含一个非 Visual 内容节点，通常为 VStack；内容按自然高度布局。
视口使用显式 height 或父级剩余空间 flex。必须裁剪绘制、命中和 surface effect 区域，
不能通过 clip/overflow 关闭。padding、spacing 放入内容容器；不要在视口上声明 action。

```prism
ScrollView(flex: 1, scrollSpeed: 3) {
    VStack(padding: 12, spacing: 8) {
        Text("General", height: 24, font: "@font_body", foreground: "@mutedText")
        Button("Appearance", action: "appearance", height: 40)
        Button("Notifications", action: "notifications", height: 40)
    }
    Visual(scrollPart: "thumb", width: 4, height: 24, inset: 3,
           cornerRadius: 2, background: "@controlOutline")
}
```

完整验证页面见 `tests/fixtures/interface-system/scroll.prism`。固定标题、分隔线、底部说明
应放在 ScrollView 外。沿用设计稿的层次与紧凑行距，内容不足时不要人为拉大每行间隙。
为右侧指示条保留空间，不能让文字覆盖指示条。

## 指示条与主题

可选直接子 Visual 使用固定 scrollPart: "track" 或 "thumb"；各角色最多一个，必须
为叶节点，width > 0。thumb 的 height 是最小长度，实际长度按可见比例计算；inset 是
轨道两端和右侧内缩。无溢出时自动隐藏。角色身份不能绑定或由主题改变。

指示条只是呈现，不接收拖动。颜色、宽度、圆角等使用普通 DSL/主题能力；几何由
Scene 在绘制快照上投影，不进入 Skia 特殊分支，也不占内容布局空间。

## 输入与生命周期

- 鼠标纵向滚量乘 scrollSpeed（默认 3，合法范围 0.1–32）。不另启 timer。
- 嵌套容器先消耗内层可用距离，将剩余原始滚量传给外层，各层使用自己的速度。
- TextField/TextArea 保留原有编辑器滚动处理；本轮不提供编辑器边界向外层传递。
- Tab/Shift+Tab 使目标自动滚入可见区；嵌套时逐层显露内部视口。
- 滚动使内容几何变化时取消内容内的按压/捕获与 Slider 流，防止松开误激活。
- 尚未提交新输入快照时，旧内容位置不能激活；连续滚轮仍可沿显示快照找到容器。
- 水平滚动、触控、惯性、拖动指示条、虚拟化不在当前范围。

## 状态与性能边界

滚动位置是 owner 线程的前端状态，不是业务值；不发送 ControlValue Preview/Commit。
`Scene::ScrollInfo(id)` 返回 offset、maximum、viewport_height、content_height；
`Scene::ScrollTo(id, offset)` 夹紧有限数值，布局尚脏或没有实际位移时返回 false。
这些 API 与其他 Scene 修改一样只能在 owner 线程调用。

纯位移平移 retained 子树 bounds，并更新输入快照和 Paint/Composite；不重新测量布局。
当前仍遍历内容子树，不宣称虚拟化或分块缓存。内容变化、尺寸变化重新布局并夹紧 offset；
主题、Preflight、保留节点的区域事务继承 offset，新节点从 0 开始。像素和输入几何仍
通过既有不可变快照交给渲染线程。
