add_library(prism_client_scene STATIC scene.cpp image_resources.cpp)
target_link_libraries(prism_client_scene PUBLIC prism_contracts Threads::Threads)
target_include_directories(prism_client_scene PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_client_scene PUBLIC cxx_std_20)

# The frontend is deliberately a separate target: the retained Scene and
# renderer never need to include AST, parser, or legacy prism_core headers.
add_library(prism_client_dsl STATIC
    dsl_frontend.cpp
    ../src/compiler/lexer.cpp
    ../src/compiler/parser.cpp
    ../src/core/logging.cpp)
target_link_libraries(prism_client_dsl PUBLIC prism_client_scene)
target_include_directories(prism_client_dsl PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
