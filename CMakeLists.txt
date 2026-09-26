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
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.18)
find_path(WLROOTS_HEADER_ROOT wlr/types/wlr_scene.h
    HINTS ${WLROOTS_INCLUDE_DIRS} NO_DEFAULT_PATH REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

add_compile_definitions(WLR_USE_UNSTABLE)

find_package(Threads REQUIRED)
include(CTest)
option(PRISM_ENABLE_GLES "Build Skia Ganesh GLES backend" OFF)

# 1. Core Framework Subdirectory
add_subdirectory(prism/contracts)
add_subdirectory(prism/platform/wayland_client)
add_subdirectory(prism/runtime)
add_subdirectory(prism/render_skia)
add_subdirectory(prism)

# 2. Demos Subdirectory
if(TARGET prism_client_app)
    add_subdirectory(demos)
endif()

# 3. Tests and manual diagnostics are excluded from production builds.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 4. Shell Standalone Projects (Desktop, TopBar, Dock)
if(TARGET prism_client_app)
    add_subdirectory(prism-desktop)
    add_subdirectory(prism-topbar)
    add_subdirectory(prism-dock)
endif()

# ==============================================================================
# 5. Installation Rules & Debian Packaging (CPack)
# ==============================================================================
include(GNUInstallDirs)

# Install Core & Shell Binaries
install(TARGETS
    prism-wm
    prism-launcher
    prism-invoker
    prism-msg
    prism-pack
    prism-compiler
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TARGET prism_client_app)
    install(TARGETS prism-desktop prism-topbar prism-dock demo_player demo_settings
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Install Session Supervisor Script
install(PROGRAMS session/prism-session
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install Wayland Session Desktop Entry
install(FILES resources/sessions/prism.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions
)

# Install Systemd User Service
install(FILES resources/systemd/prism-session.service
    DESTINATION lib/systemd/user
)

# Install Wallpapers & Assets
install(DIRECTORY resources/wallpapers/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/wallpapers
)

# Install DSL UI Templates
install(FILES prism-desktop/ui/desktop.prism
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/ui
)
install(FILES prism-topbar/ui/topbar.prism
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/ui
)
install(FILES prism-dock/ui/dock.prism
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/ui
)
install(FILES demos/demo_player/master.prism
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/ui RENAME demo_player.prism)
install(FILES demos/demo_settings/master.prism
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prism/ui RENAME demo_settings.prism)

# CPack Debian Package Configuration
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "prism-wm")
set(CPACK_PACKAGE_VENDOR "Project Prism Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Project Prism: Next-Generation Wayland Compositor & Glassmorphism Desktop Shell")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_CONTACT "Chen Bin <chenbin@prism.org>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Chen Bin <chenbin@prism.org>")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libwlroots-0.18, libwayland-server0, libwayland-client0, libdrm2, libegl1, libgbm1, libinput10, libxkbcommon0, libpixman-1-0")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/chenbin/prism")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

include(CPack)
