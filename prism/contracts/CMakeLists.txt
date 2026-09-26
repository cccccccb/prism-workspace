cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(prism_contracts LANGUAGES C CXX)
    include(CTest)
endif()

# This target must configure and compile without Wayland, wlroots, Skia or ImGui.
add_library(prism_contracts INTERFACE)
target_compile_features(prism_contracts INTERFACE cxx_std_20)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PRISM_THEME_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../resources/themes/prism-glass.json")
set(PRISM_THEME_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate-theme.py")
set(PRISM_CONTRACT_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PRISM_THEME_SOURCE}" "${PRISM_THEME_GENERATOR}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PRISM_THEME_GENERATOR}"
    "${PRISM_THEME_SOURCE}" "${PRISM_CONTRACT_GENERATED_INCLUDE}/prism/contracts/theme_tokens.hpp"
    RESULT_VARIABLE PRISM_THEME_RESULT)
if(NOT PRISM_THEME_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot generate the Prism theme contract")
endif()
target_include_directories(prism_contracts INTERFACE
    "${PRISM_CONTRACT_GENERATED_INCLUDE}"
    "${CMAKE_CURRENT_LIST_DIR}/../include")
install(FILES "${PRISM_THEME_SOURCE}" DESTINATION share/prism/themes)
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
