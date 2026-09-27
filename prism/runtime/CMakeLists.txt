add_library(prism_client_scene STATIC
    scene.cpp
    scene_properties.cpp
    scene_regions.cpp
    scene_snapshot.cpp
    layout_engine.cpp
    render_tree.cpp
    display_list_builder.cpp
    image_resources.cpp
    dsl_schema.cpp
    theme_tokens.cpp
    buffer_damage.cpp)
target_link_libraries(prism_client_scene PUBLIC prism_contracts prism_theme_contract Threads::Threads)
target_include_directories(prism_client_scene PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_client_scene PUBLIC cxx_std_20)

# The frontend is deliberately a separate target: the retained Scene and
# renderer never need to include AST, parser, or legacy prism_core headers.
add_library(prism_client_dsl STATIC
    dsl_frontend.cpp)
target_link_libraries(prism_client_dsl PUBLIC prism_client_scene prism_dsl_syntax)
target_include_directories(prism_client_dsl PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")

if(PRISM_ENABLE_GLES AND PRISM_SKIA_ROOT)
    add_library(prism_client_app STATIC
        client_application.cpp
        client_application_scene.cpp
        client_application_render.cpp)
    target_include_directories(prism_client_app PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_link_libraries(prism_client_app PUBLIC prism_client_dsl prism_skia_gles prism_wayland_egl)
endif()
