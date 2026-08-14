# Zelda Windward

A Wind Waker–style Zelda fan game. C++20, SDL3, raw OpenGL 3.3.

![Link riding the loftwing over the Wind Waker sea](flight.png)

## Build

```powershell
cmake -S . -B build          # first run downloads SDL 3.4.14
cmake --build build --config Debug
.\build\Debug\zelda.exe
```

The `zelda` target reads `assets/` from disk and keeps the dev tooling
(clip viewer, screenshots, live tuning keys). For a release build — one
self-contained exe with every asset embedded and the debug tooling
compiled out — build `zelda_packed` instead:

```powershell
cmake --build build --config Release --target zelda_packed
.\build\Release\zelda_packed.exe
```

That exe runs anywhere on its own: no `assets/` folder, no tune files,
no debug keys, everything baked in.

## Controls

| input | action |
|---|---|
| W A S D | move · steer in flight (W climbs, S dives) |
| space | roll · vault onto the bird · launch · **twirl** mid-flight |
| shift | guard on foot · boost in flight (+S stoops, +W climbs) |
| J | attack |
| I | flute — `1`–`8` play the octave, hold ↑/↓ for sharps and flats |
| arrow keys | orbit the camera |
| esc | quit |

The Song of Skies is `1 5 8 6 5 3`. Guarding with a target in view locks
the camera onto it. Flight speed is real momentum — it builds diving,
bleeds climbing, and drags off slowly.

Mounted, space launches only while you're **moving** — press it standing
still and you hop back off instead.

**SDL controller support coming soon.** Keyboard only for now.

The dev build adds F1 clip viewer, F2 screenshot, F4 dive-freeze and live
seat/shield/camera tuning keys; `zelda_packed` has none of them.

## Assets

Includes copyrighted assets. The water shader derives from
[NekotoArts' CC0 work](https://www.shadertoy.com/view/3tKBDz).
Not affiliated with or endorsed by Nintendo.
