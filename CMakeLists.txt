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
find_package(nlohmann_json 3.11 REQUIRED)
include(CTest)
option(PRISM_ENABLE_GLES "Build Skia Ganesh GLES backend" OFF)

# 1. Core Framework Subdirectory
add_subdirectory(prism/contracts)
add_subdirectory(prism/theme)
add_subdirectory(prism/launch)
add_subdirectory(prism/platform/protocols)
add_subdirectory(prism/platform/wayland_client)
add_subdirectory(prism/runtime)
add_subdirectory(prism/render_skia)
add_subdirectory(prism)
add_subdirectory(prism/host)
add_subdirectory(prism/launcher_runtime)
include(cmake/PrismAppPackage.cmake)

# 2. Demos Subdirectory
if(TARGET prism_client_app)
    add_subdirectory(demos)
endif()

# 3. Pure business modules and their DSL/assets packages
if(TARGET prism_client_app)
    add_subdirectory(prism-desktop)
    add_subdirectory(prism-topbar)
    add_subdirectory(prism-dock)
endif()

# 4. Tests and manual diagnostics are excluded from production builds.
if(BUILD_TESTING)
    add_subdirectory(tests)

    find_program(PRISM_STYLE_PYTHON python3)
    if(PRISM_STYLE_PYTHON)
        add_custom_target(prism-code-style
            COMMAND "${PRISM_STYLE_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/check-code-style.py"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Checking C/C++ formatting, goto and production file sizes"
            VERBATIM)
    endif()
endif()


# ==============================================================================
# 5. Installation Rules & Debian Packaging (CPack)
# ==============================================================================
include(GNUInstallDirs)

# Install platform binaries
install(TARGETS
    prism-wm
    prism-launcher
    prism-session-runtime
    prism-invoker
    prism-msg
    prism-pack
    prism-compiler
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install Session Entry Script
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

install(FILES resources/systemd/prism-demo@.service
    DESTINATION lib/systemd/system
)

# CPack Debian Package Configuration
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "prism-wm")
set(CPACK_PACKAGE_VENDOR "Project Prism Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Project Prism: Wayland Compositor and Skia GLES Desktop Clients")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "12")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_PACKAGE_CONTACT "Chen Bin <chenbin@prism.org>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Chen Bin <chenbin@prism.org>")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fonts-dejavu-core, libgl1-mesa-dri")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/chenbin/prism")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

set(PRISM_PACKAGE_HAS_CLIENTS OFF)
if(TARGET prism_client_app)
    set(PRISM_PACKAGE_HAS_CLIENTS ON)
endif()
configure_file(cmake/PrismPackaging.cmake.in
    "${CMAKE_BINARY_DIR}/PrismPackaging.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/PrismPackaging.cmake")

include(CPack)
