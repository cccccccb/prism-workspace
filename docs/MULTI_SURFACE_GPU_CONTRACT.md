# 多 surface GPU 所有权与提交契约

日期：2026-10-06。界面系统第四阶段 4j，承接
[跨 surface 传输基础](POPUP_MENU_CONTRACT.md#4i跨-surface-的定位与生命周期基础)。

## 1. 本步边界

一个应用仍只有一个 Wayland connection、一个 Pump 和一个 render worker。
`WaylandEglContext` 持有 EGLDisplay、Config 和唯一 GLES context；Ganesh、字体与
上传图片由同一个渲染所有者维护。`WaylandEglSurface` 只持有该 context 下的
wl_egl_window/EGLSurface，以及本 surface 的 buffer age 和损伤声明状态。

生产根窗口迁移到上述显式所有权。此步提供多个 GPU target 的底层能力；不创建
第二个 ClientApplication，不增加前端进程或线程，也不自动导出 Scene Popup/Menu。
Host surface 计划、子层输入快照/呈现反馈和 Popup effect descriptor 在后续接入。

## 2. 真实 connection 与生命周期

- Context 在 render worker 上 Open，借用仍存活的 wl_display。Surface 的 Open
  显式接收 Context；通过 wl_proxy_get_display 校验真实 wl_surface 属于同一连接。
  app_id、pid、整数身份和代理地址都不能代替这一校验。GLES 配置检查该客户端 API
  是否存在；不支持的系统在配置时给出明确错误。
- 一个 owner 只 Open 一份 Context。Context 记录附着的 WSI，Close 先关闭这些 WSI，
  再销毁 GLES context/终止 EGL。WL 连接及父子代理最后关闭；底层借用对象不能先释放。
- 关闭非 current child 不改变 parent 的 current 绑定。关闭 current child 尝试恢复
  同 owner 的另一存活 target；没有存活 target 或恢复失败时不能声称仍可绘制。
  单个 Surface Close 不销毁共享 context，也不调用 eglTerminate。
- Context 与 Surface 均不可复制/移动；Close 可重复。Context 关闭会使外部仍存活的
  Surface 失效；外部对象稍后析构不能再次访问旧 EGLDisplay 或旧连接。
- 清理顺序是：停止该 target 提交，解除其 Skia wrapper；整个 owner 退出时先释放
  Ganesh/图片，再关闭 WSI，再关闭 Context，再关闭 Wayland。原创建 context 不能
  current 时，Ganesh 必须 Abandon，不在另一个 context 删除同名 GL 资源。

Window/Popup 的 SetBeforeCloseHandler 是后端资源的单次撤销屏障，在 native proxy
与 connection 销毁前执行并解除自身绑定。回调必须 noexcept，不能销毁/重开其所有者
或递归 Pump。它只清理资源，不替代业务事件；Closed 通知仍发生在 native proxy
销毁之后，不能在该通知里才关闭借用 surface 的 EGL WSI。

Window 在 dispatch 中请求关闭时，封闭重入和新提交，先运行资源屏障、关闭 children，
connection 销毁仍等 dispatch 返回。ClientRenderOwner 用具名 CloseGpu 注册屏障，
因此 Pump 的协议/连接失败也先清理根 WSI，而不是断开后才清理。下步 Host 自动
导出必须给各 child 注册资源屏障，覆盖 popup_done、parent Close、configure 关闭
与协议失败；显式调用者也可按 ReleaseTarget/WSI Close/Popup Close 顺序先行清理。

## 3. GPU target 身份

`GpuTargetIdentity` 是不依赖 EGL/Wayland/Skia 的有类型值：

```cpp
struct GpuTargetIdentity {
    std::uint64_t context_lifetime_id;
    std::uint64_t surface_lifetime_id;
    std::uint64_t resize_generation;
};
```

三项非零。Context 与 Surface 每次成功创建获得进程内不复用的 lifetime ID；实际
尺寸变化增加 resize generation，即使后来回到原尺寸也不同。计数溢出拒绝操作。
它用于资源与提交一致性，不是输入权限 token，也不是任意应用可伪造的系统授权。

Renderer 显式接收由平台所有者提供的身份，仍校验真实 current EGL context 和原生
draw/read surface。缓存不能只按 width/height/FBO 查找：两个 EGL surface 的默认
FBO 都可以是 0。每个存活 target 的 Skia wrapper 独立；切换 target 重置 Ganesh
对 GL 状态的假设。图片纹理继续在共享 context 中复用，不因切换重新上传。
WSI MakeCurrent 明确绑定默认 framebuffer 0；Renderer 每次绘制撤销缓存的 render
target 绑定假设，以应对同 target 的原生重绑。外部非默认 FBO 属于直接 backend 调用者，
不能借用 WSI 的默认 target 身份继续绘制到另一个 FBO。

同 surface 新 generation 替换旧 wrapper，旧 generation 拒绝；同 generation 的
原生目标或 framebuffer 描述变化也拒绝。缓存至多 64 个存活 target，达到限制失败，
不做隐式创建新 renderer。ReleaseTarget 在本 context current 时执行，并在对应
WSI Close 前完成；释放后的旧身份由调用方生命周期屏障禁止再次提交，不维护无界墓碑。
现有无身份的 Render 重载只支持其创建时的原生 target，不授予多 target 能力。
旧重载也必须在该原生 target 生命周期内使用，销毁 native target 前关闭其 renderer；
原生 handle 地址被回收不能证明仍是同一个目标。

## 4. 每 surface 的提交状态

`ClientRenderSurfaceState` 把 WSI、候选/准备/最后成功像素帧、已提交输入快照、图片
损伤比较 epoch、BufferDamageHistory 和准备中的损伤计划放在同一 target 中。
Context、Ganesh 和版本化图片注册保留在 ClientRenderOwner。生产目前只有 root_target；
新增子 target 时不能复用 root 的 history 或提交/输入基线。

- QueryBufferAge、SetDamage 和 Swap 的状态属于目标自身。首次创建和 resize 需要
  full repair；关闭或失败一个 target 不清除另一个 target 的 age/partial-update 状态。
- repair 相对当前 back buffer，content damage 相对该 target 上次成功 pixel 提交。
  State/None 不推进 history；只有该 target 成功 Swap 才 Commit。失败 posting 不重试。
- Prepare 保存当时的 GpuTargetIdentity，CommitPixels 必须再次匹配当前 WSI 身份。
  旧尺寸、旧生命周期或关闭后的准备计划不能用于另一个 target。
- UI 安装/关闭清理该 target 的提交与输入基线；实际 GPU wrapper/WSI 生命周期独立。
  统计保持当前公开字段与语义，不能把提交计数当 GPU 完成或物理呈现帧率。

动画仍由 UI 所有者按当前时间采样，render worker 只消费不可变结果。此步保持原有
FrameOpportunity、事件确认与 pixel/state 门控；后续多 surface 调度需要显式 target
身份和各自的 callback/feedback 许可，不能让 parent callback 充当 child 呈现证明。

## 5. 验证门槛

测试与 probe 全部位于 tests，不进入生产包。必须覆盖：

1. 同 context 的同尺寸默认 FBO=0 目标轮换，独立颜色/裁剪与 partial repair。
2. 共用图片只上传一次，target 切换不重新创建 Ganesh 或重复上传。
3. resize/recreate 的身份失效、旧 generation 拒绝，以及外来 context 拒绝。
4. 真实 Wayland WSI 的 parent/child 关闭、恢复、同连接校验和每 surface age 状态。
5. 原有 SDK 启停、提交、图片和动画门控回归；格式、goto 和 800 行限制。

Surfaceless/Pbuffer 和真实 Wayland WSI 分开记录。Pi 的硬件结论必须实际报告 V3D；
软件 EGL 通过不写成硬件验证。headless GPU 功能验证不等价于 VNC/物理输出的视觉、
帧时、带宽或功耗验收。本步不替换正在运行的桌面。

## 4k3：背景效果元数据

Popup target 在现有共享 GPU owner 下 transport 最终局部 effect_regions；不会复制
Context、renderer 或 image registry。首次 child pixels 的 effects 暂为空，确认移除
root fallback 的成功 pixel baseline 后再 State 开启。背景捕获由 WM 负责，客户端
保留 tint 与内容；每 target 的像素、callback / feedback、输入基线保持独立。
详见 [原生 Popup 背景效果与采样](POPUP_BACKDROP_CONTRACT.md)。
