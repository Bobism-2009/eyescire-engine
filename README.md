# Eyescire Engine

A Unity/Godot-flavored 2D and 3D scene editor + runtime, written in
[Nexa](https://github.com/bobism-2009/Nexa-Lang). The engine is two layers:

- **`host/host.cpp`** - a tiny Win32 + GDI runtime exposing a flat C ABI
  (window, drawing, input, images, time). About 350 lines of C++ that get
  compiled once to `host.o`.
- **`engine/*.nxa`** - the engine layer in pure Nexa: color, vec, rect,
  input, timing, draw, scene. The only inline C++ in any `.nxa` file is the
  `extern "C"` prototype block in `engine/host.nxa`.

Apps (`examples/*.nxa`) `#include "../engine/engine.nxa"`, drive the host
through the Nexa wrappers, and never touch the C++ runtime directly.

## Layout

```
host/
  host.h           native runtime declarations (C ABI)
  host.cpp         Win32 + GDI implementation
engine/
  host.nxa         extern "C" decls + thin Nexa wrappers (only inline_cpp!)
  color.nxa        Color struct, palette, hex parse / format, UI palette
  rect.nxa         Rect struct, hit-test, intersection, inset
  input.nxa        VK_* constants, key_*/mouse_* helpers, modifier helpers
  timing.nxa       eng_set_target_fps, frame_begin/frame_end, dt
  draw.nxa         Color/Rect-aware wrappers around host_*
  scene.nxa        parallel-array entity store + .scene save/load
  engine.nxa       umbrella include (pulls in everything above)
  theme.nxa        editor-only look (not in engine.nxa): NexaC std/ui's
                   "modern" style + "dark" theme on the engine renderer
examples/
  smoke.nxa        engine smoke test (window, panels, palette, primitives)
  editor.nxa       Eyescire Editor: hierarchy + viewport + inspector
  runtime.nxa      Eyescire Runtime: load .scene + WASD/arrow movement
scenes/
  current.scene    last scene authored by the editor (line-based text)
build/
  *.exe            compiled apps
build.cmd          one-shot build (host.o + every example)
```

## Requirements

- Windows 10/11
- `clang++` on PATH (any modern LLVM; tested with llvm-mingw / UCRT64)
- `NexaC` on PATH (tested with the patched build in this repo, see "Patches"
  below)

## Build & run

```cmd
build.cmd
```

That step compiles `host\host.cpp` to `host.o`, then builds each example
with:

```
NexaC examples\<app>.nxa --link host\host.o --link -lgdi32 --link -luser32 -p -o build\<app>.exe
```

`-p` is `--preserve-names`, which keeps Nexa parameter and local names
intact in the generated C++ so the `extern "C"` wrappers in
`engine/host.nxa` can reference them directly.

Then:

- `build\smoke.exe` - shows the engine drawing (panels, palette, mouse
  cursor, frame counter)
- `build\editor.exe` - the scene editor
- `build\runtime.exe` - load `scenes\current.scene` and play it

## The editor

A focused 2D and 3D scene editor:

- **Toolbar** (top): kind cycle (`<>`), `New <kind>`, `Delete`, `Save`,
  `Load`, plus the current scene path
- **Hierarchy** (left, scrollable with the wheel): every active entity,
  with name, kind tag, and color swatch; click to select
- **Viewport** (center): grid + entities; click to select, drag to move,
  hover gets an outline, the selection gets handles + accent ring
- **Inspector** (right): id (read-only), name, kind (cycle), x/y/w/h, plus
  per-kind extras (`text` for `label`, `sprite path` for `sprite`); 12-color
  palette swatch and a current-color preview
- **Status bar** (bottom): entity count, current selection, transient
  feedback for save/load/create/delete

### Look

The editor follows NexaC's `std/ui` library ("modern" style, "dark" theme):
its palette (`col_ui_*` in `engine/color.nxa`), 6 px rounding, soft shadows,
accent-filled primary buttons, and an accent focus ring on inputs. `std/ui`
itself only draws into its own `gfx` window, so the editor reproduces the look
on the engine window through `engine/theme.nxa`. Chrome text is Segoe UI; code,
the console, and game content stay on Consolas, the runtime's font, so the
viewport matches the shipped game. Game `ui_panel` / `ui_button` entities get
the same rounded shape in the editor, the native player (`ui_draw_box`), and
the web player (`drawUiBox`). The player launcher uses `std/ui` directly for
its "can't start this game" window.

### 3D

A scene can render in 3D: select nothing, then set **Rendering** to **3D** in
the Inspector. The editor viewport then shows the scene in 3D, and Play runs
it there through the game camera. Exported games use `EyescirePlayer3D.exe`
(`examples/player3d.nxa`), a NexaC `std/gfx3d` OpenGL window; **Play in 3D
Window** (Project > Scripts panel) runs that player from the editor. The
editor can't host std/gfx3d's window, so `engine.dll` draws the 3D view
itself (`host/render3d.inl`): OpenGL into an off-screen multisampled buffer
that is copied under the editor's 2D chrome, with a software rasterizer as
the fallback. It follows the player's shapes and camera. Both run everything
through `engine.dll` (scene, scripts, physics, input).

- 3D view: click selects (selected entities get an outline). Dragging moves
  the selection on x/y (mouse left/right = x, up/down = y; hold Shift for one
  axis) and Ctrl+drag moves it in depth (Z). Hold the right button to look
  around, with WASD/QE to fly (Shift = faster); Alt+drag orbits, middle-drag
  pans, the wheel zooms, F frames the selection. **2D layout** (top right)
  switches to the flat editor for precise work.
- The 2D layout is the front view: x/y are unchanged (and stay Box2D's
  physics plane, so 3D scenes are 2.5D). Every entity gains **Z** (px into
  the screen) and **Depth** (px; 0 = the kind's natural depth).
- Right-click > Create > 3D Object: **Cube**, **Sphere**, **Cylinder**,
  **Model (.obj)**. Other visible entities are extruded into boxes; UI
  entities stay flat on top (std/gfx's 2D layer, with its 5x7 font).
- Lights: Right-click > Create > 3D Object > **Light**: **Directional** (a
  sun), **Point** (all around, fades by its Range), **Spot** (a cone) and
  **Ambient** (even fill). Color is the entity color; the Inspector's Light
  section has Intensity, Range, Cone angle and Tilt. Directional and spot
  lights shine down the screen, turned by Rotation and aimed into the screen
  by Tilt. A scene without lights gets a default sun. The editor view lights
  per vertex (up to 8 lights). `EyescirePlayer3D` has one directional light
  to work with (std/gfx3d), so it lights each object by what the lights add
  up to at its center: no light pools across a single big object.
- The main camera frames its rect at its Z like the 2D view; its
  **Rotation** tilts the 3D view down (+) or up (-).
- Scripts can use `ent_z` / `ent_set_z` / `ent_depth` / `ent_set_depth`.
- Scene files are v8 (`RENDER 3D` plus `|z|depth|model` and the four light
  fields on each entity);
  older scenes load unchanged. Web export flattens 3D scenes for now.

### Hotkeys

| Key            | Action                                  |
|----------------|-----------------------------------------|
| Click viewport | Select entity (or deselect on empty)    |
| Drag           | Move selected entity                    |
| Arrows         | Nudge selected entity (Shift = +-10)    |
| Alt + Arrows   | Resize selected entity                  |
| `N`            | New entity of the current kind          |
| `Del`          | Delete selected entity                  |
| `Esc`          | Deselect / cancel text edit             |
| `Ctrl+S`       | Save scene to `scenes\current.scene`    |
| `Ctrl+L`       | Load scene from `scenes\current.scene`  |
| Click a field  | Begin editing (Enter commits, Esc undo) |

Text fields accept printable ASCII and Backspace; numeric fields (x/y/w/h)
limit input to digits and `-`.

## The runtime

A minimal scene player. It loads `scenes\current.scene`, drives the
following per-frame logic, and renders:

- The entity named `player` (kind `rect`) responds to WASD or the arrow
  keys with float-based movement, clamped to the window.
- Any `rect` entity whose name starts with `coin` is treated as a pickup;
  AABB collision with the player removes it and bumps a HUD counter.

This is the seed of a per-entity scripting model: today the runtime
dispatches behavior off the entity name; the next step is an
`ent_script: string` field selected per-entity in the inspector and matched
against a small library of `script_*` Nexa handlers in `runtime.nxa`.

## Scene format

`scenes\*.scene` is line-based text, parsed in `engine/scene.nxa`:

```
EYESCIRE_SCENE 1
NEXT_ID 8
ENTITY <id>|<name>|<kind>|<x>|<y>|<w>|<h>|<r>|<g>|<b>|<sprite_path>|<text>
```

`|` is the field separator (chosen because no kind/name uses it).
`sprite_path` is empty for `rect`/`label`; `text` is empty for
`rect`/`sprite`. The scene file is plain UTF-8 and round-trips through the
editor.

## Why this layout

- **Nexa scripts only.** Every scripting/UI/scene file is `.nxa`. The C++
  side never grows; it is pure I/O glue (window, GDI, input, files).
- **One inline_cpp! file.** All `extern "C"` declarations live in
  `engine/host.nxa`. Each Nexa wrapper there forwards arguments and copies
  the return into a typed local; the rest of the engine is plain Nexa.
- **No arrays of structs.** Nexa cannot hold structs in arrays, so the
  scene store uses a fixed bank of parallel arrays (`ent_active[]`,
  `ent_x[]`, `ent_name[]`, ...) accessed through `scene_*` helpers.
- **Immediate-mode UI.** `ui_button` / `ui_field` redraw and read input
  every frame against a small set of state globals (`selected_slot`,
  `editing_field`, `edit_buffer`, ...).
