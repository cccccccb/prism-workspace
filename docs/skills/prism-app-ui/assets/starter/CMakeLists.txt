cmake_minimum_required(VERSION 3.20)
project(PrismCounterApp VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(PRISM_SOURCE_ROOT "" CACHE PATH "Prism repository root containing prism/include")
if(NOT EXISTS "${PRISM_SOURCE_ROOT}/prism/include/prism/contracts/app_module.h" OR
   NOT EXISTS "${PRISM_SOURCE_ROOT}/prism/include/prism/app/module_support.hpp")
    message(FATAL_ERROR "Set -DPRISM_SOURCE_ROOT=/absolute/path/to/prism-workspace")
endif()

# Compile against public value contracts only. Never add the Prism repository
# as a subdirectory or link its SDK, compositor or graphics implementations.
add_library(counter_contract_headers INTERFACE)
target_include_directories(counter_contract_headers INTERFACE
    "${PRISM_SOURCE_ROOT}/prism/include")
target_compile_features(counter_contract_headers INTERFACE cxx_std_20)

set(COUNTER_PACKAGE "${CMAKE_BINARY_DIR}/share/prism/apps/counter_app")
add_library(counter_business MODULE module.cpp)
target_link_libraries(counter_business PRIVATE counter_contract_headers)
set_target_properties(counter_business PROPERTIES
    PREFIX ""
    OUTPUT_NAME counter
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${COUNTER_PACKAGE}")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(counter_business PROPERTIES
        "LIBRARY_OUTPUT_DIRECTORY_${configuration}" "${COUNTER_PACKAGE}")
endforeach()

set(COUNTER_RESOURCES manifest.json preview.prism master.prism layout.prism
    ui/header.prism ui/controls.prism ui/detail.prism assets/README.md)
foreach(resource IN LISTS COUNTER_RESOURCES)
    get_filename_component(resource_directory "${resource}" DIRECTORY)
    file(MAKE_DIRECTORY "${COUNTER_PACKAGE}/${resource_directory}")
    configure_file("${resource}" "${COUNTER_PACKAGE}/${resource}" COPYONLY)
endforeach()

# This standalone consumer install is opt-in via cmake --install. No root Prism
# production target, registry or installed application list references this example.
set(COUNTER_INSTALL "${CMAKE_INSTALL_DATADIR}/prism/apps/counter_app")
install(TARGETS counter_business LIBRARY DESTINATION "${COUNTER_INSTALL}")
install(FILES manifest.json preview.prism master.prism layout.prism
    DESTINATION "${COUNTER_INSTALL}")
install(DIRECTORY ui assets DESTINATION "${COUNTER_INSTALL}")
