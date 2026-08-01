
set(SOUNDSTEP_EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../external)

find_package(OpenGL REQUIRED)

add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/cereal" "${CMAKE_CURRENT_BINARY_DIR}/external/cereal")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/glfw" "${CMAKE_CURRENT_BINARY_DIR}/external/glfw")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/mbedtls" "${CMAKE_CURRENT_BINARY_DIR}/external/mbedtls")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/httplib" "${CMAKE_CURRENT_BINARY_DIR}/external/httplib")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/imgui" "${CMAKE_CURRENT_BINARY_DIR}/external/imgui")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/imanim" "${CMAKE_CURRENT_BINARY_DIR}/external/imanim")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/miniaudio" "${CMAKE_CURRENT_BINARY_DIR}/external/miniaudio")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/sqlite" "${CMAKE_CURRENT_BINARY_DIR}/external/sqlite")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/stb" "${CMAKE_CURRENT_BINARY_DIR}/external/stb")

include("${SOUNDSTEP_EXTERNAL_DIR}/cmrc/CMakeRC.cmake")

set(SOUNDSTEP_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
cmrc_add_resource_library(soundstep_resource
    ALIAS soundstep::resource
    NAMESPACE soundstep_resource
    WHENCE "${SOUNDSTEP_RESOURCE_DIR}"
    "${SOUNDSTEP_RESOURCE_DIR}/font/astral delight.ttf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/new rodin.otf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/aktiv grotesk.ttf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/pp fraktion.otf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/fluent icons.ttf")

file(GLOB_RECURSE soundstep_sources CONFIGURE_DEPENDS "*.cpp")
add_executable(soundstep ${soundstep_sources})
set_target_properties(soundstep PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
target_include_directories(soundstep PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(soundstep PRIVATE
    soundstep_cereal
    soundstep_glfw
    soundstep_httplib
    soundstep_imanim
    soundstep_imgui
    soundstep_miniaudio
    soundstep_sqlite
    soundstep_stb
    soundstep::resource
    OpenGL::GL)

if(WIN32)
    target_compile_definitions(soundstep PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
    target_link_libraries(soundstep PRIVATE ws2_32 iphlpapi runtimeobject)
endif()
