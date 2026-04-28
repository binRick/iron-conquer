cmake_minimum_required(VERSION 3.16)
project(iron_conquer C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(EMSCRIPTEN)
    set(PLATFORM Web CACHE STRING "raylib platform" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

add_executable(iron_conquer
    src/main.c
    src/game.c
    src/tilemap.c
    src/debug_log.c
    src/pathfind.c
    src/units.c
    src/palette.c
    src/shp.c
    src/tmp.c
)
target_include_directories(iron_conquer PRIVATE src)
target_link_libraries(iron_conquer PRIVATE raylib)

if(MSVC)
    target_compile_options(iron_conquer PRIVATE /W4)
else()
    target_compile_options(iron_conquer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EMSCRIPTEN)
    set_target_properties(iron_conquer PROPERTIES SUFFIX ".html")
    target_link_options(iron_conquer PRIVATE
        -sUSE_GLFW=3
        -sASYNCIFY
        -sTOTAL_MEMORY=67108864
        -sFORCE_FILESYSTEM=1
    )
endif()
