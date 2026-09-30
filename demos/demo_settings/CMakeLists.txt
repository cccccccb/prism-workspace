prism_app_package(prism_settings_module demo_settings module.cpp master.prism settings)
target_sources(prism_settings_module PRIVATE metrics.cpp)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/" DESTINATION "${CMAKE_BINARY_DIR}/share/prism/apps/demo_settings/assets")
