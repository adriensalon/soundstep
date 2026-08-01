set(imgui_sources
	"imgui.cpp"
	"imgui_widgets.cpp"
	"imgui_tables.cpp"
	"imgui_draw.cpp"
	"imgui_demo.cpp"
	"misc/cpp/imgui_stdlib.cpp")
if(ANDROID)
	list(APPEND imgui_sources
		"backends/imgui_impl_android.cpp"
		"backends/imgui_impl_opengl3.cpp")		
elseif(EMSCRIPTEN)
	list(APPEND imgui_sources
		"backends/imgui_impl_opengl3.cpp")
elseif(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND imgui_sources
		"backends/imgui_impl_glfw.cpp"
		"backends/imgui_impl_opengl3.cpp")
elseif(PSP)
	list(APPEND imgui_sources
		"backends/imgui_impl_psp.cpp")
endif()	

add_library(soundstep_imgui STATIC EXCLUDE_FROM_ALL ${imgui_sources})
set_target_properties(soundstep_imgui PROPERTIES CXX_STANDARD 17)
target_include_directories(soundstep_imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(ANDROID)
	target_link_libraries(soundstep_imgui PUBLIC GLESv3)
endif()
if(PSP)
	target_include_directories(soundstep_imgui PUBLIC "include")
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(soundstep_imgui PRIVATE soundstep_glfw)
endif()
