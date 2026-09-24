cmake_minimum_required(VERSION 3.22)
project(prism_workspace
    VERSION 0.1.0
    DESCRIPTION "Project Prism Workspace"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compile flags
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -O2
    -g
)

# Global dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(UDEV REQUIRED libudev)
pkg_check_modules(PIXMAN REQUIRED pixman-1)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

add_compile_definitions(WLR_USE_UNSTABLE)

set(WLROOTS_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/deps/wlroots/usr/include")
set(WLROOTS_LIBRARY_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/deps/wlroots/usr/lib/x86_64-linux-gnu")
set(WLROOTS_LIBRARIES wlroots)

link_directories(${WLROOTS_LIBRARY_DIRS})

find_package(Threads REQUIRED)

# 1. Core Framework Subdirectory
add_subdirectory(prism)

# 2. Demos Subdirectory
add_subdirectory(demos)

# 3. Tests Subdirectory
enable_testing()
add_subdirectory(tests)

# 4. Shell Standalone Projects (Desktop, TopBar, Dock)
add_subdirectory(prism-desktop)
add_subdirectory(prism-topbar)
add_subdirectory(prism-dock)
