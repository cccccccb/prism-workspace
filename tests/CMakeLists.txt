add_executable(ipc_benchmark
    ipc_benchmark.cpp
)
target_link_libraries(ipc_benchmark PRIVATE prism_core)

add_executable(tiling_decoration_test
    tiling_decoration_test.cpp
)
target_link_libraries(tiling_decoration_test PRIVATE prism_core)
