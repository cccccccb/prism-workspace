add_library(prism_text_buffer STATIC text_buffer.cpp)
set_target_properties(prism_text_buffer PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(prism_text_buffer PUBLIC prism_contracts)

add_library(prism_task_budget STATIC session_task_budget.cpp session_task_budget_leases.cpp)
target_link_libraries(prism_task_budget PUBLIC Threads::Threads)
target_include_directories(prism_task_budget PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_task_budget PUBLIC cxx_std_20)

add_library(prism_dsl_schema STATIC dsl_schema.cpp)
target_link_libraries(prism_dsl_schema PUBLIC prism_contracts)
target_include_directories(prism_dsl_schema PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_dsl_schema PUBLIC cxx_std_20)

add_library(prism_client_scene STATIC
    scene_text.cpp
    scene_text_layout.cpp
    scene.cpp
    scene_input.cpp
    scene_input_snapshot.cpp
    scene_input_touch.cpp
    scene_input_events.cpp
    scene_gesture.cpp
    scene_state.cpp
    scene_animation.cpp
    scene_nodes.cpp
    scene_theme.cpp
    scene_construction.cpp
    scene_transaction.cpp
    scene_properties.cpp
    scene_regions.cpp
    scene_snapshot.cpp
    layout_engine.cpp
    render_tree.cpp
    display_list_builder.cpp
    image_resources.cpp
    ui_load.cpp
    theme_tokens.cpp
    buffer_damage.cpp)
target_link_libraries(prism_client_scene PUBLIC prism_text_buffer prism_animation_core prism_display_list_contract prism_dsl_schema prism_theme_contract prism_task_scheduler Threads::Threads)
target_include_directories(prism_client_scene PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_client_scene PUBLIC cxx_std_20)

# The frontend is deliberately a separate target: the retained Scene and
# renderer never need to include AST, parser, or legacy prism_core headers.
add_library(prism_client_dsl STATIC
    dsl_frontend.cpp
    prepared_component.cpp)
target_link_libraries(prism_client_dsl PUBLIC prism_dsl_schema prism_dsl_syntax prism_theme_contract)
target_include_directories(prism_client_dsl PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(prism_task_scheduler STATIC task_scheduler.cpp)
target_link_libraries(prism_task_scheduler PUBLIC prism_task_budget)
target_include_directories(prism_task_scheduler PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(prism_ui_loader STATIC load_session.cpp master_load_session.cpp master_load_tasks.cpp)
target_link_libraries(prism_ui_loader PUBLIC prism_client_dsl prism_load_plan prism_task_scheduler)
target_include_directories(prism_ui_loader PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

if(PRISM_SKIA_ROOT)
    pkg_check_modules(PRISM_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(PRISM_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(PRISM_PNG REQUIRED IMPORTED_TARGET libpng)
    add_library(prism_text_shaper STATIC text_shaper.cpp)
    target_include_directories(prism_text_shaper PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_features(prism_text_shaper PUBLIC cxx_std_20)
    target_link_libraries(prism_text_shaper PUBLIC
        PkgConfig::PRISM_HARFBUZZ PkgConfig::PRISM_FREETYPE)

    add_library(prism_png_codec STATIC png_codec.cpp)
    target_include_directories(prism_png_codec PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_features(prism_png_codec PUBLIC cxx_std_20)
    target_link_libraries(prism_png_codec PUBLIC PkgConfig::PRISM_PNG)
endif()

if(PRISM_ENABLE_GLES AND PRISM_SKIA_ROOT)
    add_library(prism_client_app STATIC
        client_application.cpp
        client_application_animation.cpp
        client_application_commands.cpp
        client_application_frame.cpp
        client_application_gesture.cpp
        client_application_scene.cpp
        client_application_state.cpp
        client_application_resources.cpp
        client_application_render.cpp
        client_application_install.cpp
        client_application_install_commit.cpp
        client_application_upload.cpp
        client_application_work_turn.cpp
        client_render_owner.cpp
        client_render_owner_commands.cpp
        client_render_owner_submit.cpp
        render_worker_lifecycle.cpp)
    target_include_directories(prism_client_app PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_link_libraries(prism_client_app PUBLIC prism_client_scene prism_client_dsl prism_png_codec prism_text_shaper prism_skia_gles prism_wayland_egl)
endif()

# Pure Interface/Slot compilation and immutable critical composition.
add_library(prism_load_plan STATIC load_plan.cpp load_layout.cpp load_composition.cpp)
target_link_libraries(prism_load_plan PUBLIC prism_client_dsl)
target_include_directories(prism_load_plan PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_load_plan PUBLIC cxx_std_20)
