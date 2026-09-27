add_library(prism_launcher_runtime STATIC
    service.cpp
    service_requests.cpp
    service_theme.cpp
    service_control.cpp
    service_workers.cpp
    service_loop.cpp)
target_link_libraries(prism_launcher_runtime PUBLIC prism_launch prism_theme_compiler)
add_executable(prism-launcher main.cpp)
target_link_libraries(prism-launcher PRIVATE prism_launcher_runtime)
add_executable(prism-invoker invoker.cpp)
target_link_libraries(prism-invoker PRIVATE prism_launch_client)
set_target_properties(prism-launcher prism-invoker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(prism-session-runtime session.cpp)
target_include_directories(prism-session-runtime PRIVATE "${PROJECT_SOURCE_DIR}/prism/include")
set_target_properties(prism-session-runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
