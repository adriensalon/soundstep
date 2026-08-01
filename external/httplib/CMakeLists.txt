add_library(soundstep_httplib INTERFACE)

target_compile_features(soundstep_httplib INTERFACE cxx_std_11)
target_include_directories(soundstep_httplib SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(soundstep_httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
target_link_libraries(soundstep_httplib INTERFACE soundstep_mbedtls)

find_package(Threads REQUIRED)
target_link_libraries(soundstep_httplib INTERFACE Threads::Threads)

if(WIN32)
    target_link_libraries(soundstep_httplib INTERFACE ws2_32 crypt32)
endif()
