add_library(soundstep_imanim STATIC "im_anim.cpp")

set_target_properties(soundstep_imanim PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_include_directories(soundstep_imanim PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(soundstep_imanim PUBLIC soundstep_imgui)

if(MSVC)
    target_compile_definitions(soundstep_imanim PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
