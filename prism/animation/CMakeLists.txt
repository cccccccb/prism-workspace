add_library(prism_animation_core STATIC timeline.cpp)
target_include_directories(prism_animation_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(prism_animation_core PUBLIC cxx_std_20)
