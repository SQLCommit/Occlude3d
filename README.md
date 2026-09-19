# Occlude3d v1.1 - Depth-Correct World Geometry for Ashita v4

A shared Ashita v4 **C++ plugin** that lets any Lua addon draw world-space geometry that is
**correctly occluded behind terrain, walls, and objects** - something a Lua addon cannot do reliably
on its own. Submit lines, ribbons, markers, filled triangles and textured geometry from your addon
and occlude3d renders it depth-tested, inside the engine's own render pass.

## Why this exists

In FFXI, Lua addons draw in `d3d_present` (after the world is composited) or in `d3d_beginscene`
passes, and neither can match the game's hardware depth buffer reliably - so addon geometry either
floats over everything (no occlusion) or vanishes. As atom0s has noted many times, only native code
can insert into the pipeline at the right moment with the right depth state. occlude3d does exactly
that: it trampoline-hooks the engine's `CXiActorNameDraw::OnMove` (where the world depth buffer is
bound, just before nameplates render) and draws your submitted geometry there, depth-tested. The
result occludes correctly and composites to the screen.

One plugin, shared by every addon - instead of each addon hand-rolling its own fragile beginscene
render path.

## Features

- **Depth-correct occlusion** - geometry is hidden by terrain/walls like native world objects.
- **A simple submit API** - addons send geometry via `RaiseEvent('occlude3d', ...)`; no native code
  in your addon.
- **Primitives**: lines, camera-facing ribbons (textured or flat), 3D markers, filled/textured
  triangles, and **billboarded bitmap text** (the plugin lays out + billboards glyphs from a registered
  font atlas, so the addon submits ~one byte per character) - plus a family of **UI primitives**:
  progress bars, rounded rectangles, gradient quads, rotated sprite-atlas billboards, arcs/rings,
  ground decals (AoE telegraphs), scalable 9-slice frames, and self-animating particles.
- **Render flags** (per item): additive blending (glows/auras), depth-write (occlude peers + native
  nameplates), pixel-constant on-screen size, deterministic depth layering, and a TEXT outline pass.
  *(No draw-through-walls: occlude3d is occlusion-correct by design - that's the point.)*
- **Robust**: validates every submission, isolates faults (a malformed submission drops a frame, it
  never crashes the game), keeps submitted textures alive while in use, and **never fails silently**
  - oversize / no-slot / bad-item drops are counted and surfaced (once per cause, with a count) in the in-game chat and
  the plugin's own log (see Files; nothing goes to Ashita's log).
- **Fast & lean**: same-state geometry is batched into single draws; flat footprint, zero per-frame
  allocations (safe for FFXI's 32-bit memory budget). 32 owner slots shared across all addons.
- **Diagnostics panel** (`/o3d`): live capacity, measured render cost, who's submitting, and drop
  counters - plus `/o3d stats` to measure a second and write the numbers to the log.

## Requirements

- **Ashita 4.3.1.2 or later** with plugin interface **4.30** - built against 4.3.1.2's SDK and tested on 4.3.2.1.

## Installation

1. Download `Occlude3d-vX.Y_Interface-N.NN.zip` from [Releases](https://github.com/SQLCommit/Occlude3d/releases) - the one whose
   `Interface-N.NN` matches your Ashita's plugin interface (each release's notes say which) - and extract it into your
   Ashita folder. It adds `occlude3d.dll` to `plugins/`, the docs to `docs/occlude3d/` and the magiccircle example
   addon to `addons/magiccircle/`. GitHub's "Source code" zip is not the plugin.
2. In-game: `/load occlude3d`

That's it. The occlusion hook auto-installs the first time any addon submits geometry, so the plugin
stays completely inert (no game-code patching) until something actually uses it.

## Commands

| Command | Description |
|---|---|
| `/load occlude3d` / `/unload occlude3d` | Load / unload the plugin. Unloading puts the game's code back with the game's other threads paused at a moment when none of them is inside the hook; the trampoline is kept and the DLL stays mapped until the game closes, so nothing can ever jump into unloaded code. A same-session `/load` works after a clean unload (same image, reused); a newer build needs a game restart. If the hook could not be undone (something else rewrote the site, or no quiet moment came in 3 s) it stays in as a harmless pass-through, the chat says so, and the next `/load` is refused until a restart. |
| `/o3d` (or `/o3d ui`) | Toggle the diagnostics panel (status, capacity, performance, active owners, drops). |
| `/o3d hook` | Manually install/remove the OnMove occlusion hook (it also auto-installs on first use). |
| `/o3d stats` | Measure the next 60 frames and write one line to the log and chat. |
| `/o3d diag` | Write a full report to your character's log. |
| `/o3d legacy on\|off` | Use the older, unbatched device call pattern (for timing comparisons). |

## For addon developers - the submit API

Any addon can draw **depth-occluded world geometry** by sending occlude3d a packed byte buffer via
`AshitaCore:GetPluginManager():RaiseEvent('occlude3d', byteTable)` each frame - no native code in your
addon. occlude3d keeps the last submission per **owner id** (with a TTL), so you re-submit per frame.

You get lines, markers, ribbons, filled/textured triangles, billboarded text, and a family of UI
primitives (bars, rounded rectangles, gradients, sprite atlases, arcs, ground decals, 9-slice frames,
self-animating particles) - all rendered occluded behind terrain.

> **The full reference lives in [API.md](API.md)** - presence detection (the clean `IsLoaded` check),
> the complete wire format, all 16 primitive bodies, render flags, the
> TEXT/font-atlas sub-format, texture lifetime, owner/TTL semantics, limits, and a runnable minimal
> example.

## Performance

The plugin's own cost is tiny and it scales well:

- **Draw batching** - items sharing (texture, flags, mode) are merged into one vertex array and
  drawn in a single call, so thousands of same-texture sprites cost ~1 draw and a handful of state
  changes instead of thousands.
- **Texture ref-counting is de-duplicated** per buffer (a 5000-sprite buffer sharing one texture
  validates/refs it once, not 5000 times).
- **~0.47 MB resident, flat** - no per-frame heap allocations (`DrawPrimitiveUP`, no vertex buffers),
  so nothing grows toward FFXI's address-space ceiling.

For realistic addon workloads (tens of items per frame) every cost is sub-millisecond. `/o3d stats`
measures 60 frames and writes the averages (items, owners, state calls, draw calls, draw and receive CPU, and the
slowest frame) so you can measure your own load.

## Building

See **BUILD.md**. In short: 32-bit MSVC + CMake against the Ashita SDK (`ASHITA4_SDK_PATH`), no
Safe-SEH, exports via the `.def` only.

## Technical notes

- The occlusion hook is a trampoline patch on `CXiActorNameDraw::OnMove`, located by a unique AOB
  signature scanned at load (the address is ASLR-relative). It goes in and comes out with the game's other
  threads paused, and occlude3d stays loaded until the game closes, so nothing can jump into unloaded code.
- Drawing happens inside the engine's active scene with full device-state save/restore, so it never
  corrupts the game's own rendering.
- Geometry is world-space FVF (`XYZ|DIFFUSE[|TEX1]`) drawn with `ZENABLE=1, ZWRITE=0,
  ZFUNC=LESSEQUAL` so it depth-tests against the world without writing depth.

## Files

Its log is one file per character:
`logs\occlude3d\<Name>_<id>\occlude3d.log` in the Ashita folder (the same `<Name>_<id>` folder name Ashita gives addon
settings). Before you log in it writes to a startup file in `logs\occlude3d\`, which moves into your character's log at
login. Each log keeps its newest 1 MB; older lines are trimmed away. Several game clients can run from one Ashita folder
at once without losing a line.

`/o3d diag` writes a full report into your character's log. If something goes wrong, run it and send that log.

## Version history

See [CHANGELOG.md](CHANGELOG.md).

## Thanks

- **The Ashita Team** - atom0s, thorny, and the Ashita Discord community
- **atom0s** - the `CXiActorNameDraw::OnMove` signature, and the idea of drawing there while the world's depth
  buffer is bound.

## License

GNU LGPL v3 - see **LICENSE**. (The Ashita plugin SDK is LGPL v3, so plugins built against it use
LGPL/GPL v3 by convention; LGPL fits occlude3d especially, since it's a shared library other addons link against.)
