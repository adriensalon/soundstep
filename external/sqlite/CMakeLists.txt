add_library(soundstep_sqlite STATIC "sqlite3.c")

set_target_properties(soundstep_sqlite PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON)

target_include_directories(soundstep_sqlite PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(soundstep_sqlite PRIVATE
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_FTS5
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1)

find_package(Threads REQUIRED)
target_link_libraries(soundstep_sqlite PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(soundstep_sqlite PRIVATE /wd4100 /wd4127 /wd4242 /wd4244 /wd4701 /wd4706)
endif()
