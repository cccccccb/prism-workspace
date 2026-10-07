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
    src/layout/fluid_split_strategy.cpp
    src/layout/fullscreen_strategy.cpp
    src/layout/mission_control_strategy.cpp
    src/wm/window.cpp
    src/wm/compositor.cpp
    src/wm/wlr_server.cpp
    src/wm/wlr_server_events.cpp
    src/wm/wlr_server_control.cpp
    src/wm/wlr_server_frames.cpp
    src/wm/wlr_server_input.cpp
    src/wm/wlr_server_touch.cpp
    src/wm/wlr_server_views.cpp
    src/wm/wlr_server_popup.cpp
    src/wm/wlr_server_popup_effects.cpp
    src/wm/wlr_server_ipc.cpp
    src/wm/wlr_server_status.cpp
    src/wm/wlr_server_layout.cpp
    src/wm/layout_control.cpp
    src/wm/wlr_server_layout_control.cpp
    src/wm/wlr_server_window_control.cpp
    src/wm/wlr_surface_fade.cpp
    src/wm/wlr_group_geometry.cpp
    src/wm/wlr_group_geometry_layout.cpp
    src/wm/wlr_surface_geometry.cpp
    src/wm/wlr_server_boundary.cpp
    src/wm/wlr_server_resize.cpp
    src/wm/wlr_server_group.cpp
    src/wm/wlr_server_group_input.cpp
    src/wm/wlr_server_surface_coords.cpp
    src/wm/surface_effects.cpp
    src/wm/surface_effects_content.cpp
    src/wm/surface_effects_gpu.cpp
    src/wm/surface_effects_contour.cpp
    src/wm/surface_effects_mask.cpp
    src/wm/surface_effects_update.cpp
    src/wm/effect_dependencies.cpp
    src/wm/layer_manager.cpp
    src/decoration/tiling_decoration_spec.cpp
    src/decoration/tiling_window_decorator.cpp
    src/decoration/tiling_drag_manager.cpp
    src/decoration/motion_controller.cpp
    src/tree/tree_node.cpp
    src/tree/tree_container.cpp
    src/tree/tree_workspace.cpp
    src/tree/tree_engine.cpp
    src/tree/tree_constraints.cpp
    src/tree/tree_boundary.cpp
    src/tree/tree_snapshot.cpp
)
set(XDG_SHELL_PROTOCOL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h")
add_custom_command(
    OUTPUT "${XDG_SHELL_PROTOCOL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" server-header
            "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
            "${XDG_SHELL_PROTOCOL_HEADER}"
    DEPENDS "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
    VERBATIM
)
add_custom_target(prism_wayland_protocol_headers DEPENDS "${XDG_SHELL_PROTOCOL_HEADER}")
add_dependencies(prism_core prism_wayland_protocol_headers)
# wlroots 0.18 exposes C99 [static N] array parameters in public C headers.
# C++ cannot parse that spelling; mirror only the affected headers with [N].
set(WLROOTS_CPP_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/wlroots_cpp")
foreach(WLROOTS_HEADER IN ITEMS wlr_buffer.h wlr_scene.h wlr_matrix.h)
    set(WLROOTS_HEADER_SOURCE "${WLROOTS_HEADER_ROOT}/wlr/types/${WLROOTS_HEADER}")
    file(READ "${WLROOTS_HEADER_SOURCE}" WLROOTS_HEADER_TEXT)
    string(REGEX REPLACE "\\[static ([0-9]+)\\]" "[\\1]" WLROOTS_HEADER_TEXT "${WLROOTS_HEADER_TEXT}")
    file(MAKE_DIRECTORY "${WLROOTS_CPP_INCLUDE_DIR}/wlr/types")
    file(CONFIGURE OUTPUT "${WLROOTS_CPP_INCLUDE_DIR}/wlr/types/${WLROOTS_HEADER}"
        CONTENT "${WLROOTS_HEADER_TEXT}" @ONLY)
endforeach()
target_include_directories(prism_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${WLROOTS_CPP_INCLUDE_DIR}
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
    prism_contracts
    prism_surface_effect_contract
    prism_animation_core
    prism_surface_protocol
    Threads::Threads
    rt
    PkgConfig::WLROOTS
    ${WAYLAND_SERVER_LIBRARIES}
    ${DRM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GBM_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${UDEV_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)
pkg_check_modules(PRISM_WM_GLES REQUIRED IMPORTED_TARGET glesv2)
target_link_libraries(prism_core
    PRIVATE PkgConfig::PRISM_WM_GLES
    PUBLIC nlohmann_json::nlohmann_json
)

# A production compositor must never compile client UI parsers or renderers.
get_target_property(PRISM_WM_SOURCES prism_core SOURCES)
file(GLOB PRISM_WM_HEADERS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/prism/wm/*.hpp")
list(APPEND PRISM_WM_SOURCES ${PRISM_WM_HEADERS})
foreach(PRISM_WM_SOURCE IN LISTS PRISM_WM_SOURCES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${PRISM_WM_SOURCE}" PRISM_WM_SOURCE_TEXT)
    if(PRISM_WM_SOURCE_TEXT MATCHES "#include [<\"](prism/(compiler|scene|sdk|render|render_skia|runtime|gui)/|imgui|include/core/Sk)")
        message(FATAL_ERROR "Client UI dependency in compositor source: ${PRISM_WM_SOURCE}")
    endif()
endforeach()

# Legacy AOT tooling remains isolated from the compositor and client runtime.
add_library(prism_aot_tools STATIC
    src/scene/node.cpp
    src/scene/container_node.cpp
    src/scene/leaf_nodes.cpp
    src/modifiers/modifier.cpp
    src/compiler/lexer.cpp
    src/compiler/parser.cpp
    src/compiler/binary_generator.cpp
    src/compiler/binary_loader.cpp
    src/core/logging.cpp src/decoration/tiling_decoration_spec.cpp src/pack/package.cpp)
target_include_directories(prism_aot_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism_aot_tools PUBLIC Threads::Threads PRIVATE nlohmann_json::nlohmann_json)

# 3. Tool: prism-compiler (DSL to .prismb AOT compiler)
add_executable(prism-compiler
    src/compiler/main.cpp
)
target_include_directories(prism-compiler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-compiler PRIVATE prism_aot_tools)

# Launcher/invoker are independent launch targets in prism/launcher_runtime.

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
target_link_libraries(prism-pack PRIVATE prism_aot_tools)

# 8. IPC Control Utility: prism-msg (Swaymsg-like IPC controller)
add_executable(prism-msg
    src/tools/prism_msg.cpp
)
target_include_directories(prism-msg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism-msg PRIVATE prism_launch_client nlohmann_json::nlohmann_json)

target_link_libraries(prism_core PUBLIC prism_control)

set_target_properties(prism-wm PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
