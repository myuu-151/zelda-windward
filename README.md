# Zelda Windward

A personal Zelda fan project about the feeling of flying: toon Link, his
bird, an endless painted sea and a sky to carve through. A love letter to
The Wind Waker's ocean and Skyward Sword's loftwing, built for the joy of
building it.

![Link riding the loftwing over the Wind Waker sea](flight.png)

## The nature of the thing

There's no engine here. It's C++20, SDL3 and raw OpenGL 3.3 — one
executable, a handful of source files, every shader written inline. The
sky is a fragment shader, not a skybox; the ocean is math, not a texture;
the flight is a little momentum model where speed is something you earn
by diving and spend by climbing. Almost everything visual is either
procedural or hand-animated in Blender and baked through a small export
pipeline.

It's a sandbox, not a product. There's one white island, water in every
direction, and a bird that's genuinely fun to fly — stoop, boost, barrel-
roll, skim the waves. Features arrive when they feel right on the stick,
get tuned live with in-game keys, and the winning numbers get baked into
the code. The repo history reads like a flight logbook for that process.

## Build

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build            # first run downloads + configures SDL 3.4.14
& $cmake --build build --config Debug
.\build\Debug\zelda.exe
```

SDL is fetched and statically linked by CMake — no submodules, no DLLs.
The `zelda_packed` target builds a single self-contained exe with the
assets embedded and the dev tooling compiled out.

## Flying it

Play the Song of Skies on the flute (`I`, notes `1`–`8`) and the bird
comes down. Vault on, launch with space. WASD steers; SHIFT is the power
glide (add S to stoop, W to trade the speed back for height); SPACE
mid-flight is the twirl. On foot it's the usual: roll, slash, guard,
climb.

## Assets note

The Link and loftwing models are ripped from Nintendo games (Tri Force
Heroes / Skyward Sword) and remain **© Nintendo**. They're included so the
project runs, as part of a non-commercial fan work — they are not mine to
license and must not be redistributed outside that context. The code is
mine; the water shader derives from
[NekotoArts' CC0 work](https://www.shadertoy.com/view/3tKBDz). This
project is not affiliated with or endorsed by Nintendo.
