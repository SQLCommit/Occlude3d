# API reference

[Back to Occlude3d](README.md)

> **Wire format:** `O3D2` (v1) submission + `O3DF` (v1) font registry. **This doc matches occlude3d v1.1** (the wire format is unchanged since v1.0).
> Everything is **little-endian** and **packed (no padding)**. occlude3d's format is first-party (not
> reverse-engineered); the C structs below mirror the plugin's own decode.

occlude3d is a **shared rendering library**: any Ashita addon submits world-space geometry, and the
plugin draws it **depth-occluded** against the game's world depth buffer (hidden by terrain/walls like
native world objects) by hooking the engine's nameplate-draw routine. Your addon sends raw bytes; the
plugin owns all the rendering.

---

## 1. Requirements

- **Ashita 4.3.1.2 or later** with plugin interface **4.30** - occlude3d is built against 4.3.1.2's SDK and
  tested on 4.3.2.1. Load the plugin with `/load occlude3d`.
- Your addon: `require('struct')` (to pack the buffer) and, for textures, `require('d3d8')` + `ffi`.
- occlude3d ships no SDK/headers - the entire interface is the byte buffer documented here.

## 2. Presence detection

occlude3d is a plugin, so **confirm it's loaded before submitting** - a `RaiseEvent` to an absent
plugin is a **silent no-op** (your geometry just vanishes with no error). Use
`GetPluginManager():IsLoaded()`: it's a clean boolean, so you can poll it every frame even 
while occlude3d is absent:

```lua
if (AshitaCore:GetPluginManager():IsLoaded('occlude3d')) then
    -- loaded; submit geometry
end
```

Still warn the user once if it's missing (don't fail silently), and re-arm when it returns (so e.g. a
registered font re-registers after an occlude3d reload):

```lua
local chat = require('chat');
local ok, warned = false, false;
local function occ3d_present()
    if (AshitaCore:GetPluginManager():IsLoaded('occlude3d')) then
        if (not ok) then ok, warned = true, false; end   -- (re)loaded: re-arm warning / re-register fonts
        return true;
    end
    ok = false;
    if (not warned) then
        warned = true;
        print(chat.header(addon.name) .. chat.error('occlude3d not loaded - run: /load occlude3d'));
    end
    return false;
end
```

## 3. The submit event

| | |
|---|---|
| **Event name** | `'occlude3d'` |
| **Sender** | `AshitaCore:GetPluginManager():RaiseEvent('occlude3d', byteTable)` |
| **Payload** | a **byte table** (array of 0-255), little-endian - or a raw pointer + size (the fast path below). No string overload (binary buffers contain null bytes; a `const char*` mapping would truncate), so marshal the packed string to a table or an `ffi` buffer. |
| **Receiver** | the plugin's `HandleEvent`; not visible to your addon. |
| **Returns** | nothing. No success/failure signal - check presence first (section 2) and the `/o3d` panel for drops. |

Marshal + fire:

```lua
local function raise(data)            -- data = a packed binary string
    local t = {};
    for i = 1, #data do t[i] = data:byte(i); end
    AshitaCore:GetPluginManager():RaiseEvent('occlude3d', t);
end
```

**Fast path (recommended for per-frame submitters).** The loop above allocates a table every call. The
3-arg form `RaiseEvent(name, ptr, size)` takes a raw pointer + length instead, so you pack once and
`ffi.copy` into one reusable buffer - a single memcpy, zero per-frame allocation:

```lua
local ffi = require('ffi');
local CAP = 16384;                                    -- occlude3d's per-owner cap
local buf = ffi.new('uint8_t[?]', CAP);
local ptr = tonumber(ffi.cast('uintptr_t', buf));
local mgr = AshitaCore:GetPluginManager();
local function raise(data)                            -- data = a packed binary string
    local n = #data;
    if (n == 0 or n > CAP) then return; end           -- never overflow the buffer
    ffi.copy(buf, data, n);
    mgr:RaiseEvent('occlude3d', ptr, n);
end
```

(This is what the bundled `magiccircle` uses.)

Two message kinds share the event, told apart by their leading magic: an **O3D2** geometry submission
(this is the common one) and an **O3DF** font registration (section 7).

## 4. Wire format (`O3D2`)

A submission is a header followed by `itemCount` variable-length items. Each item begins with a 32-bit
type word, then a body chosen by its base type.

```c
struct o3d2_header {        // 16 bytes
    uint32_t magic;        // 'O3D2' = 0x4F334432
    uint32_t owner;        // addon-chosen FourCC; one owner = one slot (re-submit replaces it)
    uint32_t ttl_ms;       // lifetime if not re-submitted; itemCount == 0 clears this owner
    uint32_t itemCount;    // number of items that follow
};

struct o3d2_typeword {     // 4 bytes, prefixes every item
    uint32_t baseType : 8; // 0-15, selects the body (section 6)
    uint32_t flags    : 24;// section 5: 0x01 additive, 0x04 depth-write, 0xF0 layer, 0x100 TEXT-outline
};                         // == (baseType & 0xFF) | (flags << 8)
```

- **owner** - your 32-bit id (use a FourCC, e.g. `'MAGC'`). Re-using the same owner replaces its geometry.
- **ttl_ms** - per-frame submitters use ~200-300; PARTICLE submits once with a long ttl (section 6).
- **itemCount = 0** clears that owner (use on unload).

## 5. Flags

Encoded in the high 24 bits of `typeWord`. `flags = 0` is the default occluded draw.

| Flag | Value | Meaning | Applies to |
|---|---|---|---|
| Additive | `0x01` | Additive blend (glow/aura; DESTBLEND=ONE) | all |
| Depth-write | `0x04` | Item writes depth (alpha-tested) so it occludes peers + later native nameplates | all (opaque content - text/HUD, not glows) |
| Pixel-constant size | `0x08` | A billboard's `halfW`/`halfH` are **screen pixels** (constant on-screen size at any distance) instead of world units | BQUAD, BAR, RRECT, GQUAD, SPRITE, ARC, NINESLICE, PARTICLE (not TEXT/DECAL) |
| Depth layer | `0xF0` | Layer `0-15` = `(flags>>4)&0xF`; nudged toward the eye one step per layer so stacked HUD elements order deterministically. Higher = in front. | same set as `0x08` |
| TEXT outline | `0x100` | 16-direction dark halo behind glyphs. Thickness in **bits 9-12** = `(flags>>9)&0xF` = 1-15% of glyph height (0 = default 7%). | TEXT only |

## 6. Primitive reference

`baseType` (low byte of `typeWord`) selects the body that follows it. **Item bytes** = 4 (type word) +
body - use it for the 16 KB budget (section 11). "Billboarded" = the plugin faces it to the camera from
the live view matrix (camera-independent: a still anchor doesn't shift as the camera rotates).

| baseType | Item bytes | What |
|---|---|---|
| 0 LINE | 36 | world line A->B (or camera-facing ribbon) |
| 1 MARKER | 24 | 3D cross |
| 2 RIBBON | 16 + 12·nPoints | untextured camera-facing strip |
| 3 TEXRIBBON | 20 + 12·nPoints | textured strip |
| 4 TRIS | 8 + 16·vertCount | filled triangle list |
| 5 TEXTRIS | 12 + 24·vertCount | textured triangle list |
| 6 TEXT | 36 + glyphCount | billboarded bitmap text |
| 7 BQUAD | 32 | billboarded quad (RRECT/BAR/GQUAD specialize it) |
| 8 BAR | 40 | progress bar |
| 9 RRECT | 32 | rounded rectangle |
| 10 GQUAD | 36 | vertical-gradient quad |
| 11 SPRITE | 52 | rotated quad with a texture sub-rect |
| 12 ARC | 36 | pie/ring fan |
| 13 DECAL | 36 | flat ground quad (not billboarded) |
| 14 NINESLICE | 40 | scalable 9-slice frame |
| 15 PARTICLE | 48 | drifting/fading billboard (submit once) |

Each body follows the 4-byte `o3d2_typeword`:

```c
struct line      { uint32_t color; float a[3], b[3]; float width; };                                        // 0  width 0=1px, >0=world ribbon
struct marker    { uint32_t color; float a[3]; float size; };                                               // 1
struct ribbon    { uint32_t color; float width; uint32_t n; float pts[/*n*3*/]; };                          // 2  width in screen px; n>=2
struct texribbon { uint32_t color; float width; uint32_t texPtr; uint32_t n; float pts[]; };                // 3
struct tris      { uint32_t vertCount; struct { float x,y,z; uint32_t color; } v[]; };                      // 4  vertCount %3==0
struct textris   { uint32_t texPtr; uint32_t vertCount;
                   struct { float x,y,z; uint32_t color; float u,v; } v[]; };                               // 5  vertCount %3==0
struct text      { uint32_t fontId; uint32_t color; float size; float anchor[3];
                   uint32_t align; uint32_t glyphCount; uint8_t glyphs[]; };                                // 6  align 0=center/1=left/2=right
struct bquad     { uint32_t texPtr; uint32_t color; float center[3]; float halfW, halfH; };                 // 7  texPtr 0 = colored
struct bar       { uint32_t texPtr; uint32_t bgColor, fillColor;
                   float center[3]; float halfW, halfH, fraction; };                                        // 8
struct rrect     { uint32_t color; float center[3]; float halfW, halfH, radius; };                          // 9
struct gquad     { uint32_t topColor, botColor, texPtr; float center[3]; float halfW, halfH; };             // 10
struct sprite    { uint32_t texPtr, color; float center[3];
                   float halfW, halfH, rot; float u0, v0, u1, v1; };                                        // 11
struct arc       { uint32_t color; float center[3]; float radius, startRad, sweepRad, innerFrac; };         // 12
struct decal     { uint32_t texPtr, color; float center[3]; float halfW, halfH, rot; };                     // 13  FLAT
struct nineslice { uint32_t texPtr, color; float center[3]; float halfW, halfH, borderFrac, borderWorld; }; // 14
struct particle  { uint32_t texPtr, color; float center[3]; float halfW, halfH;
                   float vel[3]; float fadeFrac; };                                                         // 15  SUBMIT ONCE
```

**Per-primitive notes**
- **LINE** - `width` 0 = 1px hairline; >0 = world-unit camera-facing ribbon.
- **RIBBON / TEXRIBBON** - `width` is in **screen pixels**; `n >= 2`.
- **TRIS / TEXTRIS** - `vertCount` must be a multiple of 3; TEXTRIS texture is MODULATE-blended.
- **TEXT** - glyphs are raw codepoint bytes; `(cp - firstCp)` indexes the atlas grid, out-of-range -> `'?'`. Register the font first (section 7).
- **BAR** - the plugin draws the bg then the left-aligned fill (width `2·halfW·fraction`) from its live camera basis, so the fill never drifts/jitters when the camera moves. `texPtr` modulates both quads.
- **SPRITE** - rotates by `rot` (radians) and samples the `(u0,v0)-(u1,v1)` sub-rect (sprite atlases / scrolling animation).
- **DECAL** - laid **FLAT on the world horizontal plane**.
- **NINESLICE** - the texture's `borderFrac` edge band stays `borderWorld` size while the center/edges stretch.
- **PARTICLE** - drifts by `vel` (world units/sec) and fades over the last `(1-fadeFrac)` of its ttl. **Submit once** (long ttl, unique owner, don't re-submit); animated entirely plugin-side.
- `texPtr = 0` means "colored / no texture" wherever the body allows it.

## 7. Font registration (`O3DF`)

The TEXT primitive needs a glyph atlas, registered with a **separate message** (different magic), sent once:

```c
struct o3df_register {     // 28 bytes
    uint32_t magic;        // 'O3DF' = 0x4F334446
    uint32_t fontId;       // referenced by text.fontId
    uint32_t texPtr;       // a cols x rows grid atlas (equal cells); 0 = UNREGISTER this fontId
    uint32_t cols, rows;   // atlas grid dimensions
    uint32_t firstCp;      // codepoint of cell [0,0]
    float    advance;      // per-glyph width as a fraction of glyph height (monospace atlas ~0.72)
};
```

- Max **8** fonts. The plugin AddRefs the atlas while registered (hold your own ref too).
- **Re-register after an occlude3d reload** (watch `IsLoaded()` go false, then re-send once it's back).
- TEXT outline (flag `0x100`) looks best with a **solid-fill** atlas (a hollow/outline font has little for the halo to wrap).

## 8. Conventions

- **Color** = `0xAARRGGBB` (alpha-blended).
- **World coordinates** are D3D order: `(LocalPositionX, LocalPositionZ = vertical, LocalPositionY)` =
  `(east/west, vertical, north/south)`. **"Up" is a *smaller* vertical value.** Get a position with
  `GetLocalPositionX/Z/Y(idx)`. *(This axis mapping is the most common submit bug.)*
- **Textures** (`texPtr`) - pass a live `IDirect3DTexture8*` from your addon as a `u32` (same process +
  device, so the plugin binds it directly). Load with `D3DXCreateTextureFromFileA` and pass
  `tonumber(ffi.cast('uintptr_t', ffi.cast('void*', tex)))`. **Keep it alive >= `ttl_ms`** (hold your own
  reference); occlude3d AddRefs defensively but you should too.

## 9. Lifetime & ownership

- **Owner slots: 32 total, shared across all addons.** Pick a stable, unique `owner` FourCC per logical
  layer. A new owner beyond 32 is dropped (section 11).
- A submission **replaces** that owner's prior geometry. To **update**, re-submit. To **clear**, submit a
  0-item buffer (or let the TTL expire).
- **PARTICLE is the exception:** submit **once** with a long ttl and do not re-submit - the plugin
  animates and expires it.
- **On unload:** submit a 0-item buffer for each owner you used, and unregister your fonts (`O3DF` with
  `texPtr = 0`). The defensive AddRef makes reload crash-safe regardless, but clearing is the polite
  contract and avoids geometry lingering for one ttl after unload.

## 10. Worked example

A complete addon that draws one cyan vertical line at your feet, correctly occluded:

```lua
addon.name = 'occdemo'; addon.version = '1.0';
require('common');
local struct = require('struct');

local function raise(data)
    local t = {}; for i = 1, #data do t[i] = data:byte(i); end
    AshitaCore:GetPluginManager():RaiseEvent('occlude3d', t);
end

ashita.events.register('d3d_present', 'present_cb', function()
    if (not AshitaCore:GetPluginManager():IsLoaded('occlude3d')) then return; end   -- presence (section 2)
    local mm  = AshitaCore:GetMemoryManager();
    local idx = mm:GetParty():GetMemberTargetIndex(0);
    if (idx == 0) then return; end
    local e = mm:GetEntity();
    local x, z, y = e:GetLocalPositionX(idx), e:GetLocalPositionZ(idx), e:GetLocalPositionY(idx);

    -- one LINE item (baseType 0, no flags): color, a[3], b[3], width
    local item = struct.pack('<I I fff fff f', 0, 0xFF33CCFF, x, z, y, x, z - 4.0, y, 0.08);
    local hdr  = struct.pack('<I I I I', 0x4F334432, 0x4F434431 --[['OCD1']], 250, 1);
    raise(hdr .. item);
end);
```

That LINE item on the wire (the `o3d2_header` precedes it; offsets are within the item):

```
offset  bytes  field
   0      4     typeWord   = 0x00000000  (baseType 0 = LINE, no flags)
   4      4     color      = 0xFF33CCFF  (0xAARRGGBB)
   8     12     a[3]       = x, z, y     (start)
  20     12     b[3]       = x, z-4, y   (end)
  32      4     width      = 0.08f
  ----------    item total = 36 bytes
```

## 11. Limits

| Limit | Value |
|---|---|
| Owner slots (total, all addons) | 32 |
| Bytes per owner submission | 16 KB (16384) - a submission `< 16` or `> 16 KB` is dropped |
| Registered fonts | 8 |
| RIBBON/TEXRIBBON points | `>= 2`, bounded by buffer size |
| TRIS/TEXTRIS vertex count | multiple of 3, bounded by buffer size |

Over-limit and malformed submissions are **counted and reported**. Use `/o3d` or `/o3d diag` to inspect them.

**Performance:** compatible adjacent geometry is batched while preserving submission order, including colored triangles in v1.1. Texture references are de-duplicated and device states are only set when needed. Batches still split when rendering state or capacity requires it; do not assume that every group using one texture becomes one draw call. `/o3d legacy on` restores the older call pattern for comparison, and `/o3d stats` measures the next 60 frames.

## 12. License

occlude3d is licensed under the **GNU LGPL v3** (see [LICENSE](LICENSE) and [GPL-3.0.txt](GPL-3.0.txt)).
