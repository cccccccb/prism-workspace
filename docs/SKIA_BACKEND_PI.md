# 第四步：Skia CPU 诊断后端接入

本检查点使用 Skia `canvaskit/0.39.1`（提交 `a004a27085d7dcc4efc3766c9abe92df03654c7c`）和其 DEPS 中 Wuffs 提交 `e3f919ccfe3ef542cfc983a82146070258fb57f8`。源码放在用户缓存目录，不进入仓库。构建配置关闭 Ganesh/Graphite 和图片解码，使用系统 libpng 满足 Skia 内部序列化链接，仅启用 CPU raster 与系统 FreeType/Fontconfig。GPU 性能不能由此判断。构建方式遵循 [Skia 官方 GN/Ninja 说明](https://skia.org/docs/user/build/)。

Pi 上安装依赖并构建：

```sh
sudo apt-get install generate-ninja ninja-build libfreetype-dev libfontconfig-dev libharfbuzz-dev libpng-dev
./tools/build-skia-pi.sh
cmake -S . -B build -G Ninja -DPRISM_SKIA_ROOT="$HOME/.cache/prism-deps/skia"
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`prism_skia_raster` 是独立诊断目标，不接入旧 `prism_core` 或当前生产 WM。它把 DisplayList 的矩形、圆角、裁剪、仿射变换和 glyph run 绘入 Wayland SHM 对应的 BGRA 像素，先验证命令栈和坐标。字体使用 HarfBuzz + FreeType 产生 glyph id 与位置，同一个字体文件由 Skia 光栅化。PNG 图片资源的异步解码与 DrawImage 检查点见 [IMAGE_RESOURCES.md](IMAGE_RESOURCES.md)；GPU 纹理、缩放与 shell 角色尚未实现。

诊断窗口：先启动 headless WM，再运行：

```sh
WLR_BACKENDS=headless WLR_RENDERER=pixman ./build/prism/prism-wm
./build/tests/prism_skia_wayland_probe wayland-prism-0 tests/fixtures/skia_probe.prism
```

Pi 上已测得两次 configure（640×400、1280×614）、三次 buffer commit 与两次 frame callback；WM 确认映射、解除映射和销毁。`skia_raster_test` 检查背景/圆角像素以及无效命令拒绝，客户端 Scene 单元测试检查按钮 action 命中。此处原始检查点完整 CTest 为 10/10 通过。后续图片资源测试另见 [IMAGE_RESOURCES.md](IMAGE_RESOURCES.md)。下一阶段接更完整的 DSL 语义，再测 V3D 的 Vulkan 与 GLES；只有实测后才选发布后端。

手工 probe 与其 DSL 样例只存在于 `tests/probes/` 和 `tests/fixtures/`，仅 `BUILD_TESTING=ON` 时构建；`prism/render_skia/` 只包含可复用渲染库。生产构建使用 `-DBUILD_TESTING=OFF`，不会生成 probe 或单元测试可执行文件。
