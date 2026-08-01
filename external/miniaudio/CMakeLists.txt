add_library(soundstep_miniaudio STATIC "miniaudio.c")
target_include_directories(soundstep_miniaudio SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(soundstep_miniaudio PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
target_link_libraries(soundstep_miniaudio PUBLIC Threads::Threads)

if(UNIX)
    target_link_libraries(soundstep_miniaudio PUBLIC ${CMAKE_DL_LIBS} m)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        target_link_libraries(soundstep_miniaudio PUBLIC atomic)
    endif()
endif()

if(MSVC)
    target_compile_options(soundstep_miniaudio PRIVATE /wd4242 /wd4244 /wd4267)
endif()
