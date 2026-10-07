add_library(prism_module_session STATIC module_session.cpp module_close.cpp module_task.cpp module_feedback.cpp module_work.cpp module_work_job.cpp
    layout_control_bridge.cpp module_control.cpp module_control_value.cpp)
target_link_libraries(prism_module_session PUBLIC prism_launch prism_task_scheduler prism_owner_task_contract prism_owner_feedback_contract)
if(TARGET prism_client_app)
    add_library(prism_app_host_runtime STATIC
        app_host.cpp
        app_host_helpers.cpp
        app_host_state.cpp
        app_host_startup.cpp
        app_host_business.cpp
        app_host_tasks.cpp
        app_host_feedback.cpp
        app_host_file_actions.cpp
        app_host_file_view.cpp
        app_host_file_work.cpp
        app_host_loading.cpp
        app_host_regions.cpp)
    target_link_libraries(prism_app_host_runtime PUBLIC prism_client_app prism_ui_loader prism_module_session prism_launch_client prism_theme_compiler prism_file_task_model)
    add_executable(prism-app-host main.cpp worker.cpp)
    target_link_libraries(prism-app-host PRIVATE prism_app_host_runtime)
    install(TARGETS prism-app-host DESTINATION bin)
endif()
