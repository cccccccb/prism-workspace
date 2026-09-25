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

add_library(prism_wayland_client STATIC
    wayland_window.cpp
    "${XDG_CLIENT_HEADER}"
    "${XDG_CLIENT_CODE}"
)
target_include_directories(prism_wayland_client PUBLIC
    "${CMAKE_SOURCE_DIR}/prism/include"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(prism_wayland_client PUBLIC prism_contracts
    ${WAYLAND_CLIENT_LIBRARIES})

add_executable(wayland_lifecycle_probe lifecycle_probe.cpp)
target_link_libraries(wayland_lifecycle_probe PRIVATE prism_wayland_client)
