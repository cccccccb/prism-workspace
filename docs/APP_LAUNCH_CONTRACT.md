# Prism 应用包、模块 ABI 与启动协议 v1

日期：2026-09-26。阶段：统一运行时恢复计划第一步。代码位于 `prism/contracts` 和独立 `prism/launch`，不依赖 WM、DSL parser、Skia 或 Wayland。目录包校验、C ABI 加载、消息编解码、里程碑状态机和 Shell 凭证消费检查已经实现；host、常驻 socket 服务、worker 池和真实 WM 授权接入尚未实现。

## 1. 应用包

第一版使用目录包。launcher 通过注册的 app_id 选择包根目录，客户端启动请求不能传入任意命令、业务模块路径或 Shell 角色。

```json
{
  "format_version": 1,
  "runtime_abi": 1,
  "app_id": "org.prism.music",
  "name": "Prism Music",
  "version": "1.0.0",
  "ui": "ui/master.prism",
  "preview": "ui/preview.prism",
  "module": "backend.so",
  "assets": "assets",
  "window": { "width": 960, "height": 640 }
}
```

- `format_version`、`runtime_abi` 必须是整数 1；字符串、浮点和不同版本拒绝。
- app_id 最长 128 字节，首字符为 ASCII 字母，后续仅允许字母/数字/`_-.`；它是平台注册标识，不是 shell 命令。
- 除 `preview`、`window` 外字段均必填。window 未给出时为 640×400；给出时 width/height 均必须为 1..4096 整数。
- 字符串必须非空、无 NUL，JSON 使用 UTF-8；manifest 最长 64 KiB、嵌套深度不超过 16。
- 未知字段、重复键、错类型或语法错误拒绝；`exec`、`shell_role` 不属于新版包格式。使用真实 JSON parser，不复用旧包工具的字符串字段扫描。
- 资源路径必须相对包根目录，拒绝绝对路径、`..` 和反斜杠；现存文件解析后的真实路径也必须在根目录内，阻止符号链接越界。UI/module/Preview 为普通文件，assets 为目录。
- `LoadPackage` 校验路径和资源类型，不宣称完成签名验证、沙箱或业务模块初始化。UI 解析在 host 的客户端运行时进行；module ABI 由模块加载器继续检查。
- 包由平台安装/注册后保持只读且不在实例运行中修改。当前路径校验不是针对并发替换文件的密封文件句柄方案；后续服务派发若允许动态包更新，需先固定资源句柄/内容版本。
- 旧 demo manifest 和 `.prismpkg/.prismb` 不自动升级为新版包；五应用在第四步统一切换。旧字段不会被忽略或偷偷回退。

构建依赖增加 `nlohmann-json3-dev`（本机 3.11.3）；依赖为 header-only，契约头本身不包含 JSON 库。解析 API 见 [nlohmann JSON 文档](https://json.nlohmann.me/api/basic_json/parse/)。

## 2. 业务模块 C ABI

公共头：`prism/contracts/app_module.h`，可由 C 与 C++ 编译。模块导出固定入口 `prism_app_module_v1()`，返回 `PrismAppModuleV1`。

- 表首部为固定宽度 `struct_size` / `abi_version`，ABI 1 必须包含当前完整前缀。当前版本可接受更大的结构，但不解读未知尾部。
- `create` / `destroy` 必填，create 返回 NULL 表示失败；动作、tick 与平台启动事件回调可选。
- Host API 提供类型化 binding、BackendReady、异步启动请求和一次性 tick 调度。模块不获得 Scene、Skia、EGL 或 Wayland 对象。
- 字符串为 UTF-8 指针+字节长度，不能假定 NUL 终止；ABI 内指针、size_t 与结构体仅用于同进程，不作为 wire format。
- Host API 表在实例销毁完成前有效；回调参数借用至本次调用结束，模块需要长期持有的字符串自行复制。Host 复制 set_binding 的字符串值。
- 回调与 Host API 调用发生在 host 事件线程，必须及时返回；模块后台工作结果在事件线程转交，不能直接调用现有非线程安全 Scene 接口。具体调度由第二步 host 实现。
- launch_app 返回非零请求 ID 表示已接受排队，不表示首帧已呈现；后续通过 on_launch_event 接收实例进展。
- tick 使用单调时钟纳秒；不允许业务模块自行驱动 SDK Pump 主循环。
- C++ 模块不得跨 ABI 边界抛异常。模块实例必须先 destroy，最后卸载模块。首版不在应用仍运行或业务线程未停止时 dlclose。
- `AppModule` 用 RTLD_NOW/RTLD_LOCAL 加载并验证符号、版本、结构大小和必要回调；加载与 ABI 错误抛出带 LaunchError 的 LaunchFailure 交给 host 转换为启动失败，绝不报告 RuntimeReady；缺文件/入口为 ModuleLoadFailed，ABI 不兼容为 UnsupportedAbi。包错误同样使用类型化异常，避免 host 从日志文字猜测错误。

这是一份可加载的原生业务 ABI，不是任意代码的安全沙箱。实际模块生命周期、绑定队列与 GPU 初始化在下一步实现。

## 3. 启动消息编码

公共请求只有 app_id 与模式：ActivateOrCreate=0 或 NewInstance=1。request_id 非零，作用域为调用连接；launcher 需按连接/请求 ID 关联、去重。instance_id 由服务端分配，在同一个会话中唯一，不由启动请求者指定。服务重启使此前实例/凭证失效。

每帧采用明确的大端字节序，禁止 memcpy 原生对象：

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `PRL1`（50 52 4c 31） |
| 4 | 2 | 协议版本 1 |
| 6 | 2 | type：1 LaunchRequest，2 LaunchEvent |
| 8 | 4 | payload 字节数，最大 65536 |
| 12 | 8 | request_id |
| 20 | 8 | instance_id；请求必须为 0 |

Request payload：mode:u8 → app_id 长度:u16 → UTF-8 字节。

Event payload：pid:u32 → milestone:u8 → error:u16 → exit_code:i32 → detail 长度:u16 → UTF-8 字节。detail 最大 2048 字节、无 NUL；字符串必须为有效 UTF-8。exit_code 仅 Exited 可非零：0..255 为正常退出码，-1..-64 为结束进程的信号号。

milestone 数值：Accepted=0、WorkerAssigned=1、RuntimeReady=2、SurfaceConfigured=3、FirstPresented=4、BackendReady=5、Failed=6、Exited=7。

error 数值：None=0、InvalidRequest=1、UnknownApplication=2、InvalidPackage=3、UnsupportedAbi=4、NoWorker=5、ModuleLoadFailed=6、RuntimeFailed=7、PresentationFailed=8、Cancelled=9、Timeout=10、SessionEnded=11。

Failed 必须携带非零 error，其他里程碑 error 必须为 None。包校验拒绝等未分配实例的失败可使用 instance=0、pid=0；其他事件有真实实例 ID。Accepted 的 pid=0；WorkerAssigned 之后 PID 非零且保持一致。

`FrameSize` 收到不足 28 字节的头时返回 0；完整头即校验版本、类型、长度和 ID，再返回需要的帧总长度。`DecodeMessage` 只接收恰好一帧，拒绝截断/多余字节。未来 socket 接收器据此积累部分读取并逐帧解析；当前未实现 socket 收发、超时、连接授权或事件广播，不声称已替换旧 launcher IPC。

## 4. 实例里程碑

`InstanceState` 关联请求、实例与分配的 PID；错误、重复、乱序或不同身份事件不得修改既有状态。

```text
Accepted → WorkerAssigned → RuntimeReady → SurfaceConfigured → FirstPresented
                                   └────→ BackendReady（独立里程碑）
任意活动阶段 → Failed；已分配进程 → Exited
```

- BackendReady 可在首次 configure/呈现之前或之后发生，但必须已有 RuntimeReady。
- FirstPresented 只能由实际平台呈现确认驱动；当前已有 SDK PresentedCount 是 swap 提交计数，不能直接冒充该事件。第二步需定义并接入实际呈现反馈。
- Accepted 不表示进程已派生，WorkerAssigned 不表示 UI 已显示，RuntimeReady 不表示业务完成。
- Failed 后禁止继续推进成功里程碑，但仍允许后续真实 Exited 完成进程回收记录；保留失败原因。
- Exited 为终态，后续同实例事件拒绝；新启动使用新实例 ID。
- 本状态机描述首次加载里程碑；后续 resize/configure 不重复发首次 SurfaceConfigured 事件。

## 5. Shell 凭证和会话生命周期

ShellPermit 是私有 launcher↔WM 数据：会话标识、请求/实例 ID、实际 worker PID、Desktop/TopBar/Dock 角色、32 字节随机 token 和单调时钟失效时间。不能出现在普通 LaunchRequest/manifest 中。

已实现 `ShellPermitGuard` 检查完整关联、过期、一次性消费与撤销，且 guard 不可复制。消费失败不损坏正确请求的凭证；角色映射前消费，worker/session 结束时撤销。

真正的随机 token 生成、可信 bootstrap 通道、连接凭据获取、唯一凭证登记及 WM 首次映射前绑定在第三/四步实现。调用者传入的 PID/session 不能当作身份事实；本轮单独的 guard 不是完整角色授权系统。

会话管理器将 WM 与 launcher 纳入同一会话生命周期：WM 就绪后发起三个 Shell 包启动，launcher 管理实际 worker 与退出；WM 只登记已授权角色。会话停止清理实例与待命池；服务重启的池/实例/凭证状态不得沿用，失败返回 SessionEnded。具体服务启动/重启策略随真实 host/launcher 一次性切换，不仅重启旧 Zygote 二进制。

## 6. 阶段验收

测试仅位于 `tests/`，C 动态模块 fixture 不安装、不进入应用源码目录。覆盖：

- C/C++ ABI 头独立编译，契约独立配置不依赖渲染器或 JSON。
- 包字段/类型/重复键/版本错误、资源缺失、符号链接越界与普通文件类型要求。
- 固定 wire 字节样例、分段头、截断、尾随字节、错误版本/模式/ID、UTF-8 与长度边界。
- BackendReady 的两种顺序、PID/实例错配、失败后的退出与终态拒绝。
- C 模块真正 dlopen、create、动作→binding/Ready→destroy，以及错误 ABI/必要回调缺失。
- Shell 凭证身份错配、token 错误、失效、撤销与重复消费。

完成本步骤后进入统一 host：实际载入新版包和一个 demo 业务模块，接管运行时事件循环，验证 Preview/业务 Ready/首帧反馈，再实现预热池。当前生产服务及已安装 deb 仍是上一个迁移检查点，启动优化没有在本轮完成。
