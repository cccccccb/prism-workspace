add_executable(ipc_benchmark
    ipc_benchmark.cpp
)
target_link_libraries(ipc_benchmark PRIVATE prism_core)
