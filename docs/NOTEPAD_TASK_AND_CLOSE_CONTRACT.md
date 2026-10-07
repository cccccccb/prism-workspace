# Notepad 文件任务与异步关闭续接

日期：2026-10-07。界面系统5d，本轮源码契约；正式v26尚未包含5a—5d。
前置规范为[任务Provider与业务ABI](OWNER_TASK_PROVIDER_CONTRACT.md)、
[文件Provider](FILE_TASK_PROVIDER_CONTRACT.md)和[文本编辑](TEXT_EDITING.md)。
验证结果在执行计划与独立证据目录记录，源码支持不等于正式会话已部署或视觉已验收。

## 1. 职责与接入

Notepad不再维护路径输入页和未保存确认页。共享面板、目录查询、用户输入作用域和
实际输入快照由Host/SDK管理；业务模块负责文档、文本、保存版本和真正的磁盘读写。
没有第二个前端进程，也不向WM加入记事本逻辑。

| 阶段 | 负责人 | 完成意义 |
| --- | --- | --- |
| OpenFile / SaveFile请求 | 业务C ABI → Host | 返回关联ID，只表示请求已接受 |
| 目录与路径选择 | 共享文件面板和目录worker | Success交付路径与覆盖意图 |
| 读取 / 保存 | Notepad的Host work | 完成并核对结果后更新业务文档 |
| 未保存询问 | 共享Confirmation | Save / Discard为业务choice，系统提供Cancel |
| 窗口关闭 | typed Close ABI → SDK有序命令 | 业务允许后才接受平台关闭 |

能力使用前检查完整Host尾字段、函数指针和对应capability。能力不足或请求返回0时
保留文档并显示状态说明，不恢复内联选择器。文件任务要求可读的320×240逻辑视口；
更小BSP窗口仍可编辑、保存已有路径，但选择文件需要先扩大窗口。
Confirmation使用自身240×180门槛，不能以透明度或禁用正文冒充已建立局部作用域。

## 2. 文档与业务流程

继续保留八个稳定编辑器、每页两个标签和独立撤销历史。文件任务与关闭流程禁止
标签管理；普通保存期间可继续编辑提交文档，保存完成只标记提交时的文本快照。
较新的编辑继续保持Unsaved，不会因旧保存完成而关闭标签或窗口。

- **打开**：OpenFile成功后复制规范路径；路径已在其他标签中时激活已有标签。
  否则在空槽位异步读取，验证完成才安装新文档。读取失败不占用槽位或替换草稿。
- **保存**：已有路径直接提交业务work；新文档先请求SaveFile。写入失败保留草稿。
- **另存为**：总览和宽窗工具栏提供入口；共享面板处理位置、文件名和覆盖确认。
  成功选择仍需业务worker真正保存；不能将provider成功写成“Saved”。
- **关闭标签**：干净标签直接移除；脏标签请求Save/Discard确认。Cancel保留，Save
  失败保留，保存期间出现的新修改再次确认；最后一个标签关闭后创建空白文档。
- **关闭窗口**：一次请求顺序询问所有脏文档；全部允许才自动关闭，无需再次点关窗。
  Discard许可绑定当时的文本快照，全部完成前不移除标签。中途Cancel或失败保留
  所有内存草稿，包括之前批准Discard的文档；已经完成的磁盘保存不会回滚。

关闭时已有业务读写则DEFER，工作完成后继续；工作失败拒绝关闭。已有文件/确认
面板或标签关闭流程时拒绝新的窗口关闭，用户先完成或取消当前任务再关闭窗口。
重复平台关闭在DEFER期间不重复回调、不叠加面板。

任务关联ID、work ID、close ID分别核对，不能相互替代。完成处理先清理旧pending，
再请求下一任务；拒绝迟到、重复或格式不完整结果。借用路径和文本在回调内复制，
模块不保存Scene、Wayland、Skia对象或任务panel action。

## 3. 通用Close ABI

新增纯C契约`prism/contracts/app_close.h`，ABI版本仍为1，通过尾追加兼容。

| 接口 | 语义 |
| --- | --- |
| `PrismAppModuleV1::on_close_request(instance, request)` | 新typed回调，存在时优先于旧回调 |
| `PrismCloseRequestV1` | `struct_size`、非零Host-issued `request_id` |
| `PRISM_CLOSE_REJECT_V1` = 0 | 保留窗口，退休本次关闭请求 |
| `PRISM_CLOSE_ACCEPT_V1` = 1 | 接受关闭 |
| `PRISM_CLOSE_DEFER_V1` = 2 | 保留窗口，稍后完成同一请求 |
| `PrismHostApiV1::complete_close(context, id, decision)` | 只接受REJECT/ACCEPT，成功返回0 |

新回调中的2不是旧bool接口里的“非零接受”。旧`on_close_requested`保持零拒绝、
非零接受，缺少回调默认接受；完整字段才复制，不读取partial尾指针。
模块向旧Host提供旧回调时，脏文档会明确拒绝关闭并提示先保存。

`request_id`在一个ModuleSession内单调递增、不复用、不回绕，不是窗口权限或
TaskSession identity。模块实例仍由真实Host归属约束，不能跨实例完成关闭。
只在owner线程使用；create没有pending请求，不能完成关闭；销毁/StopWork撤销
所有pending和已stage的决策，之后Host API拒绝，无销毁后回调。

`complete_close`只接收匹配pending，接受一次后退休ID。未知、零、重复、迟到ID、
非法decision和错线程拒绝。返回码为Completed=0、Invalid=-1、Closed=-2、
WrongThread=-3；模块不得将提交接受误认为平台已关闭。

ModuleSession先建立pending再调用业务，因此回调可以同步提交完成，但仍应返回
DEFER；Host在回调栈结束后的Pump消费决策。若回调直接返回ACCEPT、REJECT或非法值，
该返回决定本次结果，退休同步stage的冲突完成；非法值按拒绝处理。
DEFER期间重复关闭不重入，完成拒绝后下一次关闭获得新ID。

SDK的`ClientApplication::AcceptClose()`只在UI owner有效期间使用：撤销owner任务和
输入，单次排队`AcceptCloseCommand`，由平台线程接收。再次调用不重复排队，失败
通过现有terminal链报告。业务不直接调用SDK；Host消费typed决策后调用它。
关闭已接受后，同一输入批次后续事件也不能继续触发业务编辑或动作。

### 最小具名回调示意

```c
static int32_t CloseRequest(void *context, const PrismCloseRequestV1 *request)
{
    struct App *app = context;
    if (!request || request->struct_size < sizeof(*request)) {
        return PRISM_CLOSE_REJECT_V1;
    }
    if (!app->dirty) {
        return PRISM_CLOSE_ACCEPT_V1;
    }
    if (!app->host || app->host->struct_size <
        offsetof(PrismHostApiV1, complete_close) + sizeof(app->host->complete_close) ||
        !app->host->complete_close) {
        return PRISM_CLOSE_REJECT_V1;
    }

    app->close_id = request->request_id;
    /* Request the typed unsaved task; unavailable/failed must finish REJECT. */
    BeginUnsavedTask(app);
    return PRISM_CLOSE_DEFER_V1;
}

static void FinishClose(struct App *app, uint32_t decision)
{
    const uint64_t id = app->close_id;
    app->close_id = 0;
    if (id) {
        app->host->complete_close(app->host->context, id, decision);
    }
}
```

这是生命周期片段，不是独立完整应用；请求与确认示例见前置provider规范，真实
流程可参考Notepad的`notepad_tasks.cpp`和`notepad_files.cpp`。

## 4. 文件工作边界

业务文件工作继续通过typed FileJob/FileResult的CBOR编解码。输入、结果和scratch
使用已有Host预算，正文最多48KiB且为有效UTF-8；不在owner线程调用文件系统。
Path不是FD、文件快照或稳定权限。

父目录先canonicalize并固定目录FD，最终文件用NOFOLLOW的openat/fstatat核对；
同目录临时文件使用独占创建、0600，新旧路径竞争与目录替换在工作边界检查。
stamp包括device/inode/size/mtime/ctime；读取期间既复查打开的FD也复查路径身份。
现存文件保存还拒绝硬链接，保留原权限；不保留ACL/xattr等扩展元数据。

已有文档保存必须匹配已加载stamp；即使另存回同一路径并批准覆盖也不能跳过检查。
另存到其他现存目标仅在`overwrite_approved`时允许：worker新鲜观察目标，再在提交前
复查完整stamp。此许可表达覆盖意图，不证明目标从provider确认至worker开始没有变化。
希望精确绑定更早文件版本的业务需设计独立句柄/版本契约，不能假称路径已冻结。

新目标使用linkat，目标在最后时刻出现也不能被覆盖。现存目标复查后renameat；
POSIX没有原子compare-and-replace，metadata检查与rename之间仍有窄竞争窗口。
固定父FD避免重定向到新目录，但不宣称跨进程互斥或文件系统事务。
取消在系统调用边界协作检查并清理临时文件；已经执行的提交不能回滚。目录fsync或
提交后核对失败明确说明文件可能已保存，内存仍为未确认状态，不自动关闭或重试覆盖。

## 5. 视觉、运动和验证

保持参考图03/04的owner工作上下文、可读标题、紧凑正文和固定操作区。工具栏及标签
采用现有主题、图标和必要文字；关闭询问保留Save/Discard/Cancel文字，不能纯图标化。
普通正文可见，局部输入由任务scope阻止；面板交互以真实输入采用的Ready为门槛。

当前复用`control.feedback`及既有业务活动提示，不实现图05透视翻转。反馈、独立主题
任务运动和reduced motion接入仍按后续计划，不用动画完成取代任务或关闭决策。

验证分别记录：业务纯测试、真实临时文件竞争测试、ABI/SDK关闭测试、主题布局与
隔离真实Wayland/V3D流程。测试和probe留在tests，不安装到Notepad包或deb。
自动验证不替代实机视觉认可，也不代表FPS、触屏或系统故障时的自动恢复保证。
