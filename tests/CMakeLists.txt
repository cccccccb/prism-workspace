add_executable(ipc_benchmark
    ipc_benchmark.cpp
)
target_link_libraries(ipc_benchmark PRIVATE prism_core)

add_executable(tiling_decoration_test
    tiling_decoration_test.cpp
)
target_link_libraries(tiling_decoration_test PRIVATE prism_core prism_aot_tools)

add_executable(tree_engine_test
    tree_engine_test.cpp
)
target_link_libraries(tree_engine_test PRIVATE prism_core)

add_executable(layer_manager_test
    layer_manager_test.cpp
)
target_link_libraries(layer_manager_test PRIVATE prism_core)

add_executable(contracts_smoke contracts_smoke.cpp)
target_link_libraries(contracts_smoke PRIVATE prism_contracts)
add_executable(wayland_lifecycle_test wayland_lifecycle_test.cpp)
target_link_libraries(wayland_lifecycle_test PRIVATE prism_core prism_wayland_client)
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME wayland_lifecycle_test COMMAND wayland_lifecycle_test)
add_test(NAME tree_engine_test COMMAND tree_engine_test)
add_test(NAME tiling_decoration_test COMMAND tiling_decoration_test)
add_test(NAME ipc_benchmark COMMAND ipc_benchmark)
add_test(NAME layer_manager_test COMMAND layer_manager_test)

add_executable(client_scene_test client_scene_test.cpp)
target_link_libraries(client_scene_test PRIVATE prism_client_dsl)
add_test(NAME client_scene_test COMMAND client_scene_test)
add_executable(client_app_templates_test client_app_templates_test.cpp)
target_link_libraries(client_app_templates_test PRIVATE prism_client_dsl)
target_compile_definitions(client_app_templates_test PRIVATE PRISM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME client_app_templates_test COMMAND client_app_templates_test)
add_executable(render_tree_test render_tree_test.cpp)
target_link_libraries(render_tree_test PRIVATE prism_client_scene)
add_test(NAME render_tree_test COMMAND render_tree_test)
add_executable(dsl_syntax_test dsl_syntax_test.cpp)
target_link_libraries(dsl_syntax_test PRIVATE prism_client_dsl)
add_test(NAME dsl_syntax_test COMMAND dsl_syntax_test)

# Manual protocol probes live beside tests and are never installed.
add_executable(wayland_lifecycle_probe probes/wayland_lifecycle_probe.cpp)
target_link_libraries(wayland_lifecycle_probe PRIVATE prism_wayland_client)

if(TARGET prism_skia_raster)
    add_executable(skia_raster_test skia_raster_test.cpp)
    target_link_libraries(skia_raster_test PRIVATE prism_skia_raster prism_client_dsl)
    target_compile_definitions(skia_raster_test PRIVATE
        PRISM_TEST_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checker.png")
    add_test(NAME skia_raster_test COMMAND skia_raster_test)

    add_executable(prism_skia_wayland_probe probes/skia_wayland_probe.cpp)
    target_link_libraries(prism_skia_wayland_probe PRIVATE
        prism_skia_raster prism_client_dsl prism_wayland_client)
endif()

if(TARGET prism_client_app)
    add_executable(prism_skia_gles_wayland_probe probes/skia_gles_wayland_probe.cpp)
    target_link_libraries(prism_skia_gles_wayland_probe PRIVATE
        prism_client_app)
endif()

if(TARGET prism_skia_gles)
    add_executable(skia_gles_parity_test skia_gles_parity_test.cpp)
    target_link_libraries(skia_gles_parity_test PRIVATE prism_skia_gles)
    add_test(NAME skia_gles_parity_test COMMAND skia_gles_parity_test)
endif()

add_executable(image_resources_test image_resources_test.cpp)
target_link_libraries(image_resources_test PRIVATE prism_client_dsl)
add_test(NAME image_resources_test COMMAND image_resources_test)
