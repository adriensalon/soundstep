set(glfw_main_sources
    "src/context.c"
    "src/init.c"
    "src/input.c"
    "src/monitor.c"
    "src/platform.c"
    "src/vulkan.c"
    "src/window.c"
    "src/egl_context.c"
    "src/osmesa_context.c"
    "src/null_init.c"
    "src/null_monitor.c"
    "src/null_window.c"
    "src/null_joystick.c")
if(APPLE)
    set(glfw_platform_sources
        "src/cocoa_time.c"
        "src/posix_module.c"
        "src/posix_thread.c")
elseif(WIN32)
    set(glfw_platform_sources
        "src/win32_time.c"
        "src/win32_module.c"
        "src/win32_thread.c"
        "src/win32_init.c"
        "src/win32_joystick.c"
        "src/win32_monitor.c"
        "src/win32_window.c"
        "src/wgl_context.c")    
else()
    set(glfw_platform_sources 
        "src/posix_module.c"
        "src/posix_poll.c"
        "src/posix_time.c"
        "src/posix_thread.c"
        "src/linux_joystick.c"
        "src/x11_init.c"
        "src/x11_monitor.c"
        "src/x11_window.c"
        "src/xkb_unicode.c"
        "src/glx_context.c")
endif()
set(glfw_sources 
    ${glfw_main_sources}
    ${glfw_platform_sources})

add_library(soundstep_glfw STATIC ${glfw_sources})
set_target_properties(soundstep_glfw PROPERTIES CXX_STANDARD 17)
target_include_directories(soundstep_glfw PUBLIC "include")
target_include_directories(soundstep_glfw PUBLIC "deps")

if(WIN32)
    target_compile_definitions(soundstep_glfw PRIVATE _GLFW_WIN32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(soundstep_glfw PRIVATE _GLFW_X11)
    target_link_libraries(soundstep_glfw PUBLIC X11 Xrandr Xi Xcursor Xinerama)
endif()

if(MSVC)
    target_compile_options(soundstep_glfw PRIVATE /wd4034)
    target_compile_options(soundstep_glfw PRIVATE /wd4996)
endif()

