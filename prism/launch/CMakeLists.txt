find_package(nlohmann_json 3.11 REQUIRED)
# Launch foundations are independent of compositor, renderer and DSL parser.
add_library(prism_control STATIC control_protocol.cpp shell_permit.cpp stream.cpp)
target_link_libraries(prism_control PUBLIC prism_contracts prism_theme_contract)
target_compile_features(prism_control PUBLIC cxx_std_20)
add_library(prism_launch STATIC package.cpp protocol.cpp instance_state.cpp module.cpp worker_protocol.cpp)
target_link_libraries(prism_launch PUBLIC prism_contracts prism_control PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_features(prism_launch PUBLIC cxx_std_20)

add_library(prism_launch_client STATIC launch_client.cpp)
target_link_libraries(prism_launch_client PUBLIC prism_launch)
