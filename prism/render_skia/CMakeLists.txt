set(PRISM_SKIA_ROOT "" CACHE PATH "Pinned Skia source/build root")
if(NOT PRISM_SKIA_ROOT)
    message(STATUS "Skia diagnostic target disabled (set PRISM_SKIA_ROOT)")
    return()
endif()
set(PRISM_SKIA_LIBRARY "${PRISM_SKIA_ROOT}/out/PrismCPU/libskia.a" CACHE FILEPATH "Skia static library")
if(NOT EXISTS "${PRISM_SKIA_ROOT}/include/core/SkCanvas.h" OR NOT EXISTS "${PRISM_SKIA_LIBRARY}")
    message(FATAL_ERROR "Skia headers or ${PRISM_SKIA_LIBRARY} missing; build pinned Skia first")
endif()
foreach(SKIA_SUPPORT_LIBRARY IN ITEMS libskcms.a libwuffs.a)
    if(NOT EXISTS "${PRISM_SKIA_ROOT}/out/PrismCPU/${SKIA_SUPPORT_LIBRARY}")
        message(FATAL_ERROR "Missing Skia support archive ${SKIA_SUPPORT_LIBRARY}")
    endif()
endforeach()
pkg_check_modules(PRISM_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(PRISM_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(PRISM_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(PRISM_PNG REQUIRED IMPORTED_TARGET libpng)
add_library(prism_skia_raster STATIC raster_renderer.cpp)
target_include_directories(prism_skia_raster PRIVATE "${PRISM_SKIA_ROOT}")
target_include_directories(prism_skia_raster PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(prism_skia_raster PUBLIC prism_client_scene
    "${PRISM_SKIA_LIBRARY}"
    "${PRISM_SKIA_ROOT}/out/PrismCPU/libskcms.a"
    "${PRISM_SKIA_ROOT}/out/PrismCPU/libwuffs.a"
    PkgConfig::PRISM_HARFBUZZ PkgConfig::PRISM_FREETYPE
    PkgConfig::PRISM_FONTCONFIG PkgConfig::PRISM_PNG)
add_executable(prism_skia_wayland_probe skia_wayland_probe.cpp)
target_link_libraries(prism_skia_wayland_probe PRIVATE prism_skia_raster prism_client_dsl prism_wayland_client)

add_executable(skia_raster_test "${CMAKE_SOURCE_DIR}/tests/skia_raster_test.cpp")
target_link_libraries(skia_raster_test PRIVATE prism_skia_raster prism_client_dsl)
add_test(NAME skia_raster_test COMMAND skia_raster_test)
