# Syntax is shared by the client and theme compilers, with no Scene dependency.
add_library(prism_dsl_syntax STATIC
    ../runtime/dsl_syntax.cpp ../src/compiler/lexer.cpp)
target_link_libraries(prism_dsl_syntax PUBLIC prism_contracts)
target_include_directories(prism_dsl_syntax PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(prism_theme_compiler STATIC compiler.cpp)
target_link_libraries(prism_theme_compiler PUBLIC prism_theme_contract PRIVATE prism_dsl_syntax)
target_include_directories(prism_theme_compiler PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")

# New authored packages join the catalog without renderer or CMake branches.
file(GLOB PRISM_THEME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../resources/themes/*/theme.prism")
list(SORT PRISM_THEME_SOURCES)
foreach(PRISM_THEME_SOURCE IN LISTS PRISM_THEME_SOURCES)
    get_filename_component(PRISM_THEME_DIRECTORY "${PRISM_THEME_SOURCE}" DIRECTORY)
    get_filename_component(PRISM_THEME "${PRISM_THEME_DIRECTORY}" NAME)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/prism/themes/${PRISM_THEME}")
    configure_file("${PRISM_THEME_SOURCE}"
        "${CMAKE_BINARY_DIR}/share/prism/themes/${PRISM_THEME}/theme.prism" COPYONLY)
    install(FILES "${PRISM_THEME_SOURCE}"
        DESTINATION "share/prism/themes/${PRISM_THEME}")
endforeach()
