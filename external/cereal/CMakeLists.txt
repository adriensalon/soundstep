add_library(soundstep_cereal INTERFACE)

target_compile_features(soundstep_cereal INTERFACE cxx_std_11)
target_include_directories(soundstep_cereal SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(soundstep_cereal INTERFACE CEREAL_THREAD_SAFE=1)

find_package(Threads REQUIRED)
target_link_libraries(soundstep_cereal INTERFACE Threads::Threads)
