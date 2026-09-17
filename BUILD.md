# Building occlude3d

Releases on GitHub carry the built `occlude3d.dll`, so you only need this if you want to change something.

## Requirements

- Windows, Visual Studio 2022 with the **x86** toolset (Ashita plugins are 32-bit)
- CMake 3.22+
- The Ashita v4 SDK - the `plugins/sdk` folder of [AshitaXI/Ashita-v4beta](https://github.com/AshitaXI/Ashita-v4beta)
  (a folder containing `Ashita.h`)

## Build

Point `ASHITA4_SDK_PATH` at the SDK, then:

```
set ASHITA4_SDK_PATH=C:\path\to\ashita-sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build\Release\occlude3d.dll` - copy it into `Ashita-v4beta-main\plugins\`.

**`-A Win32` is required.** A 64-bit build compiles and then fails to load, because the FFXI client and Ashita are
both 32-bit. `-DCMAKE_BUILD_TYPE=Release` is what makes the SDK's CMake helper apply its release compiler and
linker options.

Safe-SEH is deliberately left off; the legacy Direct3D8 import libraries are not `/SAFESEH`-compatible. Exports come
from `src/exports.def` only.

The GitHub release workflow (`.github/workflows`) builds the same way, with the SDK commit pinned in
`.github/release.json`.
