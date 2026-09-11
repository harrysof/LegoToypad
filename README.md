# LegoToypad

<p align="center">
  <img src="Assets/Branding/Legotoypad_Logo.png" alt="LegoToypad logo" width="220">
</p>

A controller-driven companion app for LEGO Dimensions emulation. It emulates the Toypad and every tag - 75 characters and 240 vehicles across all 30 franchises - and sends them straight to the emulator. No mouse, no physical toy needed.

Works with any of the emulator builds below that have the local Toypad listener enabled. Everything (tags, art, sounds) is baked into `LegoToypad.exe` - there are no loose files to manage.

Thanks to the [LEGO Dimensions Discord](https://discord.gg/PuXpBMFE4P) for support!

## Custom toypad emulator builds

LegoToypad needs an emulator build with the Toypad listener enabled. Grab the one for your emulator:

| Emulator | Build |
|---|---|
| Cemu | [Cemu-2.6-Remote-Toypad-Build](https://github.com/harrysof/Cemu-2.6-Remote-Toypad-Build) |
| RPCS3 | [RPCS3-Seamless-Toypad-Build](https://github.com/NeverCookFirst/RPCS3-Seamless-Toypad-Build) |
| shadPS4 | [shadPS4-Seamless-Toypad-Bridge](https://github.com/NeverCookFirst/shadPS4-Seamless-Toypad-Bridge) |
| Xenia | [Xenia-Seamless-Toypad-Build](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build) |

## Features

- Full tag library built in: all 30 franchises, 75 characters, 240 vehicles
- Controller-first UI, no mouse needed - or drive it entirely from the keyboard
- True Toypad layout: 7 pad slots (3/1/3), with Load / Move / Clear per slot
- Toypad sneak peek: hold a button to see the pads over the game without pausing it
- Abilities browser: browse every ability, and filter by Common, Uncommon, Exclusive, Vehicular or One-Timed
- Writable vehicle tags: in-game vehicle upgrades are saved to disk and loaded back next time
- Fast loading: choose whether loading a tag closes the picker or leaves it open
- Live Toypad LEDs mirrored from the emulator in real time (off by default)
- Swappable pad art (skins)
- Web remote: control the pads from your phone over LAN
- Xbox, PlayStation and Switch controllers supported, with matching button icons
- Fully configurable from an in-app Settings screen

## Using the app

1. Launch the exe - it sits in the tray until you toggle it (default: **Ctrl+L**, so it works with no controller plugged in).
2. Pick a pad, then **Load** a franchise/character, **Move** it, or **Clear** it.

### Toypad sneak peek

Hold **LT** (rebindable) while playing and the seven pads fade in over the game -
the two side sections in the bottom corners, the center pad up top, so the middle
of the screen stays clear. Release and they fade away.

Unlike opening the picker, the peek never takes focus and never takes the
controller: the HUD window is click-through and is never activated, so the game
keeps running and the held button still reaches it. It is the same pad state the
picker shows, live - loads, moves, clears and mirrored Toypad LEDs all appear on
it the moment they happen.

Set it to Off / Small / Medium / Large from **Settings -> Sneak peek**, and rebind
the hold button from **Settings -> Button - Sneak peek (hold)**. It needs the
emulator running in windowed or borderless mode; a true exclusive-fullscreen
window will cover it.

## Controls

| Input | Action |
|---|---|
| D-pad / stick | Move selection |
| A / Enter | Confirm |
| B / Escape | Back |
| Y / S | Settings |
| X / M | Pick up the focused pad's figure to move it |
| RB / L | Load a figure onto the focused pad |
| LB / C | Clear the focused pad |
| RB / LB | Cycle the franchise sort (world grid / browse rosters) |
| LT (hold) | Sneak peek: show the pads over the game |

Every binding except the D-pad can be rebound from Settings.

### Keyboard controls

The whole picker is usable with just a keyboard - no controller required. Open
**Settings -> Keyboard layout** for a visual diagram of every key.

| Key | Action |
|---|---|
| **Ctrl + L** | Show / hide the picker (global toggle, always active) |
| Arrow keys | Move selection |
| Enter | Confirm |
| Esc | Back |
| S | Settings |
| M | Move the focused pad's figure |
| L | Load a figure onto the focused pad |
| C | Clear the focused pad |
| G | Toggle the LED demo |
| `[` / `]` | Previous / next franchise sort (on the world grid / browse rosters) |

Ctrl+L is the default toggle and stays active even after you set a controller
shortcut, so both work at once. Change the toggle from **Settings -> Toggle
shortcut**.

## Setup

1. Run `LegoToypad.exe`.
2. On first launch it creates a `LegoToypad.ini` next to itself. Make sure `[Listener] Port` matches your emulator build's Toypad listener port (default `9191`).

## Build (from source)

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

## Notes
- The exe is unsigned, so Windows SmartScreen will flag it on first launch. Click "More info" then "Run anyway."
