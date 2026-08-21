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
    src/wmap.cpp
    src/player.cpp
    src/dungeon.cpp
)
target_include_directories(zelda PRIVATE third_party/cgltf third_party/stb)
target_compile_definitions(zelda PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(zelda PRIVATE SDL3::SDL3-static opengl32)

if(MSVC)
    target_compile_options(zelda PRIVATE /W4)
    # console subsystem in Debug for logs, windows subsystem otherwise
    set_target_properties(zelda PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
endif()

# ---- zelda_packed: fully self-contained exe (all assets embedded) ----
set(EMBED_SRCS "")
foreach(pair
        "link.glb|link_glb"
        "loftwing.glb|loftwing_glb"
        "panflute_c.wav|panflute_wav"
        "song_success.wav|chime_wav"
        "bird_call1.wav|bird_call1_wav"
        "bird_call2.wav|bird_call2_wav"
        "bird_call3.wav|bird_call3_wav"
        "bird_call4.wav|bird_call4_wav"
        "flap.wav|flap_wav"
        "launch_flap.wav|launch_flap_wav"
        "wind_loop.wav|wind_loop_wav"
        "ocean_loop.wav|ocean_loop_wav"
        "harsh_wind.wav|harsh_wind_wav"
        "twirl1.wav|twirl1_wav"
        "twirl2.wav|twirl2_wav"
        "twirl3.wav|twirl3_wav"
        "hud_font.ttf|hud_font"
        "sheet_frame.png|sheet_frame_png"
        "island.glb|island_glb"
        "island_height.bin|island_height_bin"
        "props.glb|props_glb"
        "footstep1.wav|footstep1_wav"
        "footstep2.wav|footstep2_wav"
        "footstep3.wav|footstep3_wav"
        "footstep4.wav|footstep4_wav"
        "footstep5.wav|footstep5_wav"
        "footstep6.wav|footstep6_wav"
        "footstep7.wav|footstep7_wav"
        "footstep8.wav|footstep8_wav"
        "footstep9.wav|footstep9_wav"
        "footstep10.wav|footstep10_wav"
        "footstep11.wav|footstep11_wav"
        "footstep12.wav|footstep12_wav"
        "footstep13.wav|footstep13_wav")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 asset)
    list(GET pair 1 sym)
    set(embed_src ${CMAKE_CURRENT_BINARY_DIR}/embedded_${sym}.c)
    add_custom_command(
        OUTPUT ${embed_src}
        COMMAND ${CMAKE_COMMAND}
                -DIN=${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset}
                -DOUT=${embed_src}
                -DSYM=${sym}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
        COMMENT "Embedding assets/${asset}")
    list(APPEND EMBED_SRCS ${embed_src})
endforeach()
add_executable(zelda_packed
    src/main.cpp
    src/gl_loader.cpp
    src/model.cpp
    src/wmap.cpp
    src/player.cpp
    src/dungeon.cpp
    ${EMBED_SRCS}
)
target_include_directories(zelda_packed PRIVATE third_party/cgltf third_party/stb)
# ZELDA_RETAIL: no tune files, no debug/tuning keys, no screenshot CLI --
# everything runs off the baked constants, as retail intended
target_compile_definitions(zelda_packed PRIVATE _CRT_SECURE_NO_WARNINGS EMBED_LINK_GLB ZELDA_RETAIL)
target_link_libraries(zelda_packed PRIVATE SDL3::SDL3-static opengl32)
if(MSVC)
    target_compile_options(zelda_packed PRIVATE /W4)
    set_target_properties(zelda_packed PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
