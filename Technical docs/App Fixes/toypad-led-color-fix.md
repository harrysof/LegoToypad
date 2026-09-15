# Toy Pad LED Color: Calibration Fix + Flash Visibility Bug

## Bug 1: The orange cast — LED drive values aren't sRGB

The Toy Pad's RGB LED has very different luminous efficiency per channel, and
the game compensates. What comes over the wire is **drive levels**,
calibrated against a white point of `(255, 110, 24)` — not display color.
Paint those bytes straight to a screen and everything skews orange, because
green sits at 43% and blue at 9% of where sRGB would put them.

### Fix

Divide each channel by its calibration maximum and clamp:

```js
const LED_WHITE_POINT = { r: 255, g: 110, b: 24 };

const calibrate = ({ r, g, b }) => ({
  r: Math.min(255, Math.round(r / 255 * 255)),
  g: Math.min(255, Math.round(g / 110 * 255)),
  b: Math.min(255, Math.round(b /  24 * 255)),
});
```

Apply it at the single point where wire color becomes display color, so fade
interpolation and flash presentation inherit it automatically.

### Evidence

Portal 2 chamber 2, which should be turquoise / yellow / magenta:

| Raw | Ratios | Corrected |
|---|---|---|
| `255,110,0` | `[1.0, 1.0, 0.0]` | `#ffff00` yellow |
| `0,110,24` | `[0.0, 1.0, 1.0]` | `#00ffff` turquoise |
| `255,0,24` | `[1.0, 0.0, 1.0]` | `#ff00ff` magenta |

Exactly the three secondary colors. The odds of that falling out of a wrong
constant are negligible.

It also explains the "orange power-on glow" — `153,66,14` normalizes to
`#999995`, **neutral white at 60%**. It was never orange.

**Linearity confirmed too.** A mid-fade frame gave `76,32,0` / `0,32,7` /
`76,0,7` — the same three hues at ~30%, each channel scaling by the same
factor (0.298 / 0.291 / 0.292). So the drive is linear; brightness ramps like
Locate render correctly throughout, not just at endpoints.

### Caveat

The white point is empirical, derived from ~6 captures across different
scenes — all consistent, including neutrals and a 30% frame. If some future
frame shows an obvious hue skew, it'd mean the calibration isn't a fixed
constant. Nothing seen so far suggests that.

---

## Bug 2: Invisible flashes (separate bug)

If flash is rendered as an opacity animation on a glow layer tinted with the
LED color, it's invisible whenever the flash color matches its background —
**black-on-dark** and **white-on-white** both animate nothing. That's why
colored keystone flashes show but swap/build flashes appear to be missing
entirely. It looks like dropped events; it isn't.

### Fix

Modulate **tile brightness** (1.0 → 0.34) instead of glow opacity.
Brightness is color-independent, so any flash reads. Keep the opacity
keyframe alongside it if you like — colored flashes then look unchanged.

```js
// Instead of animating glow layer opacity only:
// glowLayer.opacity: 1.0 -> 0.0 -> 1.0  (invisible for black/white flashes)

// Modulate tile brightness as well:
tile.brightness: 1.0 -> 0.34 -> 1.0
```
