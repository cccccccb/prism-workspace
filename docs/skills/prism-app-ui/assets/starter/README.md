# Counter starter

Copy this directory into your own project. It is a developer template, not a
built-in Prism application or a test fixture.

```sh
cmake -S . -B build -DPRISM_SOURCE_ROOT=/absolute/path/to/prism-workspace
cmake --build build
```

The complete package is `build/share/prism/apps/counter_app/`. The module is
`counter.so`; it compiles against pure public headers without Qt, Skia, EGL or
Wayland. Build on the target architecture, or use an appropriate CMake toolchain.

If you choose to install it, use an explicit prefix:

```sh
cmake --install build --prefix /your/chosen/prefix
```

Register the resulting package directory with the launcher according to the
main skill's instructions. Installing files alone does not register an application.
Do not assume that a running launcher watches package directories for changes.

## Behavior and boundaries

- `preview.prism` is a quick, binding-free loading view using the active theme.
- `master.prism` declares typed string/number/bool bindings. `header` and `controls`
  are independent critical units. `detail` is independent deferred content; it has
  no artificial `after` dependency. The Host schedules it after Master presentation.
- `layout.prism` owns each stable Slot exactly once. Its placeholder has no action;
  component completion order cannot change the layout's painting order.
- The business module updates a bounded in-memory count. Progress stays in `[0,1]`.
  The toggle hides/shows the deferred Slot; this preserves the region and state.
  The refresh icon resets the value. The compact header and controls keep the main
  actions inside short BSP windows; details occupy the remaining space.
- `create` publishes initial values and reports BackendReady promptly. BackendReady
  is business readiness, not proof of a presented Master or installed deferred content.
  The Host applies current binding values when the deferred region is later mounted.
- Actions arrive at the owner-thread `on_action` callback. Strings are borrowed for
  the call; binding values are copied by the Host. No callback throws across the ABI.
- There is no async preparation job, periodic tick, persistence, image loading or
  audio. See the real Music module for bounded background work and completion handling.
- Theme colors/materials use the shipped `@accent`, `@text`, `@mutedText` and related
  tokens. The sample relies on those theme packages; a custom theme must supply them.

This standalone CMake project is deliberately not connected to the Prism repository's
production build or deb installation. The skill's validation instructions should be
run after copying or changing the template; this README does not claim a successful build.
