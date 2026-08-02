cmake_minimum_required(VERSION 3.19)
project(soundstep_superbuild LANGUAGES NONE)

set(SOUNDSTEP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/source" CACHE PATH
    "Soundstep leaf CMake project")
set(SOUNDSTEP_PLATFORM_DIR "${CMAKE_CURRENT_LIST_DIR}/platform" CACHE PATH
    "Soundstep platform support directory")
set(SOUNDSTEP_NESTED_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/soundstep" CACHE PATH
    "Soundstep nested build directory")

function(_find_android)
    set(_sdk_candidates)
    foreach(_sdk_environment ANDROID_SDK_ROOT ANDROID_HOME)
        if(DEFINED ENV{${_sdk_environment}})
            list(APPEND _sdk_candidates "$ENV{${_sdk_environment}}")
        endif()
    endforeach()
    if(CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
        list(APPEND _sdk_candidates "$ENV{LOCALAPPDATA}/Android/Sdk")
    endif()

    set(_ndk_candidates)
    foreach(_ndk_environment ANDROID_NDK_ROOT ANDROID_NDK_HOME ANDROID_NDK_DIR)
        if(DEFINED ENV{${_ndk_environment}})
            list(APPEND _ndk_candidates "$ENV{${_ndk_environment}}")
        endif()
    endforeach()
    foreach(_sdk_candidate IN LISTS _sdk_candidates)
        file(GLOB _sdk_ndks LIST_DIRECTORIES true "${_sdk_candidate}/ndk/*")
        if(_sdk_ndks)
            list(SORT _sdk_ndks COMPARE NATURAL ORDER DESCENDING)
            list(APPEND _ndk_candidates ${_sdk_ndks})
        endif()
    endforeach()

    foreach(_candidate IN LISTS _ndk_candidates)
        if(EXISTS "${_candidate}/build/cmake/android.toolchain.cmake")
            set(_ndk_dir "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _ndk_dir)
        find_program(_ndkbuild_executable NAMES ndk-build ndk-build.cmd)
        if(_ndkbuild_executable)
            get_filename_component(_candidate "${_ndkbuild_executable}" DIRECTORY)
            if(EXISTS "${_candidate}/build/cmake/android.toolchain.cmake")
                set(_ndk_dir "${_candidate}")
            endif()
        endif()
    endif()

    if(_ndk_dir)
        set(soundstep_android_ndk "${_ndk_dir}" CACHE PATH "Android NDK directory")
        set(soundstep_android_toolchain
            "${_ndk_dir}/build/cmake/android.toolchain.cmake"
            CACHE FILEPATH "Android CMake toolchain file")
        foreach(_sdk_candidate IN LISTS _sdk_candidates)
            if(EXISTS "${_sdk_candidate}/platforms" OR EXISTS "${_sdk_candidate}/cmdline-tools")
                set(soundstep_android_sdk "${_sdk_candidate}" CACHE PATH "Android SDK directory")
                break()
            endif()
        endforeach()
        message(STATUS "Found Android NDK at ${_ndk_dir}")
    endif()
endfunction()

function(_find_java)
    if(CMAKE_HOST_WIN32)
        set(_java_executable_name java.exe)
    else()
        set(_java_executable_name java)
    endif()
    if(DEFINED ENV{JAVA_HOME}
        AND EXISTS "$ENV{JAVA_HOME}/bin/${_java_executable_name}")
        set(_java_home "$ENV{JAVA_HOME}")
    elseif(CMAKE_HOST_WIN32)
        file(GLOB _java_candidates LIST_DIRECTORIES true
            "C:/Program Files/Microsoft/jdk-17*"
            "C:/Program Files/Eclipse Adoptium/jdk-17*")
        if(_java_candidates)
            list(SORT _java_candidates COMPARE NATURAL ORDER DESCENDING)
            list(GET _java_candidates 0 _java_home)
        endif()
    endif()
    if(_java_home)
        set(soundstep_java_home "${_java_home}" CACHE PATH "JDK used by Android Gradle")
        message(STATUS "Found Java at ${_java_home}")
    endif()
endfunction()

function(_find_ninja)
    find_program(_ninja_executable NAMES ninja ninja.exe)
    if(NOT _ninja_executable AND CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
        file(GLOB _ninja_candidates
            "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/Ninja-build.Ninja_*/ninja.exe")
        if(_ninja_candidates)
            list(GET _ninja_candidates 0 _ninja_executable)
        endif()
    endif()
    if(_ninja_executable)
        set(soundstep_ninja "${_ninja_executable}" CACHE FILEPATH
            "Ninja executable used by cross-platform builds")
        message(STATUS "Found Ninja at ${_ninja_executable}")
    endif()
endfunction()

function(_soundstep_add_native_target)
    if(CMAKE_HOST_WIN32)
        set(_platform win32)
    elseif(CMAKE_HOST_APPLE)
        set(_platform macos)
    elseif(CMAKE_HOST_UNIX)
        set(_platform linux)
    else()
        message(FATAL_ERROR "Unsupported native host platform")
    endif()

    set(_binary_dir "${SOUNDSTEP_NESTED_BINARY_DIR}/${_platform}")
    set(_generator_args -G "${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND _generator_args -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND _generator_args -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_MAKE_PROGRAM)
        list(APPEND _generator_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    if(NOT CMAKE_CONFIGURATION_TYPES)
        list(APPEND _generator_args
            "-DCMAKE_BUILD_TYPE=$<IF:$<CONFIG:Release>,Release,Debug>")
    endif()

    add_custom_target(soundstep_${_platform}
        COMMAND "${CMAKE_COMMAND}"
            -Wno-deprecated
            -S "${SOUNDSTEP_SOURCE_DIR}"
            -B "${_binary_dir}"
            ${_generator_args}
        COMMAND "${CMAKE_COMMAND}"
            --build "${_binary_dir}"
            --config "$<IF:$<CONFIG:Release>,Release,Debug>"
            --target soundstep
        COMMENT "Building Soundstep for ${_platform}"
        VERBATIM
        USES_TERMINAL)
    set(soundstep_native_target "soundstep_${_platform}" PARENT_SCOPE)
endfunction()

function(_soundstep_add_android_target)
    if(NOT soundstep_android_toolchain OR NOT soundstep_android_sdk)
        message(STATUS "Skipping Soundstep Android target: Android SDK/NDK not found")
        return()
    endif()
    if(NOT soundstep_java_home)
        message(STATUS "Skipping Soundstep Android target: JDK 17 not found")
        return()
    endif()
    if(NOT soundstep_ninja)
        message(STATUS "Skipping Soundstep Android target: Ninja not found")
        return()
    endif()

    set(_binary_dir "${SOUNDSTEP_NESTED_BINARY_DIR}/android/arm64-v8a")
    set(_package_dir "${SOUNDSTEP_NESTED_BINARY_DIR}/android/package")
    set(_jni_dir "${_package_dir}/jniLibs/arm64-v8a")
    set(_gradle_build_dir "${SOUNDSTEP_NESTED_BINARY_DIR}/android/gradle")
    if(CMAKE_HOST_WIN32)
        set(_gradlew "${SOUNDSTEP_PLATFORM_DIR}/android/gradlew.bat")
    else()
        set(_gradlew "${SOUNDSTEP_PLATFORM_DIR}/android/gradlew")
    endif()

    add_custom_target(soundstep_android
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_jni_dir}"
        COMMAND "${CMAKE_COMMAND}"
            -Wno-deprecated
            -S "${SOUNDSTEP_SOURCE_DIR}"
            -B "${_binary_dir}"
            -G Ninja
            "-DCMAKE_MAKE_PROGRAM=${soundstep_ninja}"
            "-DCMAKE_TOOLCHAIN_FILE=${soundstep_android_toolchain}"
            "-DANDROID_ABI=arm64-v8a"
            "-DANDROID_PLATFORM=android-24"
            "-DCMAKE_BUILD_TYPE=$<IF:$<CONFIG:Release>,Release,Debug>"
            "-DSOUNDSTEP_OUTPUT_DIR=${_jni_dir}"
        COMMAND "${CMAKE_COMMAND}"
            --build "${_binary_dir}"
            --target soundstep
        COMMAND "${CMAKE_COMMAND}" -E env
            "DEBUG="
            "JAVA_HOME=${soundstep_java_home}"
            "ANDROID_HOME=${soundstep_android_sdk}"
            "ANDROID_SDK_ROOT=${soundstep_android_sdk}"
            "${_gradlew}"
            --project-dir "${SOUNDSTEP_PLATFORM_DIR}/android"
            --project-cache-dir "${_gradle_build_dir}/cache"
            "$<IF:$<CONFIG:Release>,stageReleaseApk,stageDebugApk>"
            "-PtargetName=soundstep"
            "-PnativeLibDir=${_package_dir}/jniLibs"
            "-PandroidBuildDir=${_gradle_build_dir}"
            "-PpackageDir=${_package_dir}"
        COMMENT "Building and packaging Soundstep for Android (arm64-v8a)"
        VERBATIM
        USES_TERMINAL)
endfunction()

_find_android()
_find_java()
_find_ninja()
_soundstep_add_native_target()
_soundstep_add_android_target()
