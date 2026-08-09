# LegoToypad

A controller-driven companion app for LEGO Dimensions in Cemu. Browse your tag library, pick a figure and a Toypad position, and send it straight to Cemu, no mouse needed.

Requires a Cemu build with the local Toypad listener enabled.

## Build

From a **Developer PowerShell for VS** (or after installing Visual Studio's C++ build tools):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\LegoToypad.exe`

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

Don't bundle your `.bin` tag library in the zip — just the exe, ini, and README. Users supply their own.

## Release

```powershell
git add -A
git commit -m "Rename ToypadPicker to LegoToypad, add multi-controller support, fix shortcut capture"
git tag v1.0.0
git push origin main --tags
```

Then, with the `gh` CLI:

```powershell
gh release create v1.0.0 LegoToypad-v1.0.0-win64.zip `
  --title "LegoToypad v1.0.0" `
  --notes "Multi-controller support, safer shortcut capture (Back+Start cancels, keyboard shortcuts require a modifier), renamed from ToypadPicker."
```

No `gh`? Same thing works from the repo's Releases page on GitHub: Draft a new release, then attach the zip.

The exe is unsigned, so Windows SmartScreen will flag it on first launch. Tell people to click "More info" → "Run anyway," or it'll look broken to anyone downloading it cold.

## Setup

1. Place your figure collection in a folder named `Lego Dimensions Organized bins`, found automatically by searching upward from the executable.
2. Check `LegoToypad.ini` — the listener port must match Cemu's `DimensionsToypadListenerPort` (default `9191`).
3. Run `LegoToypad.exe`. It sits in the tray and stays hidden until triggered.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | Move selection |
| A / Enter | Select figure / send to Cemu |
| B / Escape | Back |
| Y / S | Open settings (from the figure list) |

Any connected controller works — player 1 through 4, not just the first pad.

## Overlay shortcut

Default toggle is the controller **Back** button. Change it from Settings → Toggle shortcut:

- **Controller combo:** release everything first, then press the new combo. It must include something other than A/B/Y/D-pad up-down (those are already used for menu navigation) — LB, RB, X, Back, Start, or a stick click all work. **Back+Start always cancels.**
- **Keyboard shortcut:** must include Ctrl, Alt, Shift, or Win. A bare key is rejected, since an unmodified global hotkey would swallow that key everywhere on your PC while the app is running. **Esc cancels.**

## Listener protocol

Sends Cemu's LOAD message: a 5-byte header (command, pad, slot, two reserved zero bytes) followed by 180 raw tag bytes, 185 bytes total, over TCP to `127.0.0.1`.

## Notes

- No CMake/compiler was available in the environment these changes were written in, so this hasn't been compiled or run — please build and test on an actual Windows machine before relying on it.
- The controller-exclusivity handoff (neutralizing real input while this app has focus) uses a named Windows event, `Local\CemuToypadPickerInputActive`. This string must match the corresponding check in the Cemu fork's `Controller.cpp` exactly, or the handoff will silently stop working.
