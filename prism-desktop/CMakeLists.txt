cmake_minimum_required(VERSION 3.22)
project(prism-desktop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(prism-desktop
    src/main.cpp
)

target_include_directories(prism-desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/prism/include
)

target_link_libraries(prism-desktop PRIVATE
    prism_client_app
)
