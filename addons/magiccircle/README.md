# Magic Circle v1.0.0

An example addon for the [occlude3d](https://github.com/SQLCommit/Occlude3d) plugin - two effects that render
**occluded behind terrain** (walk behind a hill and they're correctly hidden).

## Effects

- **Rune Circle** - a glowing seal at your feet with the 8 compass directions floating around it. Pick any color.
- **Explosions** - a 360-degree burst of sparks + smoke + an expanding ground ring. Auto-repeats, or fire one with `/mc boom`.

## Requirements

- Ashita v4, with the **occlude3d** plugin loaded (`/load occlude3d`). Without it, Magic Circle stays idle and says so
  once in chat.

## Install

It comes with the occlude3d release zip: extracting that into your Ashita folder adds `addons\magiccircle\`. Then
in-game: `/addon load magiccircle`

## Commands

- `/mc` (or `/magiccircle`) - show or hide the settings panel (colors, sizes, toggles)
- `/mc boom` - fire one explosion

## License

MIT - see `LICENSE`.
