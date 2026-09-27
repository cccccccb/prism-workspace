cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(prism_contracts LANGUAGES C CXX)
    include(CTest)
endif()

# This target must configure and compile without Wayland, wlroots, Skia or ImGui.
add_library(prism_contracts INTERFACE)
target_compile_features(prism_contracts INTERFACE cxx_std_20)
target_include_directories(prism_contracts INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/../include")
get_target_property(CONTRACT_LINK_LIBRARIES prism_contracts INTERFACE_LINK_LIBRARIES)
if(CONTRACT_LINK_LIBRARIES)
    message(FATAL_ERROR "prism_contracts must not link external or project libraries")
endif()

file(GLOB_RECURSE CONTRACT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../include/prism/contracts/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../include/prism/contracts/*.h")
foreach(CONTRACT_HEADER IN LISTS CONTRACT_HEADERS)
    file(STRINGS "${CONTRACT_HEADER}" CONTRACT_INCLUDES
        REGEX "^[ \t]*#[ \t]*include")
    foreach(CONTRACT_INCLUDE IN LISTS CONTRACT_INCLUDES)
        if(CONTRACT_INCLUDE MATCHES "[<\"](wlr/|wayland|vulkan/|include/core/Sk|imgui|prism/(wm|sdk|scene|compiler|ipc)/)")
            message(FATAL_ERROR "Backend/private dependency in contract header ${CONTRACT_HEADER}: ${CONTRACT_INCLUDE}")
        endif()
    endforeach()
    file(READ "${CONTRACT_HEADER}" CONTRACT_CONTENT)
    if(CONTRACT_CONTENT MATCHES "(^|[^A-Za-z0-9_])(Sk[A-Z][A-Za-z0-9_]*|Vk[A-Z][A-Za-z0-9_]*|wl_[A-Za-z0-9_]+)")
        message(FATAL_ERROR "Backend type leaked into contract header ${CONTRACT_HEADER}")
    endif()
endforeach()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_executable(prism_contracts_smoke "${CMAKE_CURRENT_LIST_DIR}/../../tests/contracts_smoke.cpp")
    target_link_libraries(prism_contracts_smoke PRIVATE prism_contracts)
    add_test(NAME prism_contracts_smoke COMMAND prism_contracts_smoke)
    add_executable(prism_app_module_abi_smoke "${CMAKE_CURRENT_LIST_DIR}/../../tests/app_module_abi_smoke.c")
    target_include_directories(prism_app_module_abi_smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
    add_test(NAME prism_app_module_abi_smoke COMMAND prism_app_module_abi_smoke)
endif()

# The binary theme transport is a backend independent value codec.
add_library(prism_theme_contract STATIC theme.cpp)
target_link_libraries(prism_theme_contract PUBLIC prism_contracts)

# Structural drawing validation has no resource registry or backend dependency.
add_library(prism_display_list_contract STATIC display_list_validation.cpp)
target_link_libraries(prism_display_list_contract PUBLIC prism_contracts)
