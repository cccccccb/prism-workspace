add_executable(ipc_benchmark
    ipc_benchmark.cpp
)
target_link_libraries(ipc_benchmark PRIVATE prism_core)

add_executable(tiling_decoration_test
    tiling_decoration_test.cpp
)
target_link_libraries(tiling_decoration_test PRIVATE prism_core)

add_executable(tree_engine_test
    tree_engine_test.cpp
)
target_link_libraries(tree_engine_test PRIVATE prism_core)

add_executable(dsl_widgets_test
    dsl_widgets_test.cpp
)
target_link_libraries(dsl_widgets_test PRIVATE prism_core prism_sdk)

add_executable(layer_manager_test
    layer_manager_test.cpp
)
target_link_libraries(layer_manager_test PRIVATE prism_core prism_sdk)

add_executable(shell_experimental_test
    shell_experimental_test.cpp
)
target_link_libraries(shell_experimental_test PRIVATE prism_core prism_sdk)

add_executable(contracts_smoke contracts_smoke.cpp)
target_link_libraries(contracts_smoke PRIVATE prism_contracts)
add_executable(wayland_lifecycle_test wayland_lifecycle_test.cpp)
target_link_libraries(wayland_lifecycle_test PRIVATE prism_core prism_wayland_client)
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME wayland_lifecycle_test COMMAND wayland_lifecycle_test)
add_test(NAME tree_engine_test COMMAND tree_engine_test)
add_test(NAME tiling_decoration_test COMMAND tiling_decoration_test)
add_test(NAME ipc_benchmark COMMAND ipc_benchmark)
add_test(NAME dsl_widgets_test COMMAND dsl_widgets_test)
add_test(NAME layer_manager_test COMMAND layer_manager_test)
add_test(NAME shell_experimental_test COMMAND shell_experimental_test)

add_executable(client_scene_test client_scene_test.cpp)
target_link_libraries(client_scene_test PRIVATE prism_client_dsl)
add_test(NAME client_scene_test COMMAND client_scene_test)

# Manual protocol probes live beside tests and are never installed.
add_executable(wayland_lifecycle_probe probes/wayland_lifecycle_probe.cpp)
target_link_libraries(wayland_lifecycle_probe PRIVATE prism_wayland_client)

if(TARGET prism_skia_raster)
    add_executable(skia_raster_test skia_raster_test.cpp)
    target_link_libraries(skia_raster_test PRIVATE prism_skia_raster prism_client_dsl)
    add_test(NAME skia_raster_test COMMAND skia_raster_test)

    add_executable(prism_skia_wayland_probe probes/skia_wayland_probe.cpp)
    target_link_libraries(prism_skia_wayland_probe PRIVATE
        prism_skia_raster prism_client_dsl prism_wayland_client)
endif()
