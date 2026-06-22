# occlude3d - Depth-Correct World Geometry for Ashita v4.3

[![Latest release](https://img.shields.io/github/v/release/SQLCommit/Occlude3d?sort=semver)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/SQLCommit/Occlude3d/total)](../../releases)
[![License](https://img.shields.io/badge/license-LGPL--3.0_%2F_MIT-blue)](#license)
![Ashita](https://img.shields.io/badge/Ashita-4.3.1.2_(iface_4.30)-blueviolet)

A shared Ashita v4 **C++ plugin** that lets any Lua addon draw world-space geometry that is
**correctly occluded behind terrain, walls, and objects** - something a Lua addon cannot do reliably
on its own.

## Install

Download the latest **[Release](../../releases/latest)**, unzip it, and copy the two folders into your
Ashita v4 install:

- `plugins/occlude3d.dll` -> your Ashita `plugins/` folder
- `addons/magiccircle/`   -> your Ashita `addons/` folder

Then in-game:

```
/load occlude3d              (/o3d opens the diagnostics panel)
/addon load magiccircle      (/mc opens its settings panel)
```

The occlusion hook auto-installs the first time any addon submits geometry, so the plugin stays inert
(no game-code patching) until something uses it.

## Features

- **Depth-correct occlusion** - geometry is hidden by terrain/walls like native world objects.
- **A simple submit API** - addons send geometry via `RaiseEvent('occlude3d', ...)`.
- **16 primitives, one path** - lines, markers, ribbons, filled/textured triangles, billboarded text,
  and a family of UI shapes (bars, arcs, ground decals, 9-slice frames, particles...). Most are thin
  specializations of a single billboarded quad, so the set is broad but nearly free to support.
- **Robust**: validates every submission, isolates faults (a malformed submission drops a frame, it
  never crashes the game), keeps submitted textures alive while in use.
- **Fast & lean**: same-state geometry is batched into single draws; flat footprint, zero per-frame
  allocations (safe for FFXI's 32-bit memory budget). 32 owner slots shared across all addons.
- **Diagnostics panel** (`/o3d`): live capacity, measured render cost, who's submitting, and drop
  counters.

## Commands

| Command | Description |
|---|---|
| `/load occlude3d` / `/unload occlude3d` | Load / unload the plugin. |
| `/o3d` (or `/o3d ui`) | Toggle the diagnostics panel (status, capacity, performance, active owners, drops). |
| `/o3d hook` | Manually install/remove the OnMove occlusion hook (it also auto-installs on first use). |
| `/o3d stats` | Toggle per-frame render diagnostics to the Ashita debug log. |

## For addon developers - the submit API

Any addon can draw **depth-occluded world geometry** by sending occlude3d a packed byte buffer via
`AshitaCore:GetPluginManager():RaiseEvent('occlude3d', byteTable)` each frame.
occlude3d keeps the last submission per **owner id** (with a TTL), so you re-submit per frame.

> **The full reference lives in [API.md](API.md)**

A complete working addon - **[`addons/magiccircle/`](addons/magiccircle)** (a rune circle with floating
compass directions + explosions) - is included here as a full submit-API example; read `magiccircle.lua`
to see it end to end. `API.md` above also has a runnable minimal example.

## Requirements

- **Ashita 4.3.1.2** (interface version **4.30**) - the version occlude3d was built and tested against.

## Performance

- **Draw batching** - items sharing (texture, flags, mode) are merged into one vertex buffer and drawn
  in a single call, so thousands of same-texture sprites cost ~1 draw and a handful of state changes.
- **Texture ref-counting is de-duplicated** per buffer (a 5000-sprite buffer sharing one texture
  validates/refs it once, not 5000 times).
- **~0.47 MB resident, flat** - no per-frame heap allocations (`DrawPrimitiveUP`, no vertex buffers).

## Technical notes

- The occlusion hook is a trampoline patch on `CXiActorNameDraw::OnMove`, located by a unique AOB
  signature scanned at load (the address is ASLR-relative). The hook is removed in `Release()` before
  the DLL unmaps.
- Drawing happens inside the engine's active scene with full device-state save/restore, so it never
  corrupts the game's own rendering.
- Geometry is world-space FVF (`XYZ|DIFFUSE[|TEX1]`) drawn with `ZENABLE=1, ZWRITE=0,
  ZFUNC=LESSEQUAL` so it depth-tests against the world without writing depth.

## Repository layout

Everything's here - plugin + test addon to drop into Ashita, and the plugin source to read.

```
plugins/occlude3d.dll  the runnable plugin (32-bit DLL)
addons/magiccircle/    the runnable example addon (Lua source + assets)
src/                   plugin C++ source (occlude3d.cpp / .hpp) - read / audit
API.md                 the submit-API + O3D2 wire-format reference
```

## Thanks

- **The Ashita Team** - atom0s, thorny, and the Ashita Discord community

## License

This repo is **dual-licensed by component**:

- **occlude3d** (the plugin - `plugins/`, `src/`, `API.md`): **GNU LGPL v3** - see [LICENSE](LICENSE) +
  [GPL-3.0.txt](GPL-3.0.txt). The Ashita plugin SDK is LGPL v3, and LGPL fits a shared library that
  other addons link against.
- **magiccircle** (the example addon - `addons/magiccircle/`): **MIT** - see
  [addons/magiccircle/LICENSE](addons/magiccircle/LICENSE).
