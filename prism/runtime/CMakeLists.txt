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

add_library(prism_popup_core STATIC popup_placement.cpp popup_session.cpp)
target_link_libraries(prism_popup_core PUBLIC prism_contracts)
target_compile_features(prism_popup_core PUBLIC cxx_std_20)

add_library(prism_owner_task_core STATIC task_session.cpp)
target_include_directories(prism_owner_task_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_owner_task_core PUBLIC cxx_std_20)

add_library(prism_owner_feedback_core STATIC owner_feedback_session.cpp)
target_link_libraries(prism_owner_feedback_core PUBLIC prism_contracts)

add_library(prism_client_scene STATIC
    owner_file_panel.cpp
    owner_task_panel.cpp
    owner_feedback_panel.cpp
    owner_task_text.cpp
    scene_text_metrics.cpp
    control_value.cpp
    control_value_delivery.cpp
    scene_choice.cpp
    scene_control.cpp
    scene_menu.cpp
    scene_owner_modal.cpp
    scene_owner_modal_input.cpp
    scene_popup.cpp
    scene_popup_surface.cpp
    popup_surface_prepare.cpp
    popup_surface_local.cpp
    popup_surface_layout_cache.cpp
    popup_surface_effects.cpp
    surface_effect_geometry.cpp
    scene_popup_adoption.cpp
    scene_popup_scroll.cpp
    scene_popup_input.cpp
    scene_scroll.cpp
    scene_scroll_input.cpp
    scene_slider.cpp
    scene_slider_stream.cpp
    scene_slider_input.cpp
    scene_text.cpp
    scene_text_layout.cpp
    scene.cpp
    scene_contour.cpp
    scene_contour_recipe.cpp
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
target_link_libraries(prism_client_scene PUBLIC prism_owner_feedback_contract prism_owner_task_contract prism_popup_core prism_popup_positioner_contract prism_text_buffer prism_animation_core prism_display_list_contract prism_surface_effect_contract prism_dsl_schema prism_theme_contract prism_task_scheduler Threads::Threads)
target_include_directories(prism_client_scene PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_client_scene PUBLIC cxx_std_20)

# The frontend is deliberately a separate target: the retained Scene and
# renderer never need to include AST, parser, or legacy prism_core headers.
add_library(prism_client_dsl STATIC
    dsl_frontend.cpp
    dsl_contour.cpp
    dsl_contour_recipe.cpp
    prepared_component.cpp)
target_link_libraries(prism_client_dsl PUBLIC prism_dsl_schema prism_dsl_syntax prism_theme_contract prism_contour_contract)
target_include_directories(prism_client_dsl PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(prism_task_scheduler STATIC task_scheduler.cpp)
target_link_libraries(prism_task_scheduler PUBLIC prism_task_budget)
target_include_directories(prism_task_scheduler PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(prism_file_task_model STATIC file_task_model.cpp file_task_model_worker.cpp)
target_link_libraries(prism_file_task_model PUBLIC prism_task_scheduler prism_owner_task_contract)
target_include_directories(prism_file_task_model PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_file_task_model PUBLIC cxx_std_20)

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
        client_application_control.cpp
        client_application_task.cpp
        client_application_task_refresh.cpp
        client_application_confirmation.cpp
        client_application_file_panel.cpp
        client_application_feedback.cpp
        client_application_feedback_input.cpp
        client_application_scene.cpp
        client_application_state.cpp
        client_application_resources.cpp
        client_application_render.cpp
        client_application_popup.cpp
        client_application_install.cpp
        client_application_install_commit.cpp
        client_application_upload.cpp
        client_application_work_turn.cpp
        client_render_owner.cpp
        client_render_owner_commands.cpp
        client_render_owner_submit.cpp
        client_render_owner_popup.cpp
        client_render_owner_popup_submit.cpp
        render_worker_lifecycle.cpp)
    target_include_directories(prism_client_app PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_link_libraries(prism_client_app PUBLIC prism_owner_feedback_core prism_client_scene prism_client_dsl prism_owner_task_core prism_png_codec prism_text_shaper prism_skia_gles prism_wayland_egl)
endif()

# Pure Interface/Slot compilation and immutable critical composition.
add_library(prism_load_plan STATIC load_plan.cpp load_layout.cpp load_composition.cpp)
target_link_libraries(prism_load_plan PUBLIC prism_client_dsl)
target_include_directories(prism_load_plan PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_load_plan PUBLIC cxx_std_20)
