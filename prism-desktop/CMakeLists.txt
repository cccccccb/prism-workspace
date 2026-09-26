prism_app_package(prism_desktop_module prism_desktop src/module.cpp ui/desktop.prism desktop)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/prism/apps/prism_desktop/assets/wallpapers")
configure_file("${CMAKE_SOURCE_DIR}/resources/wallpapers/anime_girl.png"
    "${CMAKE_BINARY_DIR}/share/prism/apps/prism_desktop/assets/wallpapers/anime_girl.png" COPYONLY)
