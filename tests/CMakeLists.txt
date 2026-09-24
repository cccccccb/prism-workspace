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

enable_testing()
add_test(NAME tree_engine_test COMMAND tree_engine_test)
add_test(NAME tiling_decoration_test COMMAND tiling_decoration_test)
add_test(NAME ipc_benchmark COMMAND ipc_benchmark)
add_test(NAME dsl_widgets_test COMMAND dsl_widgets_test)
