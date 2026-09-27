# Release diagnostics must execute assertions used by the test suite.
add_compile_options(-UNDEBUG)

add_executable(ipc_benchmark
    ipc_benchmark.cpp
)
target_link_libraries(ipc_benchmark PRIVATE prism_core)

add_executable(tiling_decoration_test
    tiling_decoration_test.cpp
)
target_link_libraries(tiling_decoration_test PRIVATE prism_core prism_aot_tools)

add_executable(tree_engine_test
    tree_engine_test.cpp
)
target_link_libraries(tree_engine_test PRIVATE prism_core)

add_executable(layer_manager_test
    layer_manager_test.cpp
)
target_link_libraries(layer_manager_test PRIVATE prism_core)

add_executable(contracts_smoke contracts_smoke.cpp)
target_link_libraries(contracts_smoke PRIVATE prism_contracts)
add_executable(wayland_lifecycle_test wayland_lifecycle_test.cpp)
target_link_libraries(wayland_lifecycle_test PRIVATE prism_core prism_wayland_client)
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME wayland_lifecycle_test COMMAND wayland_lifecycle_test)
add_test(NAME tree_engine_test COMMAND tree_engine_test)
add_test(NAME tiling_decoration_test COMMAND tiling_decoration_test)
add_test(NAME ipc_benchmark COMMAND ipc_benchmark)
add_test(NAME layer_manager_test COMMAND layer_manager_test)

add_executable(client_scene_test client_scene_test.cpp)
target_link_libraries(client_scene_test PRIVATE prism_client_dsl prism_client_scene)
add_test(NAME client_scene_test COMMAND client_scene_test)
target_compile_options(client_scene_test PRIVATE -UNDEBUG)
add_executable(client_app_templates_test client_app_templates_test.cpp)
target_link_libraries(client_app_templates_test PRIVATE prism_client_dsl prism_client_scene prism_module_session prism_theme_compiler prism_load_plan)
target_compile_options(client_app_templates_test PRIVATE -UNDEBUG)
target_compile_definitions(client_app_templates_test PRIVATE PRISM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
if(TARGET prism_music_module AND TARGET prism_desktop_module)
    add_dependencies(client_app_templates_test prism_desktop_module prism_topbar_module
        prism_dock_module prism_music_module prism_settings_module)
    add_test(NAME client_app_templates_test COMMAND client_app_templates_test
        $<TARGET_FILE:prism_desktop_module> $<TARGET_FILE:prism_topbar_module>
        $<TARGET_FILE:prism_dock_module> $<TARGET_FILE:prism_music_module>
        $<TARGET_FILE:prism_settings_module> "${CMAKE_BINARY_DIR}/share/prism/apps/demo_player/assets")
else()
    add_test(NAME client_app_templates_test COMMAND client_app_templates_test)
endif()

if(TARGET prism_music_module)
    add_executable(demo_player_work_test demo_player_work_test.cpp)
    target_link_libraries(demo_player_work_test PRIVATE prism_module_session prism_task_scheduler
        nlohmann_json::nlohmann_json Threads::Threads)
    target_compile_options(demo_player_work_test PRIVATE -UNDEBUG)
    add_dependencies(demo_player_work_test prism_music_module)
    add_test(NAME demo_player_work_test COMMAND demo_player_work_test
        $<TARGET_FILE:prism_music_module>)
    set_tests_properties(demo_player_work_test PROPERTIES TIMEOUT 25)
endif()
add_executable(render_tree_test render_tree_test.cpp)
target_link_libraries(render_tree_test PRIVATE prism_client_scene)
add_test(NAME render_tree_test COMMAND render_tree_test)
add_executable(visual_scene_test visual_scene_test.cpp)
target_link_libraries(visual_scene_test PRIVATE prism_client_dsl prism_client_scene)
target_compile_options(visual_scene_test PRIVATE -UNDEBUG)
add_test(NAME visual_scene_test COMMAND visual_scene_test)
add_executable(buffer_damage_test buffer_damage_test.cpp)
target_link_libraries(buffer_damage_test PRIVATE prism_client_scene)
target_compile_options(buffer_damage_test PRIVATE -UNDEBUG)
add_test(NAME buffer_damage_test COMMAND buffer_damage_test)
add_executable(dsl_syntax_test dsl_syntax_test.cpp)
target_link_libraries(dsl_syntax_test PRIVATE prism_client_dsl)
add_test(NAME dsl_syntax_test COMMAND dsl_syntax_test)

add_executable(prepared_component_test prepared_component_test.cpp)
target_link_libraries(prepared_component_test PRIVATE prism_client_dsl Threads::Threads)
target_compile_options(prepared_component_test PRIVATE -UNDEBUG)
add_test(NAME prepared_component_test COMMAND prepared_component_test)
set_tests_properties(prepared_component_test PROPERTIES TIMEOUT 10)

add_executable(theme_compiler_test theme_compiler_test.cpp)
target_link_libraries(theme_compiler_test PRIVATE prism_theme_compiler)
target_compile_definitions(theme_compiler_test PRIVATE PRISM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(theme_compiler_test PRIVATE -UNDEBUG)
add_test(NAME theme_compiler_test COMMAND theme_compiler_test)

# Manual protocol probes live beside tests and are never installed.
add_executable(wayland_lifecycle_probe probes/wayland_lifecycle_probe.cpp)
target_link_libraries(wayland_lifecycle_probe PRIVATE prism_wayland_client)

if(TARGET prism_skia_raster)
    add_executable(skia_raster_test skia_raster_test.cpp)
    target_link_libraries(skia_raster_test PRIVATE prism_skia_raster prism_client_dsl prism_client_scene)
    target_compile_definitions(skia_raster_test PRIVATE
        PRISM_TEST_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/checker.png")
    add_test(NAME skia_raster_test COMMAND skia_raster_test)

    add_executable(prism_skia_wayland_probe probes/skia_wayland_probe.cpp)
    target_link_libraries(prism_skia_wayland_probe PRIVATE
        prism_skia_raster prism_client_dsl prism_client_scene prism_wayland_client)
endif()

if(TARGET prism_client_app)
    add_executable(prism_skia_gles_wayland_probe probes/skia_gles_wayland_probe.cpp)
    target_link_libraries(prism_skia_gles_wayland_probe PRIVATE
        prism_client_app)
    add_executable(prepared_ui_probe probes/prepared_ui_probe.cpp)
    target_link_libraries(prepared_ui_probe PRIVATE prism_client_app Threads::Threads)
endif()

if(TARGET prism_skia_gles)
    add_executable(skia_gles_parity_test skia_gles_parity_test.cpp)
    target_link_libraries(skia_gles_parity_test PRIVATE prism_skia_gles)
    add_test(NAME skia_gles_parity_test COMMAND skia_gles_parity_test)
    add_executable(skia_damage_test skia_damage_test.cpp)
    target_link_libraries(skia_damage_test PRIVATE prism_skia_gles prism_wayland_egl)
    target_compile_options(skia_damage_test PRIVATE -UNDEBUG)
    add_test(NAME skia_damage_test COMMAND skia_damage_test)
    set_tests_properties(skia_damage_test PROPERTIES TIMEOUT 30)
endif()

add_executable(image_resources_test image_resources_test.cpp)
target_link_libraries(image_resources_test PRIVATE prism_client_dsl prism_client_scene)
add_test(NAME image_resources_test COMMAND image_resources_test)
target_compile_options(image_resources_test PRIVATE -UNDEBUG)

# Isolated protocol and waiting gates; no fixtures enter production installs.
add_executable(wayland_submit_test wayland_submit_test.cpp)
target_link_libraries(wayland_submit_test PRIVATE prism_wayland_client ${WAYLAND_SERVER_LIBRARIES} Threads::Threads)
target_include_directories(wayland_submit_test PRIVATE ${WAYLAND_SERVER_INCLUDE_DIRS}
    "${CMAKE_BINARY_DIR}/prism" "${CMAKE_BINARY_DIR}/prism/platform/wayland_client")
target_compile_options(wayland_submit_test PRIVATE -UNDEBUG)
add_dependencies(wayland_submit_test prism_wayland_protocol_headers)
add_test(NAME wayland_submit_test COMMAND wayland_submit_test)
set_tests_properties(wayland_submit_test PROPERTIES TIMEOUT 25)

add_library(timer_module_fixture MODULE fixtures/timer_module.cpp)
target_link_libraries(timer_module_fixture PRIVATE prism_contracts)
add_executable(module_timer_wait_test module_timer_wait_test.cpp)
target_link_libraries(module_timer_wait_test PRIVATE prism_module_session)
target_compile_options(module_timer_wait_test PRIVATE -UNDEBUG)
add_dependencies(module_timer_wait_test timer_module_fixture)
add_test(NAME module_timer_wait_test COMMAND module_timer_wait_test $<TARGET_FILE:timer_module_fixture>)
add_executable(event_wait_test event_wait_test.cpp)
target_link_libraries(event_wait_test PRIVATE prism_contracts Threads::Threads)
target_compile_options(event_wait_test PRIVATE -UNDEBUG)
add_test(NAME event_wait_test COMMAND event_wait_test)
set_tests_properties(event_wait_test PROPERTIES TIMEOUT 5)

# Launch contracts and module fixtures stay exclusively in test targets.
add_executable(app_module_abi_smoke app_module_abi_smoke.c)
target_include_directories(app_module_abi_smoke PRIVATE "${CMAKE_SOURCE_DIR}/prism/include")
add_test(NAME app_module_abi_smoke COMMAND app_module_abi_smoke)
add_executable(launch_package_test launch_package_test.cpp)
target_link_libraries(launch_package_test PRIVATE prism_launch)
add_test(NAME launch_package_test COMMAND launch_package_test)
add_executable(launch_protocol_test launch_protocol_test.cpp)
target_link_libraries(launch_protocol_test PRIVATE prism_launch)
add_test(NAME launch_protocol_test COMMAND launch_protocol_test)
foreach(fixture IN ITEMS valid wrong_abi missing_destroy)
    add_library(app_module_fixture_${fixture} MODULE fixtures/app_module_v1.c)
    target_include_directories(app_module_fixture_${fixture} PRIVATE "${CMAKE_SOURCE_DIR}/prism/include")
endforeach()
target_compile_definitions(app_module_fixture_wrong_abi PRIVATE TEST_MODULE_ABI=2)
target_compile_definitions(app_module_fixture_missing_destroy PRIVATE TEST_MISSING_DESTROY=1)
add_executable(launch_module_test launch_module_test.cpp)
target_link_libraries(launch_module_test PRIVATE prism_launch)
add_dependencies(launch_module_test app_module_fixture_valid app_module_fixture_wrong_abi app_module_fixture_missing_destroy)
add_test(NAME launch_module_test COMMAND launch_module_test
    $<TARGET_FILE:app_module_fixture_valid>
    $<TARGET_FILE:app_module_fixture_wrong_abi>
    $<TARGET_FILE:app_module_fixture_missing_destroy>)
# Checks remain active in Release configurations too.
foreach(test IN ITEMS launch_package_test launch_protocol_test launch_module_test)
    target_compile_options(${test} PRIVATE -UNDEBUG)
endforeach()

add_executable(launch_shell_permit_test launch_shell_permit_test.cpp)
target_link_libraries(launch_shell_permit_test PRIVATE prism_launch)
target_compile_options(launch_shell_permit_test PRIVATE -UNDEBUG)
add_test(NAME launch_shell_permit_test COMMAND launch_shell_permit_test)

if(TARGET prism_music_module)
    add_executable(music_module_test music_module_test.cpp)
    target_link_libraries(music_module_test PRIVATE prism_module_session)
    target_compile_options(music_module_test PRIVATE -UNDEBUG)
    add_dependencies(music_module_test prism_music_module)
    add_test(NAME music_module_test COMMAND music_module_test $<TARGET_FILE:prism_music_module>)
endif()

add_executable(launch_stream_test launch_stream_test.cpp)
target_link_libraries(launch_stream_test PRIVATE prism_launch)
target_compile_options(launch_stream_test PRIVATE -UNDEBUG)
add_test(NAME launch_stream_test COMMAND launch_stream_test)
foreach(mode IN ITEMS hang crash child)
    add_library(host_lifecycle_fixture_${mode} MODULE fixtures/host_lifecycle_module.cpp)
    target_link_libraries(host_lifecycle_fixture_${mode} PRIVATE prism_contracts Threads::Threads)
endforeach()
target_compile_definitions(host_lifecycle_fixture_hang PRIVATE TEST_HANG_INIT=1)
target_compile_definitions(host_lifecycle_fixture_crash PRIVATE TEST_CRASH_INIT=1)
target_compile_definitions(host_lifecycle_fixture_child PRIVATE TEST_CHILD_LAUNCH=1)
add_executable(module_launch_test module_launch_test.cpp)
target_link_libraries(module_launch_test PRIVATE prism_module_session)
target_compile_options(module_launch_test PRIVATE -UNDEBUG)
add_dependencies(module_launch_test host_lifecycle_fixture_child)
add_test(NAME module_launch_test COMMAND module_launch_test $<TARGET_FILE:host_lifecycle_fixture_child>)

add_executable(session_control_test session_control_test.cpp)
target_link_libraries(session_control_test PRIVATE prism_launch)
target_compile_options(session_control_test PRIVATE -UNDEBUG)
add_test(NAME session_control_test COMMAND session_control_test)
if(TARGET prism_desktop_module AND TARGET prism_settings_module)
    add_executable(shell_modules_test shell_modules_test.cpp)
    target_link_libraries(shell_modules_test PRIVATE prism_module_session)
    target_compile_options(shell_modules_test PRIVATE -UNDEBUG)
    add_test(NAME shell_modules_test COMMAND shell_modules_test
        $<TARGET_FILE:prism_desktop_module> $<TARGET_FILE:prism_topbar_module>
        $<TARGET_FILE:prism_dock_module> $<TARGET_FILE:prism_settings_module>)
endif()
add_library(legacy_module_fixture MODULE fixtures/legacy_module.c)
target_link_libraries(legacy_module_fixture PRIVATE prism_contracts)
add_executable(legacy_module_test legacy_module_test.cpp)
target_link_libraries(legacy_module_test PRIVATE prism_launch)
target_compile_options(legacy_module_test PRIVATE -UNDEBUG)
add_test(NAME legacy_module_test COMMAND legacy_module_test $<TARGET_FILE:legacy_module_fixture>)

add_executable(native_bsp_test native_bsp_test.cpp)
target_link_libraries(native_bsp_test PRIVATE prism_core prism_wayland_client)
target_compile_options(native_bsp_test PRIVATE -UNDEBUG)
add_test(NAME native_bsp_test COMMAND native_bsp_test)
set_tests_properties(native_bsp_test PROPERTIES TIMEOUT 40)

add_executable(surface_effects_state_test surface_effects_state_test.cpp)
target_link_libraries(surface_effects_state_test PRIVATE prism_core)
add_test(NAME surface_effects_state_test COMMAND surface_effects_state_test)

add_executable(effect_dependencies_test effect_dependencies_test.cpp)
target_link_libraries(effect_dependencies_test PRIVATE prism_core)
target_compile_options(effect_dependencies_test PRIVATE -UNDEBUG)
add_test(NAME effect_dependencies_test COMMAND effect_dependencies_test)

add_executable(render_scheduling_probe probes/render_scheduling_probe.cpp)
target_link_libraries(render_scheduling_probe PRIVATE prism_core prism_wayland_client)
target_include_directories(render_scheduling_probe PRIVATE "${CMAKE_BINARY_DIR}/prism/platform/wayland_client")
target_compile_options(render_scheduling_probe PRIVATE -UNDEBUG)
add_test(NAME render_scheduling_probe COMMAND render_scheduling_probe)
set_tests_properties(render_scheduling_probe PROPERTIES TIMEOUT 90)

add_executable(theme_protocol_test theme_protocol_test.cpp)
target_link_libraries(theme_protocol_test PRIVATE prism_launch prism_theme_compiler)
target_compile_definitions(theme_protocol_test PRIVATE PRISM_SOURCE_THEMES="${CMAKE_SOURCE_DIR}/resources/themes")
target_compile_options(theme_protocol_test PRIVATE -UNDEBUG)
add_test(NAME theme_protocol_test COMMAND theme_protocol_test)

add_executable(performance_samples_test performance_samples_test.cpp)
target_include_directories(performance_samples_test PRIVATE "${CMAKE_SOURCE_DIR}/prism/include")
target_compile_options(performance_samples_test PRIVATE -UNDEBUG)
add_test(NAME performance_samples_test COMMAND performance_samples_test)

add_executable(metrics_test metrics_test.cpp "${CMAKE_SOURCE_DIR}/demos/demo_settings/metrics.cpp")
target_include_directories(metrics_test PRIVATE "${CMAKE_SOURCE_DIR}/demos/demo_settings")
target_compile_definitions(metrics_test PRIVATE PRISM_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME metrics_test COMMAND metrics_test)

add_executable(json_boundary_test json_boundary_test.cpp)
target_link_libraries(json_boundary_test PRIVATE prism_core prism_aot_tools)
target_compile_options(json_boundary_test PRIVATE -UNDEBUG)
add_test(NAME json_boundary_test COMMAND json_boundary_test)
set_tests_properties(json_boundary_test PROPERTIES TIMEOUT 10)

add_executable(ui_load_test ui_load_test.cpp)
target_link_libraries(ui_load_test PRIVATE prism_client_scene Threads::Threads)
target_compile_options(ui_load_test PRIVATE -UNDEBUG)
add_test(NAME ui_load_test COMMAND ui_load_test)
set_tests_properties(ui_load_test PROPERTIES TIMEOUT 10)

add_executable(ui_presentation_test ui_presentation_test.cpp)
target_link_libraries(ui_presentation_test PRIVATE prism_contracts)
target_compile_options(ui_presentation_test PRIVATE -UNDEBUG)
add_test(NAME ui_presentation_test COMMAND ui_presentation_test)

add_executable(load_session_test load_session_test.cpp)
target_link_libraries(load_session_test PRIVATE prism_ui_loader)
target_compile_options(load_session_test PRIVATE -UNDEBUG)
add_test(NAME load_session_test COMMAND load_session_test)
set_tests_properties(load_session_test PROPERTIES TIMEOUT 20)

if(TARGET prism_app_host_runtime)
    add_library(async_master_fixture MODULE fixtures/async_master_module.cpp)
    target_link_libraries(async_master_fixture PRIVATE prism_contracts)
    add_executable(async_master_probe probes/async_master_probe.cpp)
    target_link_libraries(async_master_probe PRIVATE prism_app_host_runtime Threads::Threads)
    target_compile_options(async_master_probe PRIVATE -UNDEBUG)
    add_dependencies(async_master_probe async_master_fixture)
endif()

if(TARGET prism_app_host_runtime)
    add_executable(demo_startup_probe probes/demo_startup_probe.cpp)
    target_link_libraries(demo_startup_probe PRIVATE prism_app_host_runtime prism_theme_compiler
        Threads::Threads nlohmann_json::nlohmann_json)
    target_compile_options(demo_startup_probe PRIVATE -UNDEBUG)
endif()

add_executable(task_scheduler_test task_scheduler_test.cpp)
target_link_libraries(task_scheduler_test PRIVATE prism_task_scheduler prism_client_dsl)
target_compile_options(task_scheduler_test PRIVATE -UNDEBUG)
add_test(NAME task_scheduler_test COMMAND task_scheduler_test)
set_tests_properties(task_scheduler_test PROPERTIES TIMEOUT 30)

add_executable(session_task_budget_test session_task_budget_test.cpp)
target_link_libraries(session_task_budget_test PRIVATE prism_task_budget)
target_compile_options(session_task_budget_test PRIVATE -UNDEBUG)
add_test(NAME session_task_budget_test COMMAND session_task_budget_test)
set_tests_properties(session_task_budget_test PROPERTIES TIMEOUT 25)

add_executable(load_plan_test load_plan_test.cpp)
target_link_libraries(load_plan_test PRIVATE prism_load_plan)
target_compile_options(load_plan_test PRIVATE -UNDEBUG)
add_test(NAME load_plan_test COMMAND load_plan_test)
set_tests_properties(load_plan_test PROPERTIES TIMEOUT 20)

add_executable(master_load_session_test master_load_session_test.cpp)
target_link_libraries(master_load_session_test PRIVATE prism_ui_loader)
target_compile_options(master_load_session_test PRIVATE -UNDEBUG)
add_test(NAME master_load_session_test COMMAND master_load_session_test)
set_tests_properties(master_load_session_test PROPERTIES TIMEOUT 25)

add_executable(image_scheduler_test image_scheduler_test.cpp)
target_link_libraries(image_scheduler_test PRIVATE prism_client_scene prism_task_scheduler)
target_compile_options(image_scheduler_test PRIVATE -UNDEBUG)
add_test(NAME image_scheduler_test COMMAND image_scheduler_test)
set_tests_properties(image_scheduler_test PROPERTIES TIMEOUT 25)

add_executable(scene_region_transaction_test scene_region_transaction_test.cpp)
target_link_libraries(scene_region_transaction_test PRIVATE prism_client_scene)
target_compile_options(scene_region_transaction_test PRIVATE -UNDEBUG)
add_test(NAME scene_region_transaction_test COMMAND scene_region_transaction_test)
set_tests_properties(scene_region_transaction_test PROPERTIES TIMEOUT 20)

# Async business fixtures exercise the CPU-only bridge; no production installs.
add_library(module_work_fixture MODULE fixtures/module_work.cpp)
target_link_libraries(module_work_fixture PRIVATE prism_contracts Threads::Threads)
target_compile_options(module_work_fixture PRIVATE -UNDEBUG)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(module_work_fixture PRIVATE -fno-gnu-unique)
endif()
add_library(module_work_load_fixture MODULE fixtures/module_work_load.c)
target_link_libraries(module_work_load_fixture PRIVATE prism_contracts)
add_library(module_work_partial_fixture MODULE fixtures/module_work_partial.c)
target_link_libraries(module_work_partial_fixture PRIVATE prism_contracts)
add_executable(module_work_test module_work_test.cpp)
target_link_libraries(module_work_test PRIVATE prism_module_session prism_task_scheduler ${CMAKE_DL_LIBS})
target_compile_options(module_work_test PRIVATE -UNDEBUG)
add_dependencies(module_work_test module_work_fixture module_work_load_fixture module_work_partial_fixture)
add_test(NAME module_work_test COMMAND module_work_test $<TARGET_FILE:module_work_fixture>
    $<TARGET_FILE:module_work_load_fixture> $<TARGET_FILE:module_work_partial_fixture>)
set_tests_properties(module_work_test PROPERTIES TIMEOUT 30)

if(TARGET prism_app_host_runtime)
    add_library(async_business_fixture MODULE fixtures/async_business_module.cpp)
    target_link_libraries(async_business_fixture PRIVATE prism_contracts)
    add_executable(async_business_probe probes/async_business_probe.cpp)
    target_link_libraries(async_business_probe PRIVATE prism_app_host_runtime Threads::Threads
        nlohmann_json::nlohmann_json)
    target_compile_options(async_business_probe PRIVATE -UNDEBUG)
    add_dependencies(async_business_probe async_business_fixture)
endif()
