# Zelda Windward

A Zelda fan game about flying. C++20, SDL3, raw OpenGL 3.3, no engine.

![Link riding the loftwing over the Wind Waker sea](flight.png)

## Build

```powershell
cmake -S . -B build          # first run downloads SDL 3.4.14
cmake --build build --config Debug
.\build\Debug\zelda.exe
```

## Assets

The Link and loftwing models are © Nintendo, included only as part of
this non-commercial fan work — don't redistribute them. The water shader
derives from [NekotoArts' CC0 work](https://www.shadertoy.com/view/3tKBDz).
Not affiliated with or endorsed by Nintendo.
