# 文件任务、异步目录模型与共享面板

日期：2026-10-07。界面系统5c，源码及自动验证完成；承接
[Owner任务](OWNER_TASK_AND_MODAL_SCOPE.md)和[业务ABI](OWNER_TASK_PROVIDER_CONTRACT.md)。
本规范定义本地文件系统provider，源码能力、自动验证和正式部署分别记录。
正式v26不支持本阶段的文件任务。5d源码将Notepad接入真实业务，见
[文件业务与异步关闭](NOTEPAD_TASK_AND_CLOSE_CONTRACT.md)。

## 1. 三种请求与职责

| 请求 | 成功结果 | 业务责任 |
| --- | --- | --- |
| OpenFile | 已验证存在的普通文件绝对路径 | 打开、读取、解码及处理读取失败 |
| SaveFile | 已验证的目标路径及覆盖确认意图 | 写入、原子替换和处理实际写入失败 |
| SelectDirectory | 已验证存在的目录绝对路径 | 后续目录操作 |

provider只读取目录和metadata，不创建目录、文件，不写入、不截断、不自动保存。
成功表示已接受用户选择，不是业务操作完成；路径不是打开的FD、稳定内容快照或权限凭证。
业务操作时仍需核对对象及错误，不能用`overwrite_approved`绕过实际写入检查。

业务只提交typed请求。ModuleSession复制借用输入、发放关联ID；Host绑定实际前端
owner。目录模型只处理拥有的数据与文件系统，SDK负责共享DSL和输入快照，WM保持
通用窗口职责。没有额外前端进程、业务绘制分支或第二套任务生命周期。

## 2. ABI尾追加与校验

`PrismTaskKindV1`增加OpenFile=2、SaveFile=3、SelectDirectory=4，能力位分别为
`1u << 1/2/3`。Confirmation=1及原能力位保持。

Request原前缀完整保留，尾追加`const PrismFileTaskOptionsV1 *file`。旧Confirmation
的最低可读长度仍是原`choices_size`字段末端；只有完整的新尾指针存在时才读取。
部分广告的尾字段按未提供处理，文件类请求缺少完整options时拒绝，不越界读取旧模块。

| 文件options | 规范 |
| --- | --- |
| struct_size | 覆盖当前完整options |
| initial_directory | 空使用合法的环境HOME绝对路径提示，否则用`/`；非空必须为合法UTF-8绝对路径提示 |
| suggested_name | 仅Save可用，空由Host提供初始名称，否则为一个完整filename |
| extensions / extensions_size | Open/Save的允许后缀；0表示不筛选，最多16个 |
| flags | 目前仅ShowHidden=1，未知位拒绝 |

目录请求不携带filename或后缀。文件请求的choices必须为空；系统提供Cancel和明确
的Open/Save/Choose动作。title/message继续使用原长度及UTF-8限制。
当前文件面板显示请求title和provider状态，不显示业务message；需要用户阅读的业务
说明应留在应用正文中，不能依赖文件请求的message呈现。

路径最多4096字节，每个filename最多255字节；拒绝NUL、非法UTF-8及控制字符。
filename不能是空、`.`、`..`或包含`/`；Linux反斜杠是合法字面内容，不按DSL转义解释。
目录提示允许绝对路径别名及`.`/`..`，只在worker中canonicalize。
结果必须是规范绝对路径，无空段、`.`、`..`或尾斜杠；根`/`只可作为目录结果。

后缀最多32字节，使用ASCII `.ext`或`.tar.gz`形式；每段非空，仅字母、数字、`_`、`-`。
比较区分大小写，业务可显式列出多个后缀。不存在自动追加未声明后缀或绕过筛选的行为。

Result尾追加借用`file_path`及`overwrite_approved`（0/1）。文件Success的choice_id为0；
覆盖标志仅Save可为true，路径和后缀必须匹配请求。Confirmation成功仍只携带已知choice，
没有文件payload。Cancelled/Failed不能携带路径或覆盖标志，混合结果拒绝。
回调视图只在回调期间有效，业务需要继续异步工作时复制为自身值。

## 3. 异步模型、限额与取消

`FileTaskModel`使用Host已有scheduler的独立channel。所有canonicalize、目录枚举、
stat和选择校验均在worker执行；所有者线程只校验纯值、排队、接收不可变结果。
worker不访问Scene、模块、平台或GPU，也不修改业务值。

每次导航/校验获得独立单调generation，取消旧代并只保留最新待提交请求。
scheduler Busy时通过容量通知FD重试，不用Timer或忙轮询。迟到completion只清理，
不能更新新目录、选择、错误或结束下一任务。Host同时核对实际任务identity和当前generation。

目录snapshot包含canonical路径、不可变条目、条目ID和排除数量。条目ID只在该snapshot
中稳定，不是跨代次或跨owner权限。目录排在文件之前，同类按字节名称稳定排序。
显示名合法的普通目录总是保留，普通文件按后缀筛选；隐藏项按请求处理。

- 最多4096个可显示条目，最多扫描16384个非`.`/`..`原生条目。
- 不可变输出最多4MiB，并通过scheduler retention lease预算。
- 超限返回typed TooLarge，不把前半页称为完整目录。
- 无法用本阶段UTF-8路径契约表达的原生名排除并计数；symlink和特殊文件排除并计数。
- 初始目录别名可canonicalize；候选最后一个filename使用NOFOLLOW metadata核对，
  不能把symlink先canonicalize成普通文件后误认为原对象已通过。

Open要求现存普通文件，SelectDirectory要求现存目录；Save允许目标不存在，现存
目标必须为普通文件。选择校验返回canonical路径、parent stamp和可选target stamp，
不产生写入。stamp包含device/inode、size、mtime及ctime，供覆盖重验使用。

stop在枚举、排序和系统调用边界检查。阻塞文件系统调用不能被强制中断；停止channel
会等待其自身活动任务，不停止共享scheduler，也不把“发出取消”称为系统调用已经结束。

## 4. 呈现代次与输入

任务identity、文件generation、输入owner_modal_epoch是不同概念。目录、行投影（含选择
状态）、页码、Loading和覆盖模式变化会替换显示语义；SDK的`RefreshOwnerTask`刷新
局部作用域代次，撤销已有捕获/按键流，保留面板文本焦点和原owner焦点恢复记录。
相同任务回到Preparing，只有新快照被实际worker采用才Ready。

旧列表的按下/释放、空或外来快照都不能操作新列表。行action是受信面板的固定槽位，
Host只从当前已采用投影、当前snapshot及当前页解析条目；不能把行索引或字符串当授权。
文本值更新使用拥有的数据，不拼DSL源，不把保留前缀的编辑事件交给业务模块。
filename、status、selected_caption等纯文字回送和由输入派生的按钮enabled变化只发布
新呈现，不刷新模态代次，也不把Ready改回Preparing。当前Scene的enabled校验继续
阻止禁用操作；相同编辑值回送保留光标和选区，避免连续输入因等待新快照而丢字。

Loading/验证中禁用文件名编辑与提交；覆盖重验中也禁用Back，Cancel/Esc保持可用。
覆盖提示等待用户选择时Back恢复可用，编辑名称需先返回浏览状态。主题和窗口尺寸变化在
受支持的可读布局内保留当前目录与文本；低于320×240，或应用padding/有效clip导致固定
主操作不能完整显示与命中时，活动任务以PreparationFailed结束并收回面板。
切到其他owner不取消任务。UI替换、失效或退出按5a/5b撤销scope，
取消目录工作，丢弃迟到结果。模块销毁之后不再交付结果。

## 5. 图03布局与失败恢复

共享资源`owner-file-panel.prism`从owner下缘呈现，保留背后的工作上下文。
Host在Preview之后、Master安装之前配置无资源的受信模板。原应用必须为无region的
Box/Card根；Confirmation和File组合在同一个mounted `__prism_task_panel`区域内，
该区域保留一个内容子节点，模式绑定只显示当前任务树。应用根属性、正文几何和根末尾
Popup/Menu顺序保持；应用不能声明或通过公开SetBinding覆盖`__prism_task_`前缀。

`SupportsOwnerFileTasks()`要求已打开、未退出或失败的owner、当前已安装Scene、受信
文件模板和实际mounted面板，以及至少320×240逻辑viewport。尺寸使用逻辑单位；
模板刚配置但仍是旧Scene时不广告文件能力。Confirmation自己的240×180门槛保持。
每次面板准备还检查实际Scene尺寸、固定主操作bounds、footer容器和祖先有效clip。
即使viewport满足最低尺寸，应用几何仍可能使面板准备被拒绝；不裁切主操作或缩小字号绕过。

| 场景 | 模板尺度与内容 |
| --- | --- |
| viewport高度至少400 | 面板最大600×360、padding16；实际尺寸受owner可用空间约束 |
| viewport高度小于400 | 面板最大600×216、padding12；固定标题、Up/位置、Save输入和底部动作 |
| 宽至少640且高至少400 | 列表左侧显示Home/Location，右侧显示Location / Selection完整路径预览 |
| 窄窗或短窗 | 收起侧栏；完整位置或选择路径在列表内容的滚动详情中可读；短窗分页控件也在滚动内容中 |

标题、正文、辅助文字分别使用现有18/14/11主题字号；文件行和操作目标高32，组内
spacing8、宽窗分区spacing12。背景使用现有card材质、surfaceRaised及控件状态token。
Cancel、Up及最终Open/Save/Choose动作均有明确文字，页码区域宽64。
状态文字保留22高滚动视口，紧凑/短窗路径详情保留44高滚动视口，空列表不会压缩状态行框。

列表采用八个固定投影槽位并分页，空name隐藏未使用行；ScrollView处理页内纵向滚动，
不新增通用DSL动态列表。行名称可clip，SDK按实际shaping宽度折行title、状态和完整
位置或选择路径，详情可滚动读取；目录任务显示canonical目录，文件任务显示完整目标路径，
覆盖提示显示已验证的canonical目标。路径拼接超过4096字节时禁用提交并提示，保留完整
输入，不截断出一个不同的候选路径。没有Tooltip或Text.wrap属性，也不借此声明字体回退能力。

SDK接口为`ConfigureOwnerFilePanel(prepared)`、`BeginOwnerFileTask(request, view)`及
`UpdateOwnerFileTask(identity, view)`。`OwnerFilePanelView`只持有标题、目录提示、文字、
八行name/directory/selected值与状态布尔量，不访问文件系统。初始目录提示中的别名、
`.`或`..`可直接投影，worker结果仍遵守规范绝对路径约束。

只有Save显示32高的TextField。编辑buffer可暂存空值、斜杠或超过255字节的非法名称，
最多4096字节；更长粘贴截到完整UTF-8前缀并显示长度错误。提交仍要求合法的最多255
字节filename和匹配后缀，错误输入禁用Save并提供修正提示，不因显示buffer更大而放宽
请求、候选或结果契约。

空目录、Loading、无选择和访问失败有明确说明。目录读取及候选验证失败保留当前
位置和filename，允许更换位置/修正输入/重试；不会自动覆盖业务状态或冒充Success。

Save遇到现存目标时，在同一面板内显示覆盖确认，保留目标位置和名称；默认保留更安全
动作，不新开系统窗口或嵌套TaskSession。Replace后再次worker校验canonical路径、
parent identity和完整target stamp；对象变化则回到可恢复状态，要求重新审阅。
校验后出现的业务操作竞争仍由业务在实际写入时处理。

## 6. 业务请求示例

业务通过`PrismHostApiV1`查询能力并提交请求，Host复制这些借用值。以下Save请求不
提供业务message；显示标题是“Save document”，后续状态由provider生成。

```c
#include "prism/contracts/app_module.h"

static uint64_t RequestSave(const PrismHostApiV1 *host)
{
    if (!host || host->struct_size <
        offsetof(PrismHostApiV1, cancel_task) + sizeof(host->cancel_task) ||
        !host->task_capabilities || !host->request_task || !host->cancel_task ||
        !(host->task_capabilities(host->context) & PRISM_TASK_CAP_SAVE_FILE_V1)) {
        return 0;
    }

    const char title[] = "Save document";
    const char name[] = "notes.md";
    const PrismTaskStringViewV1 suffixes[] = {{".md", 3}};
    const PrismFileTaskOptionsV1 options = {
        sizeof(options), {NULL, 0}, {name, sizeof(name) - 1}, suffixes, 1, 0
    };
    const PrismTaskRequestV1 request = {
        sizeof(request), PRISM_TASK_SAVE_FILE_V1,
        {title, sizeof(title) - 1}, {NULL, 0}, NULL, 0, &options
    };
    return host->request_task(host->context, &request);
}
```

非零返回值关联异步`on_task_completed`，回调仍在提交返回之后。回调先检查result完整
尾字段和outcome，再复制需要保留的file_path与覆盖意图；Cancelled/Failed不启动文件
写入。模块应提供具名完成回调，不能把“选好路径”当成“已保存”。空initial_directory
使用经过纯值校验的环境`HOME`，缺失或非法时回退到`/`，不调用getpwuid查询；目录解析
仍在worker执行。本例不自动追加后缀，也不会创建或写入`notes.md`。

## 7. 实施与验证顺序

1. 尾追加请求/结果及完整字段兼容，pure DTO和ModuleSession拥有复制。
2. 异步目录模型、预算、取消/迟到代次、真实临时目录的候选校验。
3. 共享DSL面板、普通/短窗布局、作用域刷新和真实输入采用。
4. Host路由导航/分页/文字/提交、同任务覆盖重验及一次终态。
5. 相关自动测试、隔离原生Wayland/V3D验证和安装规则审计。

本阶段不自动替换正式VNC。自动策略、布局、SDK模拟metadata、原生GPU及用户视觉
认可分别记录；5d源码接入Notepad业务和异步关闭续接，验证单独记录。

## 8. 本阶段验证结果

正式Host/WM、Preferences模块及相关目标构建通过。最终同一轮CTest **24/24**通过，
包括文件ABI兼容、真实临时目录的异步模型、作用域刷新、面板/SDK、文字编辑与原生
虚拟输入回归。纯面板布局覆盖八种材质/配色×四视口×三种请求，共96组合；SDK用
真实FreeType/HarfBuzz和模拟提交metadata，不把其结果称为GPU呈现。

隔离的真实Wayland/V3D探针 **14/14**通过：原7个确认流程和7个文件流程，验证打开、
原生键盘改名、新目标、覆盖重验、对象变化后重新审阅、目录选择、旧输入拒绝、取消
与owner关闭。结果核对规范路径、覆盖意图、一次终态和provider未修改文件内容。
探针采用Square Light，其他主题为纯布局验证；不是全主题视觉、触控屏或FPS验收。

规范、Skill、diff、WM客户端边界及共享DSL安装规则检查通过。初轮失败、生产修复、
夹具纠偏及构建静态库恢复记录保留在
`dist/validation/interface-system-step5c/README.md`和summary。正式VNC仍为v26，
本阶段没有创建commit、deb或部署；Notepad接入和图05任务运动按后续步骤推进。
