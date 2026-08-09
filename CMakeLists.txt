cmake_minimum_required(VERSION 3.24)
project(zelda_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# SDL3, pinned release, built statically into the exe (no DLL juggling)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-3.4.14.zip
)
FetchContent_MakeAvailable(SDL3)

add_executable(zelda
    src/main.cpp
    src/gl_loader.cpp
    src/model.cpp
    src/player.cpp
)
target_include_directories(zelda PRIVATE third_party/cgltf third_party/stb)
target_compile_definitions(zelda PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(zelda PRIVATE SDL3::SDL3-static opengl32)

if(MSVC)
    target_compile_options(zelda PRIVATE /W4)
    # console subsystem in Debug for logs, windows subsystem otherwise
    set_target_properties(zelda PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
endif()
