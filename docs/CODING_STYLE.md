# C/C++ 代码规范

日期：2026-09-27。适用于 Prism 自有生产代码、工具和业务模块。排版采用接近 Qt 的
传统 C++ 风格，使用标准 C++ 与现有依赖实现；不引入 Qt 类型、接口、信号槽、宏或运行库。
`.clang-format` 是自动排版的唯一配置，固定使用 clang-format 19。

## 1. 排版与命名

- 四空格缩进，不使用 Tab；一行通常不超过 100 列，无法拆分的字符串等由 formatter 保留。
- 函数定义的左花括号另起一行；类、结构体、namespace 和控制语句左花括号保持同一行。
- `if/else/for/while` 的语句体使用花括号；短函数、多个语句不能压缩在一行。
- 指针和引用符号靠近变量名：`Window *window`、`const Event &event`。
- namespace 内不增加额外缩进；访问限定符与类边界对齐；相邻函数定义之间留一空行。
- 类型保持 PascalCase。已有公开 C++ 接口、字段名和 C ABI 符号保留当前命名，不在
  排版调整中破坏调用者。新方法、成员和局部变量遵守所在组件的命名惯例；协议 JSON
  key、DSL 名称与 ABI 名称不因代码命名而变化。
- 单个声明表达一种职责；相互独立的变量、操作分别声明和书写。include 按功能分组，
  项目自身头文件优先；不依赖其他头文件偶然引入的标准库声明。

## 2. 函数与处理阶段

函数按一个主要职责组织，优先提前返回以减少嵌套。禁止 `goto`，清理使用已有 RAII
对象、明确的具名清理函数和结构化控制流程。不要在整理代码时悄悄改变错误处理顺序。

函数内部用空行区分校验、准备、主要处理、提交和清理。一个阶段中的紧密相关操作
放在一起；不在每一行后插入空行，也不删除有意义的阶段分隔以压缩文件长度。

```cpp
bool Surface::Submit(const Request &request)
{
    if (!Validate(request)) {
        return false;
    }

    auto prepared = Prepare(request);
    if (!prepared) {
        return false;
    }

    Render(*prepared);

    return Commit(*prepared);
}
```

## 3. Lambda 与回调

Lambda 只用于调用点附近、同步执行的短小局部算法，例如 `find_if`、`sort`、`erase_if`
的谓词。通常不超过十行格式化代码；一旦有多个处理阶段、递归遍历或复杂错误处理，
提取为具名函数、成员或辅助对象。

保存在对象中的回调、事件监听、线程入口和异步完成处理使用具名实现。C 接口采用
具名静态函数和明确的 context 指针；C++ 接口可用 `std::bind_front` 绑定具名成员，
或者使用具名函数对象。现有 `std::function` 接口可以保留，其业务实现应可独立阅读。
不得用大型 lambda 在初始化函数中隐藏整条处理链，也不得用一行包装 lambda 规避规则。

绑定对象的生命期必须覆盖回调，销毁前按现有生命周期解除监听或停止任务。无捕获
lambda 也不作为长期 C 回调的默认写法。具名实现本身不应改变原来的回调时机或线程。

## 4. JSON 边界

JSON 输入先解析为 typed DTO/结构体，再交给业务逻辑；输出先构造 typed DTO，再由
`to_json/from_json` 或等价具名序列化接口转换。沿用项目现有 nlohmann/json，不新增 Qt
JSON 依赖。不用 `find/substr` 提取 JSON 字段，不手写引号、逗号、数组或字符串转义。

字段名称字符串允许集中出现在序列化/反序列化实现中；这属于 wire schema，不能让
业务层散落 `json["key"]`、`json.at("key")` 等访问。可选字段是否省略、默认值、类型、
范围及错误分类必须明确保持，不能为了宏生成序列化而放松现有校验。

输入边界保留大小、嵌套深度、重复 key、未知字段、整数范围、ABI 版本及路径验证。
二进制 launch/control/worker 协议继续使用其已有 typed 编解码，不改成 JSON。
输出状态、窗口树、监控计数等保持既有 JSON key 与结构，结构调整必须另行设计。

## 5. 文件拆分

每个自有生产 `.cpp/.hpp/.c/.h` 文件最多 **800 个物理行**，包含注释和空行。
适用范围包括内核/WM、框架、平台层、host、launcher、工具、模块及私有实现头。
测试文件不受行数限制；第三方依赖和构建生成文件也不参与本项目行数门槛。

按真实职责拆分，例如生命周期、协议事件、输入、提交、资源、布局、损伤和序列化。
同一个类可以在多个 `.cpp` 中实现；必要时使用不安装的 `_p.hpp` 私有声明。新增编译
单元必须列入 CMake，不通过 `#include` 一个 `.cpp` 拼装，不用压缩排版或删注释绕过限制。
公共头只暴露调用所需契约；第三方和平台细节尽量保持在私有实现。

## 6. 执行与检查

```sh
sudo apt install clang-format-19
python3 tools/check-code-style.py --format
python3 tools/check-code-style.py
# 配置 BUILD_TESTING=ON 的构建目录也提供相同门槛：
cmake --build build-gles --target prism-code-style
```

工具扫描 Git 中自有 C/C++ 生产文件以及未忽略的新源文件，检查 800 行、真实 `goto`
token 和 clang-format 一致性；测试只检查排版与 goto，不检查行数。它不扫描 deps、
build、dist 或生成物。`--format` 仅修改排版，随后仍执行行数与 goto 检查。

Lambda 的同步/存储语义、函数阶段空行和 JSON 边界需要代码审查；不以正则检查冒充
完整 C++ 语义检查。结构拆分、回调迁移与 JSON 边界改变使用现有行为和协议门槛验证。
仅排版改动不编写镜像实现的测试；发现具体覆盖缺口时补充有意义的边界用例。

测试、fixture、性能 probe 保持在 `tests/`。本规范检查工具属于开发工具，不进入
桌面运行链或 deb 包。生产接口、包布局及内核/前端解耦规范继续有效。

## 7. 本轮整理与验证

渲染优化先提交为 `25064e5`，随后进行本轮代码整理。194 个自有生产 C/C++ 文件已统一
格式并满足 800 行限制，最大文件为 `prism/render_skia/raster_renderer.cpp`，715 行。
48 个测试/fixture/probe C/C++ 文件采用同一排版，不受行数限制；第三方依赖未改写。

- WM 服务器按生命周期、事件、控制、帧、输入、view、IPC 与状态查询拆分；原 1,923 行
  的 `wlr_server.cpp` 保留生命周期主体，当前 298 行。背景效果按内容依赖、GPU 与更新拆分。
- Scene 分离属性与区域几何；ClientApplication 分离 UI/资源、像素提交及公开生命周期；
  WaylandWindow 分离协议事件；launcher 分离请求、主题、控制、worker 与事件循环。
- 持久回调改为具名成员/静态函数与绑定；递归遍历、图标绘制和复杂局部过程改为具名辅助
  实现。保留短小的同步算法谓词。公共接口与业务 C ABI 保持，WM 不链接客户端前端。
- 清单解析、窗口树、WM 状态/控制响应及工具输出使用 typed JSON 边界。保留已有字段、
  optional 的 false/零值与数值精度规则。旧 pack 工具正确识别转义及顶层字段，非法 JSON
  与错误字段类型明确失败；该行为修正由独立边界用例验证。

全量构建及最终增量确认通过；完整 CTest **39/39**，包含新增 `json_boundary_test`。
Host、预热池、失败 worker、带继承 helper 的会话启动/崩溃/退出回归通过；WM 真实 GLES
验证 **50 个功能场景 + 4 个诊断记录**。真实 V3D SDK 像素、损伤、提交、resize 与图片
门槛通过，native 像素对照为 74 场景/75 次读回，使用已有渲染规范的数值比较规则。
格式检查、goto/行数门槛及 `git diff --check` 通过；WM 符号审计未见客户端前端，动态
依赖未见 Qt。测试代码和诊断工具仍独立于生产源码与安装规则。

证据位于 `dist/validation/prism-code-style/`。初轮配置的 imported target 作用域问题、
C 协议同名函数遮蔽类型问题及 JSON 小数精度差异均已修正，初始日志保留；最终通过
记录为 `build-verified.log`、`ctest-final.log`、`runtime-gates.json`、`sdk-native-gates.json`、
`style-target.log` 和 `wm-boundary.json`。本轮没有重新打包或替换已安装桌面。
