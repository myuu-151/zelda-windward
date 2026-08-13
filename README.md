# Zelda Windward

A Wind Waker–style Zelda fan game. C++20, SDL3, raw OpenGL 3.3.

![Link riding the loftwing over the Wind Waker sea](flight.png)

## Build

```powershell
cmake -S . -B build          # first run downloads SDL 3.4.14
cmake --build build --config Debug
.\build\Debug\zelda.exe
```

## Assets

Includes copyrighted assets. The water shader derives from
[NekotoArts' CC0 work](https://www.shadertoy.com/view/3tKBDz).
Not affiliated with or endorsed by Nintendo.
