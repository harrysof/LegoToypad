# LEGO Dimensions Toypad Emulation — Plain-Language Explainer

> **Audience**: A non-programmer project owner who needs to understand what the Toypad emulation does, where the key pieces live, and what a realistic follow-up project looks like.

---

## What Does the Toypad Emulation Do?

### The Real Hardware

The real LEGO Dimensions Toypad is a small USB platform with three illuminated pads. You place NFC-tagged LEGO minifigures and vehicles on it, and the Wii U game detects which characters are present and where they're standing. The game also sends lighting commands back to the pad — for example, during a "Locate Keystone" puzzle, specific pads glow different colors to tell you where to move a figure.

### The Emulation (An Analogy)

Think of Cemu's Toypad emulation as a **fake post office** sitting between the LEGO Dimensions game and the user:

- The **game** thinks it's sending letters to a real Toypad and receiving mail back. It doesn't know the Toypad is virtual.
- The **fake post office** (Cemu's nsyshid system) intercepts every letter, processes it according to the real Toypad's rules, and sends back the correct reply.
- Instead of placing a physical LEGO figure on a physical pad, the **user loads a small file** (a `.bin` file that contains a digital copy of the NFC chip inside a real figure) onto one of 7 virtual "slots."

When the user loads a figure file, the emulation announces to the game: "Hey, a new figure just appeared on pad 2, slot 1 — here's its identity badge." The game then reacts exactly as if a real figure was placed on a real pad.

---

## Where Does the Code Live?

### The "File Loading" Logic

When you click "Load" in Cemu's Emulated USB Devices window and select a `.bin` file, the following happens:

1. **The GUI** (`src/gui/wxgui/EmulatedUSBDevices/EmulatedUSBDeviceFrame.cpp`) opens a file picker dialog filtered to `.bin` files.
2. It reads the entire file — exactly 180 bytes — into memory. That's the complete digital snapshot of the NFC chip inside a LEGO Dimensions figure.
3. It calls a function called `LoadFigure` in the Toypad backend, passing: the 180 bytes, a file handle (so changes can be saved back), which pad (left/center/right), and which slot (0–6).
4. The backend (`src/Cafe/OS/libs/nsyshid/Dimensions.cpp`) stores the figure data, decrypts the figure's identity number, and queues up a "figure appeared!" event for the game.

There's also a "Create" button that generates a brand new `.bin` file for any character or vehicle from a dropdown list, so you don't need to own a physical figure.

### The "Talking to the Game" Logic

The core protocol handler lives in `src/Cafe/OS/libs/nsyshid/Dimensions.cpp`. It does two things:

**Receiving commands from the game** (via HID Write): The game sends 32-byte messages to the virtual pad. The emulation parses each message — commands like "wake up," "read page 5 of the tag in slot 3," "write data to a tag," "set pad 1 to blue" — and constructs the correct reply.

**Sending events/replies to the game** (via HID Read): The game continuously asks "anything new?" via HID reads. The emulation responds with either:
- A reply to a previous command (e.g., "here are the 16 bytes you asked for from that tag")
- A notification that a figure was added or removed (automatically queued whenever the user loads/clears a figure)

### LED/Lights Direction

The game sends color/fade/flash commands (like "set pad 2 to red" or "flash all pads white"). The emulation acknowledges them and now also records the resulting LED state for each zone. The companion app polls that state and animates the pad on screen (see `TOYPAD_LED_PROTOCOL.md`). The lighting is cosmetic; gameplay mechanics don't depend on it.

---

## Where Are the Key Files?

| What | Where | One-sentence summary |
|------|-------|---------------------|
| Toypad protocol & crypto | `src/Cafe/OS/libs/nsyshid/Dimensions.cpp` | The brain — handles every command, encrypts/decrypts, manages figure slots |
| Toypad class definitions | `src/Cafe/OS/libs/nsyshid/Dimensions.h` | Defines the data structures for figures and the toypad |
| Device registration | `src/Cafe/OS/libs/nsyshid/BackendEmulated.cpp` | Tells Cemu to create a virtual Toypad when the config says so |
| HID routing | `src/Cafe/OS/libs/nsyshid/nsyshid.cpp` | Routes the game's USB calls to the right virtual device |
| GUI for figure management | `src/gui/wxgui/EmulatedUSBDevices/EmulatedUSBDeviceFrame.cpp` | The window where you load/create/move/clear figures |
| Config toggle | `src/config/CemuConfig.h` | The "Emulate Dimensions Toypad" on/off switch |

---

## Companion App (as built)

The follow-up was realised as a two-part system rather than an Android app:

1. **An in-emulator listener** — a loopback-only TCP server that accepts figure LOAD/REMOVE/MOVE messages and answers `GET_LED` for LED state.
2. **A standalone companion app** (a Windows desktop app) that shows the seven pad slots, lets you pick a figure with a game controller, sends it to the listener, and mirrors the game's LED colours on screen.

It works because `LoadFigure` accepts raw figure data plus a pad/slot and does everything else automatically — it doesn't care whether the data came from the emulator's file dialog or a socket.

The bridge, configuration, and wire protocol are documented in `TOYPAD_LISTENER_Latest-09-2026.md`; the LED mirror protocol and animation math are in `TOYPAD_LED_PROTOCOL.md`.

### What Doesn't Need to Change

The HID protocol layer, the game communication, the crypto, and the command parsing are all untouched. The companion app is a separate process that speaks only the socket protocol — it does not read emulator memory or link against the emulator.

---

## Caveats

**On data validation**: Cemu performs no validation, checksums, or integrity checks when loading a `.bin` figure file — it accepts any 180 bytes as a valid tag dump. The cryptographic elements (TEA encryption of figure IDs, password generation) exist within the emulation layer for correct protocol responses to the game, not as a security barrier for file loading.

**On legal considerations**: The `.bin` files used by this emulation are raw NFC tag dumps that contain data structured by LEGO/TT Games. Dumping these from physical figures you own, and creating new tags with known figure IDs (as the "Create" feature does), exists in a gray area. Figure IDs and character names are intellectual property of their respective holders. Users should be aware that distributing dump files of commercial figures may raise copyright or terms-of-service concerns, although the emulator itself deals only in the technical mechanics of the protocol. This is stated factually, not as legal advice.
