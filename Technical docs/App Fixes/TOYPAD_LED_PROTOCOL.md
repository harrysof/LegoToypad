# LEGO Dimensions Toy Pad LED protocol — implementation notes

Written so this LED handling can be ported to another emulator (or another
front-end) without re-deriving it from scratch. Covers three layers:

1. The **real hardware protocol** (what the actual Wii U game sends over USB
   HID — this part is emulator-agnostic and always applies).
2. How **this Cemu build** receives and models that (`Dimensions.cpp`).
3. The **local mirror protocol** this project invented to hand that state to
   an external renderer (`DimensionsNetworkListener.cpp` /
   `LegoToypad/main.cpp`), and the animation math that makes the renderer's
   output match real hardware.

Only §1 is required reading to port this to a different emulator's HID layer.
§3 is only relevant if you also want a separate renderer process talking over
a socket the way this project does.

---

## 1. Real hardware protocol

Source: the game always talks to the toy pad using 32-byte USB HID reports.
Verified against two independently-authored reverse-engineering efforts
([`Ellerbach/LegoDimensions`](https://github.com/Ellerbach/LegoDimensions/blob/main/LegoDimensionsProtocol.md),
[`AlinaNova21/node-ld`](https://github.com/AlinaNova21/node-ld)) which agree
byte-for-byte.

Every message: `[0x55, length, command, messageId, ...payload, checksum, 0-padding to 32 bytes]`.
Payload starts at byte 4. `pad`: 0 = all pads, 1 = center, 2 = left, 3 = right.

| Command | Payload (from byte 4) | Notes |
|---|---|---|
| `0xC0` Color | `pad, r, g, b` | Immediate solid color. |
| `0xC1` GetPadColor | `pad` | Reply: `[msgId, r, g, b]`. Query only. |
| `0xC2` Fade | `pad, tickTime, tickCount, r, g, b` | See "Fade" below — **not a brightness ramp**. |
| `0xC3` Flash | `pad, tickOn, tickOff, count(0xFF=forever), r, g, b` | Real on/off blink of one color against dark. |
| `0xC4` FadeRandom | `pad, tickTime, tickCount` | Same as Fade, but the portal firmware itself picks the target color — the commanding side never learns which. |
| `0xC6` FadeAll | 3× `[onOffFlag, tickTime, tickCount, r, g, b]` (center, left, right) | Same fade semantics per region. |
| `0xC7` FlashAll | 3× `[onOffFlag, tickOn, tickOff, count, r, g, b]` | Same flash semantics per region. |
| `0xC8` ColorAll | 3× `[onOffFlag, r, g, b]` | `onOffFlag`: 0 = off, 1 = on. |

`tick` is an undocumented-length time unit; nothing published states its
exact duration. This project measured **~40 ms/tick** by capturing a real
LEGO Dimensions trace: the game's idle rainbow effect issues a Fade All with
`tickTime = 0x1E` (30) and re-issues the next color ~1.2 s later, and
1.2 s / 30 ≈ 40 ms lines up with the fade filling exactly that interval.
Treat this as a calibrated estimate, not a spec value.

### Flash — the easy one

Exactly what it sounds like: the given `r,g,b` is shown for `tickOn` ticks,
then the pad goes dark for `tickOff` ticks, repeating `count` times
(`0xFF` = forever). No cleverness needed — one color, blinking against off.

### Fade — the one everyone gets wrong

The docs are explicit and mutually corroborating on this:

> "This command fade[s] a pad. It will take the current color as the
> original color... tick count will start with the old color and move to
> the new one then move to the new one to the old one. Meaning that odd
> numbers will finish on the new color."

**Fade does not ramp one color's brightness.** It cross-fades — alternates —
between whatever color the pad was already displaying (the "from" color)
and the new target (the "to" color) given in the command:

- Step 0: from → to
- Step 1: to → from
- Step 2: from → to
- ... `tickCount` steps total, each `tickTime` ticks long.
- `tickCount == 0` means "repeat forever" (never settles).
- Otherwise, after `tickCount` steps it **stops** on whichever color the
  parity dictates: odd count → ends on the new (`to`) color, even count →
  ends back on the old (`from`) color.

Concretely: pad currently red, game sends `Fade(pad, speed, count=1, blue)`
→ pad crossfades red→blue once and stays blue. `count=2` → red→blue→red,
ends back on red. `count=0` → crossfades red↔blue forever.

**The naive, wrong implementation** (which is what this codebase originally
had, and probably what most first-pass emulators do) treats Fade as "ramp
brightness from wherever it is now up to full brightness of the new color,
then hold solid" — i.e. it discards the *old hue* entirely and only ever
shows the *new* one, varying its brightness. This looks superficially
plausible (it does look like "something is fading") but is visibly wrong the
moment the old and new colors differ: real hardware shows both colors
alternating; the naive version only ever shows one.

`FadeRandom` is the same alternation, except the portal's own RNG (not the
game) picks the target color, so there's no way to reproduce the *specific*
hue it would land on — only the fact that it's a second, different color
from whatever was already showing.

---

## 2. This emulator's model (`Dimensions.cpp`)

`DimensionsUSB::SendCommand` intercepts the game's real HID writes (this is
the part that's genuinely emulator-specific — whatever your HID/USB HLE
layer looks like, this is the hook point) and routes `0xC0`–`0xC8` to
`HandleLedCommand`, which parses the payload per the table above into a
`LedPadState`:

```cpp
struct LedPadState
{
    uint8 pad = 0;
    uint8 mode = 0;              // 0 off, 1 solid, 2 flash, 3 fade
    uint8 r = 0, g = 0, b = 0;             // current/target color
    uint8 fromR = 0, fromG = 0, fromB = 0; // color to fade FROM (mode 3 only)
    uint8 onMs = 0, offMs = 0, count = 0, speedMs = 0; // still tick units, not ms — named for historical reasons
};
```

The critical bit for an accurate port: **when a Fade command lands,
`SetLedState` snapshots the pad's pre-command `r/g/b` into `fromR/fromG/fromB`
before overwriting `r/g/b` with the new target.** This is what lets a
downstream renderer reproduce the real alternation instead of only ever
knowing the newest color. Do this capture centrally (one place all fade-family
commands funnel through), not per-command-handler, so `Fade`, `FadeAll`, and
`FadeRandom` all get it for free.

For `FadeRandom`, generate an actual random `r/g/b` here (this build uses a
`std::mt19937` seeded independently of the NFC crypto RNG, so it never
perturbs that unrelated sequence) rather than leaving the color unchanged —
leaving it unchanged means the pad silently does nothing, which is a bigger
visible wrong than picking a plausible-but-inexact random hue.

Three bytes of state per pad is enough; you do **not** need to track
animation phase/elapsed time in the emulator core. That belongs in whatever
renders the LEDs, timestamped from the moment it *receives* the state change
— see §3.

---

## 3. This project's renderer bridge (optional — only if you also split emulator/renderer across a socket)

If your renderer lives in-process with the emulator, skip the wire format
below and just pass `LedPadState` (plus a wall-clock timestamp of when it
last changed) directly to whatever draws the LEDs, and implement
§3's animation math (`ComputeLedFrame`) directly against that struct.

This project renders the toy pad in a **separate process**
(`LegoToypad/main.cpp`, a Win32 GDI+ app) that polls the emulator over a
loopback TCP socket, because the emulator and the visualizer are independent
projects with independent release cycles. If you're doing the same thing:

### Wire format (v2)

Client → server, 5 bytes: `[0x04, 0, 0, 0, 0]` (`GET_LED`; the last 4 bytes
are unused padding shared with the LOAD/REMOVE/MOVE commands' header shape).

Server → client, 40 bytes:

```
[0] 0x4C ('L' magic)
[1] serial (increments on any real state change; lets a poller skip re-applying an unchanged snapshot)
[2] format version (2)
[3] region count (3)
then 3 regions × 12 bytes, each:
  pad, mode, r, g, b, fromR, fromG, fromB, onMs, offMs, count, speedMs
```

**Put a version byte in your handshake.** Version 1 of this project's mirror
was 30 bytes (no `fromR/G/B`) — exactly the naive single-hue Fade bug from
§1, baked into the wire format. When the fix required widening the payload,
the version byte is what lets a stale client (or stale server) notice the
mismatch and ignore the packet instead of misreading 3 extra bytes/region as
garbage. Bump it again if the layout changes further.

### Change detection: compare wire-against-wire, never against animation state

The serial is **global**, not per-region: any pad's command bumps it, so
every serial change makes the client re-examine all three regions and decide
per-region whether anything actually changed. Getting that comparison wrong
is subtle and this project shipped it wrong once, so it is worth stating
plainly:

**Keep a copy of the last wire snapshot you applied per region, and compare
the incoming snapshot against that copy.** Do not compare against your live
animation state.

The reason is that the emulator stores *commands*, not *animation progress* —
it has no clock and will keep reporting `mode = fade, count = 1` forever,
long after that fade finished. A renderer, meanwhile, necessarily mutates
its own state as the animation runs: a finished fade settles to solid, a
finished flash settles to off, a settled fade folds its final blended colour
into its base colour. So the two representations legitimately diverge the
moment anything completes, and a comparison between them mismatches
permanently from then on. Combined with the global serial, that means the
next command to *any* pad re-applies and visibly restarts every animation
that had already finished elsewhere.

The same trap catches any field the two sides normalise differently. The
`fromR/G/B` bytes are only meaningful for fades, so it is tempting for a
renderer to "tidy" them for the other modes (e.g. set `from = to` for a
solid colour). Don't — if those bytes are part of the comparison, rewriting
them desyncs the region from the snapshot it came from and every later poll
sees a phantom change. Store what the wire said, verbatim, and let the
renderer simply ignore the fields that don't apply to the current mode.

### Animation math (`ComputeLedFrame` in `main.cpp`)

The renderer, not the emulator, owns elapsed-time tracking. On each
`ApplyLedCommand` (i.e. each time a poll shows the state actually changed),
record `cycleStart = now`. Every repaint tick, recompute from
`elapsed = now - cycleStart`:

- **Off** → intensity 0.
- **Solid** → intensity 1, draw `r/g/b`.
- **Flash** → `period = (onTicks + offTicks) * msPerTick`; if a finite
  `count` has elapsed, settle to Off. Otherwise blink 1/0 intensity by phase
  within the period, always drawing `r/g/b`.
- **Fade** → intensity is always 1 (a fade never dims to black — only its
  *hue* moves). `step = speedTicks * msPerTick`; `stepIndex = elapsed / step`
  picks which leg of the alternation you're in (even = from→to, odd =
  to→from); `localT = (elapsed % step) / step` is progress within that leg.
  If a finite `count` has elapsed, clamp to the last leg at `localT = 1` —
  this is what makes it land exactly on the odd/even endpoint the real
  hardware would. Blend `from` and `to` by (an eased) `localT` to get the
  color to draw this frame, and once settled, fold that color into
  the stored `r/g/b` and switch the mode to Solid (so re-applying an
  unchanged-looking command later isn't suppressed as a no-op, and so Solid
  keeps drawing the correct settled color even when it settled on `from`).

**Quantize the fade blend fraction** to a small fixed number of steps (this
project reuses the same `kLedIntensityLevels` bucket count already used for
flash/solid glow-bitmap caching, currently 9) before computing the blended
color, rather than using the continuous `localT` directly. If you cache any
per-color rendering (glow bitmaps, etc.) keyed by exact RGB, an unquantized
continuous crossfade produces a new unique color practically every frame,
and an infinite (`count == 0`) fade never stops generating new colors —
unbounded cache growth for as long as that pad keeps fading. Quantizing
bounds the distinct colors (and therefore cache entries) to the same count
as before the fade fix, per pad, per from/to pair.

---

## Summary for a from-scratch port

1. Parse `0xC0`–`0xC8` per the table in §1 — this part is settled, sourced,
   and won't change.
2. Model each pad as `{mode, to-color, from-color, timing fields}`; capture
   `from-color` from the pad's *previous* state whenever a Fade-family
   command lands.
3. Animate Fade as a **discrete alternating crossfade** between `from` and
   `to`, not a brightness ramp of `to` alone. Land exactly on the
   count-parity-correct endpoint when finite.
4. Animate Flash as a plain on/off blink of `to` against dark — no
   alternation needed there, that part was already easy to get right.
5. If state crosses a process boundary, version the wire format; the
   from-color is exactly the kind of field a first pass omits and a later
   fix has to add without breaking whichever side updates first.
