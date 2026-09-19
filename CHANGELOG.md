# occlude3d - Changelog

## v1.1

### Logs
- **One log per character**, `logs\occlude3d\<Name>_<id>\occlude3d.log`; lines from before login move into it at login.
- **Several clients can share one Ashita folder** without losing or overwriting each other's lines.
- **`diag` writes its report into your character's log** instead of a separate file.
- Every failure says so once in chat and names the log.
- **`/o3d stats` measures the next 60 frames and writes one line** (it no longer writes a line every 60 frames while on);
  the panel's button does the same.
- **`/o3d diag` is new**: hook, a measured second, the addons submitting, drops and the hook scan detail.
- A dropped submission is said once per cause, with a count at unload (it was said every 5 seconds).
- The update deletes the old files: `logs\occlude3d\occlude3d.log` and `logs\occlude3d\occlude3d.log.old`.

### Unloading and safety
- **occlude3d keeps itself loaded from the moment it hooks the game until the game closes,** so a thread inside the
  hook can never reach unloaded code.
- **The hook goes in and comes out with the game's other threads paused,** at a moment when none of them is at the
  hooked instruction, inside the hook or inside occlude3d. Coming out, it replaces only its own jump (another tool's is
  left alone) and checks the result. Hook calls still running are waited for; if they do not finish within 3 seconds,
  what they use is kept, never freed.
- **A hook that cannot be taken out stays in as a harmless pass-through,** and `/load occlude3d` is refused until the
  game restarts.
- The thread pause skips threads that have already exited. One kept alive by another handle used to make every pause
  fail, so the hook never installed.

### Performance
- **Fewer device calls per frame.** Render states, texture stage states, the texture and the vertex
  format are set only when they change, instead of being set and reset around every batch. Adjacent
  colored triangles with the same flags now share one draw call (they used to draw one item at a
  time). Submission order is unchanged. `/o3d` shows the new draw-call count beside the state count.
- **Resubmitting the same textures is cheaper.** When an owner resubmits, only textures that are new
  are referenced and only textures that are gone are released; each change used to validate every
  texture with `ReadProcessMemory` twice. The per-frame texture validation cache holds 256 (was 32).
- **`/o3d legacy on|off`** restores the old call pattern, so the two can be timed in one session.
  Textured batches break on the full flags in legacy mode, as they did before batching.

## v1.0

The first public release.