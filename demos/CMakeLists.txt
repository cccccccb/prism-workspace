add_executable(demo_player
    demo_player/main.cpp
)
target_link_libraries(demo_player PRIVATE prism_sdk)

add_executable(demo_settings
    demo_settings/main.cpp
)
target_link_libraries(demo_settings PRIVATE prism_sdk)
