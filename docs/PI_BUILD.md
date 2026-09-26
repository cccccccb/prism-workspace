# Raspberry Pi 构建与运行

当前构建目标是 Raspberry Pi OS / Debian 13 的 ARM64 系统，使用发行版提供的 wlroots 0.18。CMake 通过 pkg-config 查找头文件和库，不引用仓库内旧的 x86_64 wlroots 拷贝。

## 安装构建依赖

```sh
sudo apt-get update
sudo apt-get install -y cmake ninja-build libwlroots-0.18-dev
```

`libwlroots-0.18-dev` 会引入 Wayland、DRM、GBM、libinput、pixman、xkbcommon、wayland-protocols 等开发依赖。若使用别的 Debian 衍生发行版，先确认 `pkg-config --modversion wlroots-0.18` 可用。

## 编译与测试

```sh
cmake -S . -B build -G Ninja
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Pi 4B 上建议先用 `-j2`，避免并行 C++ 编译占满内存。构建过程会用 `wayland-scanner` 生成 XDG Shell 服务端协议头。wlroots 0.18 的少数公共 C 头使用 C99 数组参数语法，CMake 会在构建目录生成等价的 C++ 可解析头；系统安装的头文件不会修改。

## 无显示器运行检查

在仓库根目录运行：

```sh
WLR_BACKENDS=headless WLR_RENDERER=pixman \
  XDG_RUNTIME_DIR=/run/user/$(id -u) ./build/bin/prism-wm
```

看到 `Wayland display listening on wayland-prism-0` 和 `commit=OK` 表示服务端与输出帧正常运行。按 Ctrl+C 停止。此检查验证软件渲染和 Wayland 服务端；实际 DRM/KMS 显示输出还需要在本地图形会话或 TTY 上单独验证。

## 统一启动基础构建依赖

新版 prism_launch 包读取器使用 nlohmann JSON 3.11+，安装开发头：

```sh
sudo apt install nlohmann-json3-dev
```

业务模块 ABI 为纯 C；prism/contracts 仍可独立构建，无 JSON/Wayland/Skia 依赖。实际启动恢复进度见 [APP_LAUNCH_CONTRACT.md](APP_LAUNCH_CONTRACT.md)。

## 统一会话入口

Skia GLES 构建的正式入口是 build-gles/bin/prism-session-runtime；原始 prism-wm 仅提供无 Shell 的诊断会话。统一 host/包/服务启动与授权规范见 [SESSION_LAUNCH_RUNTIME.md](SESSION_LAUNCH_RUNTIME.md)。
