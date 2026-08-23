# LegoToypad

<p align="center">
  <img src="Assets/Branding/Legotoypad_Logo.png" alt="LegoToypad logo" width="220">
</p>

A controller-driven companion app for LEGO Dimensions emulation. It emulates the
Toypad and all 30 franchises' tags — 78 characters and 240 vehicles across
every world — and sends them straight to the emulator, no mouse needed and no
physical toy required. Works with a **Cemu** or **RPCS3** build that has the
local Toypad listener enabled.

Every asset — figure tags, portraits, world logos, the background, the wordmark,
the app icon — is compiled into `LegoToypad.exe` itself as a Win32 resource. The
exe is fully self-contained; there are no loose `.bin`/`.png` files next to it.

## Features

- **Whole-game tag library built in**: all 30 franchises, 78 characters and
  240 vehicle builds, each with its real NFC tag data and portrait art.
- **Controller-first UI**: every screen is navigable with a gamepad — no mouse
  required. Keyboard and mouse work too.
- **True Toypad geometry**: 7 glossy pad slots matching the real 3/1/3 Toypad
  layout, with Load, Move and Clear actions per slot.
- **Rounded-corner overlay window**: hovers above the emulator, drawn with GDI+
  and rendered to an off-screen buffer for a flicker-free frame.
- **Sits in the system tray**: stays hidden until the toggle shortcut shows it,
  launching with a one-time toast notification.
- **Web remote**: a phone-friendly HTML UI is embedded in the exe and served
  over your LAN, mirroring the desktop overlay so you can Load/Move/Clear tags
  from any device on your network.
- **One-press pad shortcuts**: with a pad focused, **X** picks that figure up to
  move it, **RB** jumps straight into figure selection for it, and **LB** clears
  it — no action menu in between.
- **Story mode**: switch character selection to the starter pack only (Batman,
  Gandalf The Grey, Wyldstyle, Batmobile) and skip the series grid entirely.
- **No Background**: make the overlay's backdrop fully transparent so the game
  stays visible between the UI elements.
- **Configurable everything**: toggle shortcut, confirm-button style, background
  selection, story mode, and a rebindable button for every picker action all
  live in the in-app Settings screen.

## Screenshots

<p align="center">
  <img width="440" height="298" alt="img1" src="https://github.com/user-attachments/assets/c67b67d6-70d9-4037-982b-e0121c408770" />
  <img width="440" height="298" alt="img4" src="https://github.com/user-attachments/assets/08f13a24-78a9-4016-8b84-71457e1ab586" />
</p>
<p align="center">
  <img width="440" height="298" alt="img3" src="https://github.com/user-attachments/assets/92582bf1-fb32-4b05-b709-68f58da14aa9" />
  <img width="440" height="298" alt="img2" src="https://github.com/user-attachments/assets/5256bd92-6013-4a17-93fe-8dfaa866aec3" />
</p>

## Using the app

1. Launch the exe: it sits in the tray and stays hidden until triggered. A toast
   on first run tells you the toggle shortcut.
2. Press the toggle shortcut (default: controller **Back**) to show the overlay.
3. On the pad screen you'll see the 7 Toypad slots. A "Y / Settings" hint in the
   bottom-right corner reminds you how to reach Settings.
4. With a pad focused you can act on it in one press: **X** picks its figure up
   for a move, **RB** goes straight to figure selection, **LB** clears it.
5. Or pick a pad and confirm to get its action menu:
   - **Load** — opens the franchise grid. Pick a world, then a character or
     vehicle. Multi-build vehicles open a build picker (Build 1/2/3) before
     loading. The tag is sent to the emulator instantly.
   - **Move** — pick where the selected tag should go; both slots update in the
     emulator.
   - **Clear** — removes the tag from that pad slot.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | Move selection (pads, world grid, roster grid, menus) |
| A / Enter | Select / confirm |
| B / Escape | Back |
| Y / S | Open settings (from the pad grid) |
| X / M | Pick the focused pad's figure up and move it (from the pad grid) |
| RB / L | Load a figure onto the focused pad (straight to selection) |
| LB / C | Clear the focused pad |

Every button in this table except the D-pad can be rebound from
**Settings → Button - …**.

Any connected controller works — player 1 through 4, not just the first pad.
While the overlay is visible, a named Windows event tells the emulator to
neutralize real controller input so the picker and the game never fight over
the same pad.

## Settings

Press **Y / S** on the pad grid to open Settings:

1. **Toggle shortcut** — rebind the show/hide overlay trigger (controller combo
   or keyboard shortcut, see below).
2. **Confirm button** — swap A/B confirm style: **A (RPCS3)** or **B (Cemu)**.
3. **Background** — cycle through the bundled background images, plus a final
   **No Background** choice that makes the backdrop fully transparent so the
   game shows through between the UI elements (drop more `background*.png`
   files into `Assets/Wallpapers` and rebuild to add your own).
4. **Character selection** — **All series** (pick a world, then a figure) or
   **Story (starter pack only)**, which skips the series grid and shows just
   Batman, Gandalf The Grey, Wyldstyle and the Batmobile.
5. **Button - …** — one row per action (Confirm, Back, Settings, Move active
   pad, Quick load, Quick clear). Select a row, release every controller
   button, then press the button you want — including **LT / RT**, which the
   app reads as buttons even though the controller reports them as axes. The
   D-pad stays reserved for menu navigation, and a button already used by
   another action or by the overlay toggle is rejected with a message on the
   Settings screen. **Back+Start or Esc cancels.**
6. **Clear all pad** — empty every slot at once (useful if the emulator reset
   its Toypad state out from under the app, e.g. after an emulator restart).
7. **Web remote** — turn the phone UI on or off (see below).

## Overlay shortcut

Default toggle is the controller **Back** button. Change it from Settings → Toggle shortcut:

- **Controller combo:** release everything first, then press the new combo (LT and RT count as buttons here too). It must include at least one button the picker doesn't already use itself — the D-pad and whatever is currently bound to Confirm / Back / Settings / Move active pad / Quick load / Quick clear are all off-limits on their own, so Back, Start or a stick click are the usual picks. **Back+Start always cancels.**
- **Keyboard shortcut:** must include Ctrl, Alt, Shift, or Win. A bare key is rejected, since an unmodified global hotkey would swallow that key everywhere on your PC while the app is running. **Esc cancels.**

## System tray

Right-click the tray icon for the menu:

- **Show/Hide overlay** — same as the toggle shortcut.
- **Shortcut settings** — opens the Settings screen's shortcut row directly.
- **Copy web remote address** — copies the URL to open on your phone/tablet.
- **Exit** — quits the app and removes the tray icon.

## Web remote (phone UI)

A complete phone remote is compiled into the exe. When enabled, the app shows
`Web remote on: http://<your-ip>:8765/` in the status bar and copies that URL to
the clipboard so you can just open it on your phone. The page is a mobile-first
9:16 portrait layout (it stays at 9:16 on any sized screen): the 7 Toypad slots
keep the iconic 3/1/3 arrangement on top of a thumb-friendly action bar, and a
picked world shows a scrollable roster with the vehicle build picker — Load,
Move and Clear work from the selected slot.

- The desktop app must keep running — it is the bridge that relays your taps to
  the emulator's Toypad listener on loopback.
- Turn it off entirely from **Settings → Web remote** (default: **On**). When
  off, no server thread is created and no port is opened.
- On Windows, the first browser connection from another device may trigger a
  firewall prompt for `LegoToypad.exe` — allow it on **private** networks.
- Use it on your LAN only; nothing is encrypted and there is no authentication.
  Don't expose port `8765` to the internet.

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
The `Assets` tree is organized into category folders — `Wallpapers` (backgrounds),
`Pads` (the 7 Toypad slot art), `Tiles`, `Buttons`, `Branding` and `Fonts` — and
the generator gathers them recursively, so drop a file in the right folder and
rebuild. Legacy art in `Assets/Old` is skipped.
The script only rewrites a file when its content changes, so incremental builds
don't recompile the resources unnecessarily. Add or change a `.bin`/`.png` in
the source folders and just rebuild - no manual step.

The build fetches and links **SDL2 statically** (it is used purely as the
game-controller input library), so the final exe embeds the tag library, all
artwork and the input backend with no companion DLLs.

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
Compress-Archive -Path dist\* -DestinationPath LegoToypad-v1.3.0-win64.zip
```

The exe carries the full tag library and all images inside it - there is
nothing else to bundle.

## Release

```powershell
git add -A
git commit -m "Describe changes here"
git tag v1.3.0
git push origin main --tags
```

Then, with the `gh` CLI:

```powershell
gh release create v1.3.0 LegoToypad-v1.3.0-win64.zip `
  --title "LegoToypad v1.3.0" `
  --notes "Self-contained single exe with embedded tag library and artwork; franchise browsing with glossy pads and portraits."
```

No `gh`? Same thing works from the repo's Releases page on GitHub: Draft a new release, then attach the zip.

The exe is unsigned, so Windows SmartScreen will flag it on first launch. Tell people to click "More info" → "Run anyway," or it'll look broken to anyone downloading it cold.

## Setup

1. Run `LegoToypad.exe`. It sits in the tray and stays hidden until triggered.
2. A `LegoToypad.ini` is created next to the exe on first launch. The
   `[Listener] Port` must match the emulator's Toypad listener port — Cemu's
   `DimensionsToypadListenerPort` (default `9191`), or the corresponding RPCS3
   setting.

### `LegoToypad.ini` reference

Everything below can also be set from the in-app Settings; the file exists so
you can tweak by hand or pre-configure a deployment.

| Section | Key | Meaning |
|---|---|---|
| `[Listener]` | `Port` | TCP port to send tag messages to (default `9191`) |
| `[Shortcut]` | `Type` | `Controller` or `Keyboard` |
| `[Shortcut]` | `ControllerMask` | Raw XInput button bitmask for the toggle (Back = 32; add values to chord, e.g. LB 256 + RB 512 = 768). Must include a non-nav button |
| `[Shortcut]` | `KeyModifiers` | Keyboard modifier bitmask (Alt=1, Ctrl=2, Shift=4, Win=8) |
| `[Shortcut]` | `KeyCode` | Keyboard virtual-key code (must have a modifier) |
| `[Input]` | `SwapConfirmBackButtons` | `0` = A confirm / B back (RPCS3 style), `1` = B confirm / A back (Cemu style) |
| `[Input]` | `BackgroundIndex` | Index into the bundled `Assets/Wallpapers/background*.png` choices; one past the last one means **No Background** (transparent backdrop) |
| `[Input]` | `StoryMode` | `1` = show only the starter pack when picking a figure, `0` = all series |
| `[Input]` | `ButtonConfirm` / `ButtonBack` / `ButtonSettings` | Raw XInput button for confirm / back / open settings (A=4096, B=8192, Y=32768) |
| `[Input]` | `ButtonMoveActive` / `ButtonQuickLoad` / `ButtonQuickClear` | Raw XInput button for the one-press pad actions (X=16384, RB=512, LB=256). D-pad values are ignored and fall back to the default |
| | | The triggers extend the XInput mask: **LT=65536**, **RT=131072**. They work anywhere a button value does, including `[Shortcut] ControllerMask` |
| `[Web]` | `Enabled` | `1` = serve the phone UI / `0` = no web server at all (default `1`) |
| `[Web]` | `Port` | Port the HTTP server binds (default `8765`) |

## Listener protocol

A tiny fire-and-forget TCP protocol on loopback to the emulator's listener:

| Message | Header | Payload |
|---|---|---|
| **LOAD** | `0x01`, pad, index, `0x00`, `0x00` | 180 raw tag bytes + 2-byte little-endian path length + path |
| **REMOVE** | `0x02`, pad, index, `0x00`, `0x00` | — |
| **MOVE** | `0x03`, destPad, destIndex, srcPad, srcIndex | — |

`pad`/`index` select one of the emulator's 7 Toypad slots. Tag bytes come from
the embedded resources, and since there is no on-disk `.bin` anymore the LOAD
path length is `0` — game writes stay in the emulator's memory for the session
instead of persisting to a file.

## Notes

- Regenerate the asset table with `python generate_assets.py --root . --out build\generated` on demand if you ever want a manual refresh.
- The controller-exclusivity handoff (neutralizing real input while this app has focus) uses a named Windows event, `Local\CemuToypadPickerInputActive`. This string must match the corresponding check in the Cemu fork's `Controller.cpp` exactly, or the handoff will silently stop working.