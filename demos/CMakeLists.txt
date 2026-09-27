# The business module depends only on the pure C contract, never the renderer.
add_library(prism_music_module MODULE demo_player/module.cpp demo_player/player.cpp demo_player/catalogue.cpp)
target_link_libraries(prism_music_module PRIVATE prism_contracts nlohmann_json::nlohmann_json)
set_target_properties(prism_music_module PROPERTIES PREFIX "" OUTPUT_NAME player
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/prism/apps/demo_player")
set_target_properties(prism-app-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(PLAYER_PACKAGE "${CMAKE_BINARY_DIR}/share/prism/apps/demo_player")
file(MAKE_DIRECTORY "${PLAYER_PACKAGE}/assets")
foreach(resource manifest.json master.prism preview.prism layout.prism ui/header.prism ui/artwork.prism ui/transport.prism ui/library.prism assets/catalog.json assets/README.md)
    configure_file("demo_player/${resource}" "${PLAYER_PACKAGE}/${resource}" COPYONLY)
endforeach()
install(TARGETS prism_music_module LIBRARY DESTINATION share/prism/apps/demo_player)
install(FILES demo_player/manifest.json demo_player/master.prism demo_player/preview.prism demo_player/layout.prism
    DESTINATION share/prism/apps/demo_player)
install(DIRECTORY demo_player/ui DESTINATION share/prism/apps/demo_player)
install(DIRECTORY demo_player/assets DESTINATION share/prism/apps/demo_player)

add_subdirectory(demo_settings settings-module)
