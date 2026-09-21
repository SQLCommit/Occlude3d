# Building

[Back to Occlude3d](README.md)

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

Output: `build\Release\occlude3d.dll` - copy it into `/ashita/plugins/`.

Fully close FFXI before replacing an installed DLL that has been used in the current session.

**`-A Win32` is required.** A 64-bit build compiles and then fails to load, because the FFXI client and Ashita are
both 32-bit. `-DCMAKE_BUILD_TYPE=Release` is what makes the SDK's CMake helper apply its release compiler and
linker options.

Safe-SEH is deliberately left off; the legacy Direct3D8 import libraries are not `/SAFESEH`-compatible. Exports come
from `src/exports.def` only.

The GitHub release workflow (`.github/workflows`) builds the same way, with the SDK commit pinned in
`.github/release.json`.

## Release documentation

Edit the root `README.md` for GitHub. Both release workflows convert it to plain Markdown inside `docs/occlude3d/README.md` in the ZIP, keeping the source unchanged. Badges become links, image headings become text, and dropdown contents stay readable.

Preview the packaged README in PowerShell:

```powershell
./.github/scripts/export-readme.ps1 -Output build/README.release.md
```

Documentation comes from the revision being packaged. Existing ZIPs do not change when the README is edited later.

The example addon files are listed individually in `.github/release.json`. Add any new example runtime files there; screenshots, local notes, and development tools are not release files.
