cmake_minimum_required(VERSION 3.19)
project(soundstep LANGUAGES C CXX)

set(SOUNDSTEP_EXTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../external" CACHE PATH
    "Soundstep external dependency directory")
set(SOUNDSTEP_PLATFORM_DIR "${CMAKE_CURRENT_LIST_DIR}/../platform" CACHE PATH
    "Soundstep platform support directory")

if(NOT ANDROID)
    find_package(OpenGL REQUIRED)
endif()

add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/cereal" "${CMAKE_CURRENT_BINARY_DIR}/external/cereal")
if(NOT ANDROID)
    add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/glfw" "${CMAKE_CURRENT_BINARY_DIR}/external/glfw")
endif()
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/mbedtls" "${CMAKE_CURRENT_BINARY_DIR}/external/mbedtls")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/httplib" "${CMAKE_CURRENT_BINARY_DIR}/external/httplib")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/imgui" "${CMAKE_CURRENT_BINARY_DIR}/external/imgui")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/imanim" "${CMAKE_CURRENT_BINARY_DIR}/external/imanim")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/miniaudio" "${CMAKE_CURRENT_BINARY_DIR}/external/miniaudio")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/sqlite" "${CMAKE_CURRENT_BINARY_DIR}/external/sqlite")
add_subdirectory("${SOUNDSTEP_EXTERNAL_DIR}/stb" "${CMAKE_CURRENT_BINARY_DIR}/external/stb")

include("${SOUNDSTEP_EXTERNAL_DIR}/cmrc/CMakeRC.cmake")

set(SOUNDSTEP_RESOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
cmrc_add_resource_library(soundstep_resource
    ALIAS soundstep::resource
    NAMESPACE soundstep_resource
    WHENCE "${SOUNDSTEP_RESOURCE_DIR}"
    "${SOUNDSTEP_RESOURCE_DIR}/font/astral delight.ttf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/new rodin.otf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/aktiv grotesk.ttf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/pp fraktion.otf"
    "${SOUNDSTEP_RESOURCE_DIR}/font/fluent icons.ttf"
    "${SOUNDSTEP_RESOURCE_DIR}/platform/common/soundstep.png")

file(GLOB_RECURSE soundstep_sources CONFIGURE_DEPENDS "*.cpp")
if(ANDROID)
    set(_native_app_glue_dir "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(soundstep_native_app_glue STATIC
        "${_native_app_glue_dir}/android_native_app_glue.c")
    target_include_directories(soundstep_native_app_glue PUBLIC "${_native_app_glue_dir}")
    add_library(soundstep SHARED ${soundstep_sources})
elseif(WIN32)
    add_executable(soundstep WIN32 ${soundstep_sources})
else()
    add_executable(soundstep ${soundstep_sources})
endif()

set_target_properties(soundstep PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
if(NOT ANDROID)
    set_target_properties(soundstep PROPERTIES OUTPUT_NAME "SoundStep")
endif()
if(SOUNDSTEP_OUTPUT_DIR)
    set_target_properties(soundstep PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SOUNDSTEP_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${SOUNDSTEP_OUTPUT_DIR}")
endif()
target_include_directories(soundstep PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(soundstep PRIVATE
    soundstep_cereal
    soundstep_httplib
    soundstep_imanim
    soundstep_imgui
    soundstep_miniaudio
    soundstep_sqlite
    soundstep_stb
    soundstep::resource)

if(ANDROID)
    target_compile_options(soundstep PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(soundstep PRIVATE
        soundstep_native_app_glue
        android
        log
        EGL
        GLESv3)
else()
    target_link_libraries(soundstep PRIVATE soundstep_glfw OpenGL::GL)
endif()

if(WIN32)
    target_sources(soundstep PRIVATE
        "${SOUNDSTEP_PLATFORM_DIR}/win32/soundstep.rc")
    target_compile_definitions(soundstep PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
    target_link_libraries(soundstep PRIVATE ws2_32 iphlpapi runtimeobject shell32)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    include(GNUInstallDirs)
    install(TARGETS soundstep RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${SOUNDSTEP_PLATFORM_DIR}/linux/soundstep.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(_icon_size 16 24 32 48 64 128 256 512)
        install(FILES
            "${SOUNDSTEP_PLATFORM_DIR}/linux/icons/hicolor/${_icon_size}x${_icon_size}/apps/soundstep.png"
            DESTINATION
            "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_icon_size}x${_icon_size}/apps")
    endforeach()
endif()
