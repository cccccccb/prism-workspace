cmake_minimum_required(VERSION 3.22)
project(prism-dock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(prism-dock
    src/main.cpp
    src/dock_item.cpp
)

target_include_directories(prism-dock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/prism/include
)

target_link_libraries(prism-dock PRIVATE
    prism_sdk
    prism_core
)
