set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(XDG_CLIENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
set(XDG_CLIENT_CODE "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
add_custom_command(
    OUTPUT "${XDG_CLIENT_HEADER}" "${XDG_CLIENT_CODE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${XDG_CLIENT_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${XDG_CLIENT_CODE}"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM
)

set(PRESENTATION_XML "${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml")
set(PRESENTATION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h")
set(PRESENTATION_CODE "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-protocol.c")
add_custom_command(OUTPUT "${PRESENTATION_HEADER}" "${PRESENTATION_CODE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${PRESENTATION_XML}" "${PRESENTATION_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${PRESENTATION_XML}" "${PRESENTATION_CODE}"
    DEPENDS "${PRESENTATION_XML}" VERBATIM)

add_library(prism_wayland_client STATIC
    wayland_window.cpp
    "${PRESENTATION_HEADER}"
    "${PRESENTATION_CODE}"
    "${XDG_CLIENT_HEADER}"
    "${XDG_CLIENT_CODE}"
)
target_include_directories(prism_wayland_client PUBLIC
    "${CMAKE_SOURCE_DIR}/prism/include"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(prism_wayland_client PUBLIC prism_contracts
    prism_surface_protocol ${WAYLAND_CLIENT_LIBRARIES})

if(PRISM_ENABLE_GLES)
    pkg_check_modules(PRISM_WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
    pkg_check_modules(PRISM_EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(PRISM_GLES REQUIRED IMPORTED_TARGET glesv2)
    add_library(prism_wayland_egl STATIC wayland_egl_surface.cpp)
    target_include_directories(prism_wayland_egl PUBLIC "${CMAKE_SOURCE_DIR}/prism/include")
    target_link_libraries(prism_wayland_egl PUBLIC
        prism_wayland_client PkgConfig::PRISM_EGL PkgConfig::PRISM_WAYLAND_EGL PkgConfig::PRISM_GLES)
endif()
