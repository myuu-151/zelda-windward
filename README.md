# Zelda Windward

A small Zelda fan game: toon Link, a rideable loftwing, a procedural sky
and a Wind Waker sea — SDL3 + OpenGL 3.3 core, C++20, no engine.

![Link on the test plane, Wind Waker sea behind](screenshot.png)

## What's in it

- **Skinned toon Link** (glTF via cgltf, CPU-sampled animation, 30+ clips:
  run/roll/slash combo/guard/crouch/crawl/climb/flute/ride...)
- **Rideable loftwing** — whistle it down with the Song of Skies (play the
  flute with `I`, notes on `1`–`8`), vault on, launch, fly
- **Momentum flight**: SHIFT is a power glide that trades altitude for
  speed — stoop with S to build it, rotate skyward with W to spend it;
  speed persists and drags off slowly
- **Barrel-roll twirl** (SPACE mid-flight): a tucked 360 with a burst of
  speed and wind rushing past, carving out into a left-right sway
- **Procedural sky** — gradient, toon sun, domain-warped FBM clouds on one
  fullscreen triangle; no dome, no textures
- **Wind Waker sea** — a recreation of
  [NekotoArts' Shadertoy 3tKBDz](https://www.shadertoy.com/view/3tKBDz)
  (CC0 Godot port additions included): the classic 84-circle foam pattern,
  rolling vertex waves, endless in every direction
- Z-targeting camera, positional flute audio with reverb, HUD staff
  notation for the songs

## Build

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build            # first run downloads + configures SDL 3.4.14
& $cmake --build build --config Debug
.\build\Debug\zelda.exe
```

SDL is fetched and statically linked by CMake — no submodules, no DLLs.
The `zelda_packed` target builds a single self-contained exe (assets
embedded, dev tooling compiled out).

## Controls

| input | action |
|---|---|
| WASD | move / steer in flight |
| space | roll · vault on the bird · launch · **twirl** (mid-flight) |
| shift | guard on foot · **boost/glide** in flight (+S = the stoop) |
| J | attack (slash combo) |
| I | flute (`1`–`8` notes, up/down for sharps/flats) |
| esc | quit |

Z-targeting engages while guarding (shift) with a target in view — the
camera locks on and tracks it, on foot or in the sky.

Dev build extras: F1 clip viewer, F2 screenshot, F4 dive-freeze, live
seat/shield/camera tuning keys.

## Layout

- `src/main.cpp` — the whole game: sim, flight model, camera, audio,
  shaders (sky/sea/rig) as inline GLSL
- `src/model.h/.cpp` — glTF loading, skinning, pose sampling/blending
- `src/player.h/.cpp` — on-foot state machine
- `src/gl_loader.h/.cpp` — minimal GL 3.3 core loader
- `assets/` — exported glbs + audio (see note below)

## Assets note

The Link and loftwing models are ripped from Nintendo games (Tri Force
Heroes / Skyward Sword) and remain **© Nintendo**. They're included so the
project runs, as part of a non-commercial fan work — they are not mine to
license and must not be redistributed outside that context. The code is
mine; the water shader derives from NekotoArts' CC0 work (credited above).
This project is not affiliated with or endorsed by Nintendo.
