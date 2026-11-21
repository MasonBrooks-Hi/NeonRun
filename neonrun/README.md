# NEON RUN

A 3D endless runner written against **`creaddinscart.studio.cpp.sl / v3D-1.0.0`** — a
header-only C++17 3D library with a software rasterizer and no window system.

The ship runs down a neon tunnel at ever-increasing speed. Switch lanes to dodge
pillars, jump the amber bars, duck the red beams, collect the green orbs. Three
shields, then the run ends.

![gameplay](../neonrun.gif)

## Build

```sh
# 1. unpack the library and put its headers somewhere on your include path
tar xzf v3D-1.0.0.tar.gz
cp -R v3D-1.0.0/creaddinscart.studio.cpp.sl ~/.local/include/

# 2. build the game (needs a C++17 compiler and the system zlib for PNG output)
make V3D=$HOME/.local
```

There is nothing to configure: `v3D-1.0.0` is header-only, so the only object
file in the project is the game.

## Play

```sh
./neonrun
```

Requires a truecolor terminal (`COLORTERM=truecolor`: iTerm2, Kitty, WezTerm,
Ghostty, Windows Terminal, recent Terminal.app). The picture is drawn with ANSI
`▀` half blocks, so one character cell carries two pixel rows.

If the picture looks like it is struggling, drop the present rate:
`./neonrun --fps 30`. The simulation is fixed-step, so this changes nothing
about how the game plays - only how often it is painted.

### About the sharpness

There is no blur filter to blame: **the framebuffer is exactly `cols × 2·rows`
pixels**, because one half-block cell carries one pixel across and two down. An
80×24 window is therefore an 80×48 image, and Terminal is magnifying it roughly
14× to fill the screen. Nothing in the renderer can invent detail that is not
there.

So the single biggest quality lever is more cells, not more code: shrink the
font or widen the window. 120×40 gives 120×80 (2.5× the detail of 80×24), and
160×50 gives 160×100. The HUD switches to short labels below about 96 columns
and back to full ones above.

By default the scene is rasterised 1:1 with that grid, so every cell is one
untouched pixel and edges are hard. `--smooth` rasterises at double width and
box-filters it back, which softens diagonals at the cost of smearing every
vertical edge and pulling highlights down - it looks dimmer as well as softer.

| Key | Action |
| --- | --- |
| `←` `→` or `A` `D` | switch lane |
| `↑` / `W` / `Space` | jump the amber bar |
| `↓` / `S` | duck the red beam (also fast-falls) |
| `R` | restart after a crash |
| `Q` / `Esc` | quit |

The mouse works too, and the two devices are kept apart on purpose:

| Mouse | Action |
| --- | --- |
| move the pointer | steer the lane (left / centre / right third of the window) |
| left click, or wheel up | jump |
| right click, or wheel down | duck |

A click never changes lane, even though the terminal reports the cell it
landed on - otherwise clicking to jump would also yank the ship sideways.
Any arrow key or `A`/`D` press immediately hands steering back to the keyboard.
While the game runs it enables SGR mouse tracking, so hold your terminal's
option key to select text.

## Headless modes

The library has no window backend, which makes it trivially scriptable. Every
mode below runs the same simulation on the same fixed timestep.

```sh
./neonrun --info                 # library system report
./neonrun --ascii                # one frame as ASCII, no colour needed
./neonrun --shot  shot.png       # one 640x360 frame (.ppm also accepted)
./neonrun --probe 30000          # autopilot stress test: how long it survives
./neonrun --width 640 --height 360 \
        --skip 420 --replay 300 frames
python3 tools/make_gif.py frames neonrun.gif
```

`--replay` drives the game through `GameLoop::advance()` with a seeded RNG, so
the same `--seed` produces byte-identical frames on every machine.

## Who does what

Everything between "triangle in" and "pixel out" is the library. The window is
the one thing it deliberately does not provide, so `neonrun.cpp` supplies it.

| Concern | Owner |
| --- | --- |
| Z-buffered triangle rasterizer, Phong shading | `detail/renderer.hpp`, `detail/texture.hpp` |
| Fog, two point lights riding the ship | `TexturedRenderer` |
| Ship jump / fast-fall | `PhysicsWorld` + `RigidBody` |
| Engine wash, pickup sparkle, crash blast | `ParticleSystem` × 3 |
| Fixed 60 Hz update, once-per-frame render | `GameLoop` |
| Key edges (`was_pressed`) and lane axis | `InputState` |
| Score, speed bar, shields, overlay cards | `hud_text` / `hud_bar` / `hud_circle` / `hud_rect` |
| Screen shake decay | `ease_out_elastic` |
| Terminal raw mode, arrow-key decoding, ANSI presenter, PNG encoding | this file |

## Notes on the implementation

- **Presentation geometry.** A character cell is about twice as tall as it is
  wide, so the framebuffer is `cols × (2·rows)`: one pixel per half cell, which
  keeps pixels square and the picture undistorted.
- **Supersampling.** The scene rasterises at `2·cols` wide and is box-filtered
  down to `cols` before the HUD is drawn, which removes most of the staircase
  aliasing you would otherwise get at terminal resolution.
- **Fog needs textures.** `Renderer::draw_mesh` is not virtual and does not fog;
  fog and point lights live in `TexturedRenderer::draw_textured_triangle`. So
  every draw in this game goes through `draw_textured_mesh` with a brightness
  texture, and hue comes from the `tint` argument.
- **Ground scroll for free.** The track is a strip of recycled checker tiles.
  They are cheap, and their pattern is what sells the speed.

## Licence

MIT, matching v3D-1.0.0 itself. Keep the `SPDX-License-Identifier` lines.
