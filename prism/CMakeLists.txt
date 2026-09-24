cmake_minimum_required(VERSION 3.22)
project(prism_framework
    VERSION 0.1.0
    DESCRIPTION "Prism Core Framework: Decoupled Wayland Window Manager & Application Runtime"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. Core library: libprism_core
add_library(prism_core
    src/core/logging.cpp
    src/ipc/channel.cpp
    src/ipc/ipc_server.cpp
    src/scene/node.cpp
    src/scene/container_node.cpp
    src/scene/leaf_nodes.cpp
    src/modifiers/modifier.cpp
    src/layout/fluid_split_strategy.cpp
    src/layout/fullscreen_strategy.cpp
    src/layout/mission_control_strategy.cpp
    src/lifecycle/preview_state.cpp
    src/lifecycle/transition_state.cpp
    src/lifecycle/master_state.cpp
    src/wm/window.cpp
    src/wm/compositor.cpp
    src/wm/wlr_server.cpp
    src/wm/layer_manager.cpp
    src/compiler/lexer.cpp
    src/compiler/parser.cpp
    src/compiler/binary_generator.cpp
    src/compiler/binary_loader.cpp
    src/pack/package.cpp
    src/render/framebuffer.cpp
    src/render/canvas_renderer.cpp
    src/decoration/tiling_decoration_spec.cpp
    src/decoration/tiling_window_decorator.cpp
    src/decoration/tiling_drag_manager.cpp
    src/decoration/motion_controller.cpp
    src/tree/tree_node.cpp
    src/tree/tree_container.cpp
    src/tree/tree_workspace.cpp
    src/tree/tree_engine.cpp
)
link_directories(${WLROOTS_LIBRARY_DIRS})
target_link_directories(prism_core PUBLIC ${WLROOTS_LIBRARY_DIRS})

target_include_directories(prism_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_link_libraries(prism_core PUBLIC
    Threads::Threads
    rt
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${DRM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GBM_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)

# 2. SDK library: libprism_sdk (for application backends & Wayland client runtime)
add_library(prism_sdk
    src/sdk/event_dispatcher.cpp
    src/sdk/application.cpp
)
target_include_directories(prism_sdk PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_link_libraries(prism_sdk PUBLIC 
    prism_core
    ${WAYLAND_CLIENT_LIBRARIES}
)

# 3. Tool: prism-compiler (DSL to .prismb AOT compiler)
add_executable(prism-compiler
    src/compiler/main.cpp
)
target_include_directories(prism-compiler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-compiler PRIVATE prism_core)

# 4. Daemon: prism-launcher (Zygote Daemon)
add_executable(prism-launcher
    src/launcher/zygote_server.cpp
    src/launcher/main.cpp
)
target_include_directories(prism-launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-launcher PRIVATE prism_core dl)

# 5. Daemon: prism-invoker (App Dispatcher & Validator)
add_executable(prism-invoker
    src/invoker/launch_pipeline.cpp
    src/invoker/main.cpp
)
target_include_directories(prism-invoker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-invoker PRIVATE prism_core)

# 6. Compositor: prism-wm (Window Manager & Compositor Core)
add_executable(prism-wm
    src/wm/main.cpp
)
target_include_directories(prism-wm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-wm PRIVATE prism_core)

# 7. Packaging Tool: prism-pack (.prismpkg manager)
add_executable(prism-pack
    src/pack/main.cpp
)
target_include_directories(prism-pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-pack PRIVATE prism_core)

# 8. IPC Control Utility: prism-msg (Swaymsg-like IPC controller)
add_executable(prism-msg
    src/tools/prism_msg.cpp
)
target_include_directories(prism-msg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-msg PRIVATE prism_core)

