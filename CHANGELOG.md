# Changelog

[Back to Occlude3d](README.md)

## v1.1

### Improved

- Reduced graphics-device calls by reusing unchanged render state and batching compatible adjacent colored triangles. Submission order stays unchanged.
- Reuses texture references between submissions and expands the validation cache.
- Added `/o3d legacy on|off` to compare the old rendering-call pattern in the same session. The panel now shows draw-call and state-change counts.

### Diagnostics

- Per-character logs support multiple clients sharing one Ashita folder. Pre-login entries move into the character's log after login.
- Added `/o3d diag` for hook status, measured rendering cost, submitters, dropped submissions, and scan details.
- `/o3d stats` and the panel button now measure the next 60 frames once.
- Failures report once in chat with the log location. Dropped submissions report once per cause, with totals at unload.
- Removes the obsolete shared log files during the update.

### Unloading and safety

- Keeps the DLL mapped after its hook is first installed, preventing in-flight calls from reaching unloaded code. Replacing the DLL requires a game restart.
- Installs and removes the hook with other game threads paused outside the affected code, verifies the result, and leaves another tool's replacement hook untouched.
- Waits for active hook calls during unload. If they cannot finish, their resources are retained instead of freed.
- If hook removal fails, leaves a safe pass-through and refuses another load until FFXI restarts.
- Ignores already-exited threads when pausing, fixing a case that prevented hook installation.

## v1.0

Initial release.
