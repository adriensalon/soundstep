add_library(soundstep_glm INTERFACE)

target_compile_features(soundstep_glm INTERFACE cxx_std_17)
target_include_directories(soundstep_glm SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
