# Zelda Fangame — SDL3 client

SDL3 + OpenGL 3.3 core, C++20, MSVC (VS 2026). SDL 3.4.14 is fetched and
statically linked by CMake — no submodules, no DLLs.

## Build

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build            # first run downloads + configures SDL
& $cmake --build build --config Debug
.\build\Debug\zelda.exe
```

(Or just open the folder in Visual Studio — it picks up CMakeLists.txt.)

## Layout

- `src/main.cpp` — entry, window/context, fixed-timestep loop (60 Hz sim, vsync render)
- `src/gl_loader.h/.cpp` — minimal GL 3.3 core loader via `SDL_GL_GetProcAddress`

## Controls (animation viewer)

- **left/right** — switch animation clip (name shown in the title bar)
- **mouse drag** — orbit camera, **wheel** — zoom
- **esc** — quit

## Roadmap

1. ~~Window + GL context + game loop~~
2. ~~glTF export of `../link/Link3.blend` (mesh, skeleton, all actions)~~
   — re-export with `../link/anim_scripts/export_gltf.py` after baking loops
3. ~~cgltf loader + GPU linear-blend skinning (GLSL) + toon shading~~
4. Animation state machine (idle/run/roll/slash chains mirror the Blender actions)
5. Character controller + camera
