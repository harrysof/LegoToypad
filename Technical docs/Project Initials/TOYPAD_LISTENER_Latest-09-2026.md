# LEGO Dimensions Toypad Listener

This document explains how the shadPS4 LEGO Dimensions emulation talks to the
external **LegoToypad** app over a local TCP connection. It covers what the two
programs are, how to turn the feature on, and the message format they use, with
enough detail to build a compatible tool but without diving into the emulator's
internals.

## The idea

A real LEGO Dimensions toy pad is a USB gadget. When you place a figure on it,
the pad reads the figure's NFC tag and passes it to the game, and the game lights
up the pad's three LED zones. shadPS4 emulates that USB device in software.

The emulator build described here adds a **loopback listener**: a small TCP
server that lets a companion app act as the physical toy pad. The companion app
is **LegoToypad**, which shows a visual toy pad, lets you drop characters and
vehicles onto its seven slots, and mirrors the game's LED colours back onto the
screen.

In short:

- **shadPS4** runs the game and hosts the listener (the TCP **server**).
- **LegoToypad** is the desktop app you click around in (the TCP **client**).
- Both run on the same machine and talk over `127.0.0.1`.

```
 +----------------+         TCP 127.0.0.1:9191          +------------------+
 |    LegoToypad  |  ----- LOAD / REMOVE / MOVE ---->   |      shadPS4     |
 |   (companion)  |  <----  GET_LED snapshot   ------    |  Dimensions      |
 |                |                                     |  Toypad backend  |
 +----------------+                                     +------------------+
                                                               |
                                                          guest game
```

## Turning it on

### On the shadPS4 side

The listener is only started when the LEGO Dimensions USB backend is selected
and a port is configured. In `config.toml` under `[Input]`:

```toml
usbDeviceBackend     = "DimensionsToypad"
dimensionsListenerPort = 9191
```

- The default port is **9191**.
- Setting the port to `0` disables the listener.
- The socket binds to `127.0.0.1` only, so nothing outside the local machine can
  reach it. There is no authentication or encryption, which is fine for a local
  bridge but means you should not expose the port to a network.

### On the LegoToypad side

LegoToypad reads its settings from `LegoToypad.ini`:

```ini
[Listener]
Port=9191
```

The two port values must match. LegoToypad connects to `127.0.0.1` on that port,
sends a command, then closes the connection. It reconnects for the next command,
so you can start or restart either program independently without a long-lived
session to renegotiate.

## The toy pad layout

The pad has **three zones** and **seven slots**:

- **Pad 1 = centre**, **pad 2 = left**, **pad 3 = right**.
- Slots are numbered `0` to `6`.

A slot is addressed by a `(pad, slot)` pair. The three LED zones map to the same
pads: left zone = pad 2, centre zone = pad 1, right zone = pad 3.

## Figures and tags

Each character or vehicle is stored as a **180-byte tag blob** (the same size a
real NFC tag presents to the game). A tag can be:

- **Built in** - shipped inside the LegoToypad executable as a resource.
- **On disk** - a `.bin` file the app reads and sends.
- **Session-only** - sent with no file path, living only in the emulator's
  memory until the slot is cleared.

When a LOAD includes a real file path, shadPS4 keeps that file open read/write.
If the game writes to the tag (some vehicles store progress this way), the change
is saved back to the `.bin` automatically.

## The TCP protocol

Every message starts with the same **5-byte header**:

| Byte | Meaning                                                   |
|------|-----------------------------------------------------------|
| 0    | Command                                                   |
| 1    | Pad (1-3) - destination for LOAD/REMOVE, dest for MOVE    |
| 2    | Slot (0-6) - destination for LOAD/REMOVE, dest for MOVE   |
| 3    | Source pad (MOVE only, otherwise 0)                       |
| 4    | Source slot (MOVE only, otherwise 0)                      |

### Commands

| Code | Name    | Purpose                                             | Extra data                          |
|------|---------|-----------------------------------------------------|-------------------------------------|
| 0x01 | LOAD    | Place a figure into a slot                          | 180 tag bytes + path length + path  |
| 0x02 | REMOVE  | Clear a slot                                        | none                                |
| 0x03 | MOVE    | Move/swap a figure between slots                    | none (source in header bytes 3-4)   |
| 0x04 | GET_LED | Ask for the current LED state                       | none                                |

### LOAD payload

After the 5-byte header:

1. **180 bytes** of raw tag data.
2. A **2-byte little-endian length**.
3. That many bytes of **UTF-8 file path**.

A zero-length path means "keep this tag in memory only". A non-empty path tells
the emulator to persist game writes back to that file.

If the destination slot is already occupied, the emulator removes the old figure
first, waits about 100 ms so the game observes the removal, then loads the new
one. This makes hot-swapping a figure look natural to the game.

LOAD, REMOVE, and MOVE are fire-and-forget. A single connection may carry more
than one message, but LegoToypad normally opens one connection per command.

### GET_LED reply

GET_LED is the only request that gets a response. The emulator replies with
**40 bytes**:

| Offset | Meaning                                      |
|--------|----------------------------------------------|
| 0      | `'L'` (0x4C) - reply marker                   |
| 1      | Serial / change counter                      |
| 2      | Protocol version (**2**)                      |
| 3      | Region count (**3**)                          |
| 4..    | Three 12-byte region records                  |

Each 12-byte region record is:

| Field       | Meaning                                          |
|-------------|--------------------------------------------------|
| pad         | Which zone this is (1 centre, 2 left, 3 right)   |
| mode        | `0` off, `1` solid, `2` flash, `3` fade          |
| r, g, b     | Target colour                                     |
| from_r/g/b  | Colour the fade started from                      |
| on_ms       | Flash on-time                                     |
| off_ms      | Flash off-time                                    |
| count       | Number of flashes                                 |
| speed_ms    | Fade speed                                        |

LegoToypad polls this roughly every 33 ms and animates the result on screen, so
the app's LEDs follow whatever the game is doing - centre, left, and right zones
independently. During a colour transition the `from_*` values let the app show a
smooth cross-fade instead of a hard jump.

Protocol version note: **version 2** added the `from_r/g/b` fields, growing the
response from 30 to 40 bytes. The app checks the version byte and falls back to
the older 30-byte layout if needed.

## Alternative: the IPC bridge

The same emulator build also exposes a text-based IPC bridge over stdin/stdout
for launchers. It is disabled unless the environment variable
`SHADPS4_ENABLE_IPC=true` is set. It offers the same figure operations
(`USB_LOAD_FIGURE`, `USB_REMOVE_FIGURE`, `USB_MOVE_FIGURE`, plus the temporary
remove/cancel variants) and emits LED state as `;LED_STATE ...` lines. This is a
different, optional integration path; the TCP listener is the one LegoToypad
uses.

## Don't confuse this with the web remote

LegoToypad also runs its own **HTTP server on port 8765** (configurable under
`[Web]`) so a phone or browser can control it. That is the opposite direction:
here LegoToypad is the **server** and the browser is the client. It is unrelated
to the shadPS4 toypad protocol described above. If a firewall prompt or a port
conflict appears, check which port is which.

## Troubleshooting

- **Nothing happens when placing a figure.** Check that
  `usbDeviceBackend = "DimensionsToypad"` is set in shadPS4's `config.toml` and
  that the game is actually running with that config.
- **Connection refused.** The listener may be disabled (port `0`) or still
  starting. Confirm both programs use the same port (default 9191).
- **"Why is my real controller also moving things?"** LegoToypad and the emulator
  coordinate input ownership through a named event
  (`Local\CemuToypadPickerInputActive`) so the pad UI can temporarily take over
  controller input while its picker is open.
- **LEDs don't match the game.** Make sure LED mirroring is enabled in the app
  (it is off by default) and that the poll thread is running.

## Developer notes

For those extending this, the relevant pieces are:

- Emulator listener: `src/core/libraries/usbd/emulated/dimensions_listener.cpp`
  and `.h` (protocol comments at the top of the header).
- Emulator device/LED logic: `src/core/libraries/usbd/emulated/dimensions.cpp`
  and `.h`.
- Port setting: `src/core/emulator_settings.h` (`dimensions_listener_port`).
- Companion client: `main.cpp` (wire constants near the top, send helpers around
  line 3962, LED poll around line 7073).

The protocol is intentionally small: one 5-byte header, one binary payload form,
and one fixed-length reply. Keep any additions backward compatible by bumping the
version byte and letting clients ignore unknown fields.
