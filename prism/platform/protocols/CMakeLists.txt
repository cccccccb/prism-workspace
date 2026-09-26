set(PRISM_EFFECT_XML "${CMAKE_SOURCE_DIR}/protocols/prism-surface-effects-v1.xml")
set(PRISM_EFFECT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/prism-surface-effects-client.h" "${CMAKE_CURRENT_BINARY_DIR}/prism-surface-effects-server.h")
set(PRISM_EFFECT_CODE "${CMAKE_CURRENT_BINARY_DIR}/prism-surface-effects-v1.c")
add_custom_command(OUTPUT ${PRISM_EFFECT_HEADERS} "${PRISM_EFFECT_CODE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${PRISM_EFFECT_XML}" "${CMAKE_CURRENT_BINARY_DIR}/prism-surface-effects-client.h"
    COMMAND "${WAYLAND_SCANNER}" server-header "${PRISM_EFFECT_XML}" "${CMAKE_CURRENT_BINARY_DIR}/prism-surface-effects-server.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${PRISM_EFFECT_XML}" "${PRISM_EFFECT_CODE}"
    DEPENDS "${PRISM_EFFECT_XML}" VERBATIM)
add_library(prism_surface_protocol STATIC ${PRISM_EFFECT_HEADERS} "${PRISM_EFFECT_CODE}")
target_include_directories(prism_surface_protocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
