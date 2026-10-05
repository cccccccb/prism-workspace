prism_app_package(prism_settings_module demo_settings module.cpp master.prism settings)
target_sources(prism_settings_module PRIVATE metrics.cpp)
file(GLOB SETTINGS_PREVIEWS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.png")
foreach(preview IN LISTS SETTINGS_PREVIEWS)
    get_filename_component(preview_name "${preview}" NAME)
    configure_file("${preview}"
        "${CMAKE_BINARY_DIR}/share/prism/apps/demo_settings/assets/${preview_name}" COPYONLY)
endforeach()
