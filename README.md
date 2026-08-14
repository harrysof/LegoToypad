# LegoToypad

A controller-driven companion app for LEGO Dimensions in Cemu. Browse franchises
and figures, pick a Toypad position, and send it straight to Cemu, no mouse
needed. Requires a Cemu build with the local Toypad listener enabled.

Every asset - figure tags, portraits, world logos, the background, the wordmark,
the app icon - is compiled into `LegoToypad.exe` itself as a Win32 resource. The
exe is fully self-contained; there are no loose `.bin`/`.png` files next to it.

## Build

From a **Developer PowerShell for VS** (or after installing Visual Studio's C++ build tools):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\LegoToypad.exe`

Regenerating the embedded assets happens automatically on every build
(`generate_assets.py` walks the `All Bin Files` + `Assets` folders and emits
`resources.rc`, `GeneratedAssetTable.{h,cpp}` and `app.ico` into `build\generated`).
The script only rewrites a file when its content changes, so incremental builds
don't recompile the resources unnecessarily. Add or change a `.bin`/`.png` in
the source folders and just rebuild - no manual step.

No Visual Studio? Use MinGW instead:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

Output: `build\LegoToypad.exe`

## Package

```powershell
mkdir dist
copy build\Release\LegoToypad.exe dist\
copy LegoToypad.ini dist\
copy README.md dist\
Compress-Archive -Path dist\* -DestinationPath LegoToypad-v1.0.0-win64.zip
```

The exe carries the full tag library and all images inside it - there is
nothing else to bundle.

## Release

```powershell
git add -A
git commit -m "Embed assets as resources, add franchise browsing, GDI+ glossy UI"
git tag v1.0.0
git push origin main --tags
```

Then, with the `gh` CLI:

```powershell
gh release create v1.0.0 LegoToypad-v1.0.0-win64.zip `
  --title "LegoToypad v1.0.0" `
  --notes "Self-contained single exe with embedded tag library and artwork; franchise browsing with glossy pads and portraits."
```

No `gh`? Same thing works from the repo's Releases page on GitHub: Draft a new release, then attach the zip.

The exe is unsigned, so Windows SmartScreen will flag it on first launch. Tell people to click "More info" → "Run anyway," or it'll look broken to anyone downloading it cold.

## Setup

1. Run `LegoToypad.exe`. It sits in the tray and stays hidden until triggered.
2. Check `LegoToypad.ini` — the listener port must match Cemu's `DimensionsToypadListenerPort` (default `9191`).

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | Move selection (pads, world grid, roster grid) |
| A / Enter | Select / confirm |
| B / Escape | Back |
| Y / S | Open settings (from the pad grid) |

Any connected controller works — player 1 through 4, not just the first pad.

## Overlay shortcut

Default toggle is the controller **Back** button. Change it from Settings → Toggle shortcut:

- **Controller combo:** release everything first, then press the new combo. It must include something other than A/B/Y/D-pad up-down (those are already used for menu navigation) — LB, RB, X, Back, Start, or a stick click all work. **Back+Start always cancels.**
- **Keyboard shortcut:** must include Ctrl, Alt, Shift, or Win. A bare key is rejected, since an unmodified global hotkey would swallow that key everywhere on your PC while the app is running. **Esc cancels.**

## Listener protocol

Sends Cemu's LOAD message: a 5-byte header (command, pad, slot, two reserved zero bytes) followed by 180 raw tag bytes, plus a 2-byte path length and path. The tag bytes come from the embedded resources, and since there is no on-disk `.bin` anymore the path is empty — game writes stay in Cemu's memory for the session instead of persisting to a file.

## Notes

- Regenerate the asset table with `python generate_assets.py --root . --out build\generated` on demand if you ever want a manual refresh.
- The controller-exclusivity handoff (neutralizing real input while this app has focus) uses a named Windows event, `Local\CemuToypadPickerInputActive`. This string must match the corresponding check in the Cemu fork's `Controller.cpp` exactly, or the handoff will silently stop working.
