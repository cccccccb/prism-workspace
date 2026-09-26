# Skia Ganesh GLES 与 Wayland 呈现检查点

本检查点按照[总体架构文档](Wayland_DSL_Skia_Rendering_Engine_Architecture_Detailed.md)第四步推进。客户端保留 DSL → Scene → DisplayList；Skia Ganesh 在 EGL 的 GLES framebuffer 绘制，EGL Wayland WSI 负责实际 buffer 提交。平台层只拥有 `wl_surface`、EGL 对象和 frame callback；DSL/Scene 不依赖 GL 或 Wayland 类型。CPU Skia 仍是诊断后端，不作为发布时的第二条 UI 路径。

## Pi 4B 环境与构建

在本机 headless wlroots 中，`WLR_RENDERER=gles2` 报告 EGL driver `v3d`、GL vendor `Broadcom`、GL renderer `V3D 4.2.14.0`，OpenGL ES 3.1。最初客户端只看到 `wl_shm`，Mesa Wayland EGL 因缺少 `linux-dmabuf` global 退回 llvmpipe。WM 现通过 wlroots 创建 `linux-dmabuf-v1` 并关联 scene；重测客户端报告 `Broadcom / V3D 4.2.14.0`、OpenGL ES 3.1，`configure=2 frame=2 presented=3 images=1/1`。探针在客户端不是 V3D 时判失败。

构建依赖除 CPU 检查点的包外，还需要 `libwayland-dev`、`libegl-dev`、`libgles-dev` 和 `libgl-dev`。此版 Skia 的 GL 静态库引用 GLX 入口，因此即使客户端运行的是 EGL/GLES，也要在链接时提供系统 `libGL`。

```sh
tools/build-skia-pi.sh gles
cmake -S . -B build-gles -G Ninja \
  -DPRISM_SKIA_ROOT="$HOME/.cache/prism-deps/skia" \
  -DPRISM_SKIA_VARIANT=PrismGLES -DPRISM_ENABLE_GLES=ON -DBUILD_TESTING=ON
cmake --build build-gles --target prism_skia_gles_wayland_probe -j4
```

`tests/probes/skia_gles_wayland_probe.cpp` 使用测试 DSL、异步 PNG、字体整形与 slot 更新，分别报告客户端 GL 驱动、configure、frame callback、提交次数与图片加载结果。运行时需启动 headless WM，再将 socket 名与测试 DSL 文件传给 probe。它只用于集成验收，不进入安装目标。

本机 `build-gles` 完整构建成功，CTest 11/11 通过；上述 headless V3D 探针也以退出码 0 通过。CPU 配置 `build` 的 CTest 同为 11/11 通过。

```sh
WLR_BACKENDS=headless WLR_RENDERER=gles2 ./build/prism/prism-wm
./build-gles/tests/prism_skia_gles_wayland_probe \
  wayland-prism-0 tests/fixtures/skia_probe.prism
```

## 当前边界

- 当前尺寸按 scale=1 处理；HiDPI、fractional scale 与 resize 压力测试未完成。
- EGL WSI 管理 GPU buffer 和释放；当前客户端不自行导出 dma-buf。仍需验证关闭/重新创建、丢失上下文和错误恢复。
- headless WSI 验收只能证明 GPU 客户端提交及合成器处理，不能代替物理 DRM/KMS 显示的帧时和功耗测量。
- Vulkan 后端尚未完成同等功能对比。选择最终发布后端前，按迁移计划测物理显示上的功能与延迟。
