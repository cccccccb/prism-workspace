find_package(nlohmann_json 3.11 REQUIRED)
# Launch foundations are independent of compositor, renderer and DSL parser.
add_library(prism_launch STATIC package.cpp protocol.cpp instance_state.cpp module.cpp shell_permit.cpp)
target_link_libraries(prism_launch PUBLIC prism_contracts PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_features(prism_launch PUBLIC cxx_std_20)
