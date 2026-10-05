<img width="950" height="612" alt="railview" src="https://github.com/user-attachments/assets/f256dd98-a782-477c-9600-4580a68f3cb3" />

# RailView – OpenGL Railway Simulation

A 2D computer graphics project written in C++ on classic (immediate-mode) OpenGL with GLUT.
Every element in the scene is drawn from raw primitives – no textures, no external assets.
One internal clock (`st.tod`, hours 0–24) drives the whole simulation: sky gradient, sun and moon arcs, ambient
light tint, lamp glow, shadows, mist and window lighting all follow the time of day.

**Single source file:** [`Railview.cpp`](Railview.cpp)

---

## Features

- **Day / night / dawn cycle** – one `timeOfDay` clock (hours 0–24) drives the sky, sun,
  moon, stars, twilight colour and the ambient tint used by every lit colour.
- **Two trains** – approach, brake to a stop at the platform, open their doors, depart and
  wrap around the scene; a rail signal turns red while a train is held at the platform.
- **Passengers** – waiting people walk to the open doors, board (and disappear from the
  platform), then alight and walk back to their waiting spot on the next stop.
- **Weather** – `R` toggles rain, overcast lighting, lightning flashes, wet-ground sheen and
  ripples; mist appears at dawn/dusk and in rain; birds, clouds and meteors keep moving.
- **Shadows and ground detail** – sun-direction cast shadows under trees and people,
  grass speckles, gravel, road traffic, benches, lamps and street light cones.
- **GLSL bloom + colour grade** – the scene renders into a supersampled FBO, a bright-pass
  and separable blur produce bloom, then a grade pass adds exposure/contrast/saturation.
  Automatically falls back to the plain path if the GPU has no GL 2.0 / FBO support.
- **Procedural sound (Windows)** – rain, wheel/track rumble, horn and thunder are synthesised
  at start-up with winmm; no audio files are needed. Non-Windows builds compile as stubs.
- **Frame-rate independent animation** – all motion uses a clamped `dt`, so speed changes
  (`+` / `-`) and pausing behave the same at any frame rate.

---

## Build

freeglut is required (GLUT no longer ships with most systems).

**Linux**

```bash
g++ Railview.cpp -o RailView -lglut -lGLU -lGL -ldl
./RailView
```

**macOS**

```bash
clang++ Railview.cpp -o RailView -framework OpenGL -framework GLUT -ldl
./RailView
```

**Windows (MinGW / MSYS2)**

```bash
g++ Railview.cpp -o RailView.exe -lfreeglut -lopengl32 -lglu32 -lwinmm
```

**Code::Blocks (Windows)** – add `Railview.cpp` to a *Console application* project, and add
these to *Linker → Link libraries*: `freeglut`, `opengl32`, `glu32`, `winmm`.
(`winmm` is what provides the audio; without it the build fails on the sound symbols.)

### Compile-time switches

At the top of `Railview.cpp`:

| Define | Default | Meaning |
|--------|---------|---------|
| `USE_MSAA` | `1` | Request a multisample framebuffer. Set `0` if window creation fails. |
| `USE_POST` | `1` | Bloom / colour grade path. Set `0` to force the fixed-function renderer. |
| `USE_SOUND` | `1` | winmm audio layer. Set `0` to drop it entirely. |
| `SSAA` | `1.5f` | Scene-buffer scale for the bloom path (also acts as supersampling). |

If the GPU has no GL 2.0 or framebuffer-object support, the post-processing path disables
itself at start-up and prints a message; the simulation runs exactly as before.

---

## Controls

| Key | Action |
|-----|--------|
| `D` | Jump to daytime |
| `N` | Jump to night-time |
| `A` | Toggle automatic day/night cycle (60 s per day) |
| `R` | Toggle rain, lightning and overcast |
| `SPACE` | Pause / resume the simulation |
| `+` or `=` | Speed up time (up to 6×) |
| `-` | Slow down time (down to 0.25×) |
| `S` | Sound on / off |
| `B` | Bloom + colour grade on / off |
| `H` | Toggle the HUD |
| `F` | Fullscreen / windowed |
| `ESC` | Quit |

The on-screen HUD shows the clock, pause state, frame rate, passenger count and the
sound/bloom switches; press `H` to hide it.

---

## Notes

- The window keeps a 5:3 aspect ratio and letterboxes the rest; the HUD is drawn on top.
- Bloom needs a GL 2.0 driver with framebuffer objects (any GPU from the last ~15 years).
- Audio is Windows-only (winmm). On Linux/macOS the program runs silently.
- Made with classic OpenGL fixed-function pipeline — great for learning rasterisation,
  clipping, transforms, colour blending and scene composition.
