# LEGO Dimensions / LEGO engine audio mixing bug — Cemu source analysis

Companion to the LEGO Dimensions audio-bug write-up and the `Audio Fix/`
patch set (both in the Cemu fork). This revision replaces the original
analysis: the AUX-zeroing theory below turned out not to be the dominant
cause. The dominant cause is a stereo downmix bug that drops the Center
channel, where this engine puts most dialogue and gameplay SFX.

- **Tree analysed:** this repo (`Cemu-2.6-Remote-Toypad-Build`), base commit
  `b044ad5`. The audio code was unmodified stock Cemu 2.6 at that commit, so
  the findings below apply upstream too, and to issue
  [#1303](https://github.com/cemu-project/Cemu/issues/1303).
- **Status:** three fixes implemented, applied, and built successfully
  (see `Audio Fix/`). Not yet confirmed against real LEGO Dimensions
  gameplay — that comparison against an unmodified build is the remaining
  step.

---

## Summary

Cemu mixes every voice into six internal TV buses (L, R, SL, SR, FC, LFE)
regardless of output mode. When the emulated title's TV output mode is
**Stereo** — which is what LEGO Dimensions uses — Cemu's final downmix step
only ever copied buses 0 and 1 (L/R) into the two-channel output and threw
the other four buses away completely, silently.

The LEGO engine routes a large share of gameplay dialogue and SFX through
the **Center (FC)** bus rather than L/R. Music is mixed as ordinary L/R
stereo. So: music survives at full level, and anything routed through
Center is deleted outright. That is the reported symptom — quiet dialogue,
quiet stud pickups and hit sounds, normal music, normal cutscenes (which
apparently use a different, non-positional stream).

A second, real but secondary bug compounds this: Cemu also discards the
AUX/reverb-send portion of every voice unconditionally (see "Secondary bug"
below). And a third, unrelated correctness bug was found in the per-voice
low-pass filter while tracing this code.

---

## Primary cause: stereo downmix drops Center and Surround

### The bug

`src/Cafe/OS/libs/snd_core/ax_out.cpp`, the `AX_MODE_STEREO` branch of the
TV DMA output function, read only two of the six always-populated TV buses:

```cpp
else if (__AXMode[AX_DEV_TV] == AX_MODE_STEREO)
{
    sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
    sint32* inputChannel1 = __AXTVBuffer48.GetPtr() + numSamples * 1;
    ...
    dmaOutputBuffer[0] = ...*inputChannel0...;
    dmaOutputBuffer[1] = ...*inputChannel1...;
    ...
}
```

`__AXTVBuffer48` is always allocated at `AX_TV_CHANNEL_COUNT` (= 6) channels
wide (`ax_out.cpp`), and the 6-channel (`AX_MODE_6CH`) branch right above
this one already reads all six. The stereo branch never read buses 2-5
(SL, SR, FC, LFE) — it simply dropped them on the floor with no clamp, no
mix-down, nothing.

### The fix

`Audio Fix/patches/003-lego-audio-source-fix.patch` combined with
`Audio Fix/tools/apply-lego-audio-audio-fix.py` rewrites the stereo branch
to fold Center and the matching surround channel into L/R at -3 dB
(equal-power fold-down):

```
L_out = L + 0.707*FC + 0.707*SL
R_out = R + 0.707*FC + 0.707*SR
```

A diagnostic switch, `CEMU_LEGO_TV_FC_DIAG=1`, instead routes Center alone
to both output channels, for isolating whether a given sound is in fact
carried on the Center bus during testing.

### Why this fits every observed symptom

| Symptom | Explanation |
|---|---|
| Gameplay dialogue, stud pickups, hit sounds too quiet | Routed on the Center bus, which was discarded entirely in stereo output |
| Music unaffected | Ordinary L/R stereo bus, always copied through |
| Cutscene dialogue normal | Separate non-positional stream, not routed through Center |
| Affects LEGO Movie, Jurassic World, City Undercover too | Same middleware, same bus routing convention |
| DirectSound / XAudio2 / Cubeb and mono/stereo/surround backend choice all identical | The loss happens in HLE mixing, above any audio backend |
| Balance bug, not missing audio | Center-bus content plays correctly when the TV mode is 6-channel; only the stereo downmix path was broken |

---

## Secondary bug: AUX sends are zeroed before they return to the main bus

This was the original leading theory and is still real, just not the
dominant cause of the reported symptom.

### The stub

`src/Cafe/OS/libs/snd_user/snd_user.cpp:1149`

```cpp
void AXFXMultiChReverbCallback(AUXCBSAMPLEDATA* auxSamples, AXFXMultiChReverbData* reverbHi, AXAUXCBCHANNELINFO* auxInfo)
{
    // todo - implement me
    __UnimplementedFXCallback(auxSamples, auxInfo->numSamples, true, true, true, true, true, true);
}
```

The six `true` arguments are `clearCh0..clearCh5`. `__UnimplementedFXCallback`
writes `0` over every sample on each flagged channel. `AXFXReverbHiCallback`
does the same for channels 0-2. And when **no** aux callback is registered
at all, `AXAux_Process` (`ax_aux.cpp`) memsets the aux output buffer anyway —
so the loss is unconditional, not limited to titles that register a stubbed
effect.

### The full signal path

| # | What happens | Location |
|---|---|---|
| 1 | Each voice is mixed into 4 TV buses — bus 0 = dry, buses 1-3 = aux sends | `ax_mix.cpp` `AXVoiceMix_MixIntoBuses` |
| 2 | Buses 1-3 are copied into the aux **input** buffers | `ax_mix.cpp` `AXMix_process` → `AXAuxMix_StoreAuxSamples` |
| 3 | The registered TV aux callback runs — the stub above zeroes the buffer, or the buffer is memset if no callback exists | `ax_aux.cpp` `AXAux_Process` |
| 4 | The now-silent aux **output** is mixed back into the main bus at `__AXTVAuxReturnVolume` (unity) | `ax_mix.cpp` `AXMix_mergeTVBuses` |

### The fix

`Audio Fix/patches/001-lego-audio-aux-fallback.patch` adds
`AXAux_GetRawInputBuffer` and, when no aux callback output exists, mixes the
raw (dry) aux input back into the output at unity gain instead of silence.
This is enabled by default; set `CEMU_LEGO_AUDIO_AUX_FALLBACK=0` to disable
it. It is a dry passthrough, not a real reverb implementation — it restores
the aux-routed signal's *level* without its intended effect.

Note also that `AXAuxMix_StoreAuxSamples` right-shifts by 8 and the return
path left-shifts by 8, so even this passthrough round-trips with 8 bits of
precision loss — minor, but relevant to any future real reverb
implementation.

---

## Third, unrelated bug found while tracing this: LPF sign/scaling error

`AXVoiceMix_ApplyLowPass` (`ax_mix.cpp`) had an inverted feedback sign and an
extra, incorrect division when converting its persisted filter state back
and forth between fixed-point and float:

```cpp
// before
float prevSample = ... * 256.0f / 32767.0f;
sampleData[i] = a0 * sampleData[i] - b0 * prevSample;   // wrong sign
...
internalShadowCopy->lpf.yn1 = ... (prevSample / 256.0f * 32767.0f);  // extra *32767 undoes the /32767 above incorrectly
```

This distorted the output of any voice using the LPF (`lpf.on` set), and
compounded per frame since the corrupted `yn1` state carries forward. Fixed
in the same patch set to use the correct feedback sign and consistent
fixed-point scaling, with the restored state clamped to `[-32768, 32767]`.

---

## GamePad theory: ruled out

Issue #1303's author (m0420) hypothesised that voice audio is redirected to
the GamePad speaker instead of the main output. The source does not support
this.

**Terminology correction:** in the `AX` API, `Rmt` means **Wii Remote
speaker**, not the GamePad. The GamePad is `DRC`.

Concretely:

- **`AXSetVoiceRmtMix` and `AXSetVoiceRmtOn` do not exist anywhere in
  Cemu** — no implementation, no export registration.
- **`AXIsValidDevice` (`ax_voice.cpp`) has a copy-paste bug**: it checks
  `device == AX_DEV_TV` where it plainly means `AX_DEV_RMT`, so
  `AX_DEV_RMT` always falls through to `return -1`.
- **TV and DRC mixes are computed independently from the same source
  samples**, not switched between — there is no path by which DRC/RMT
  output can steal signal from the TV mix.
- **`_MIXUpdateDRC` and `_MIXUpdateRmt` (`snd_user.cpp`) are empty `// todo`
  stubs**, so nothing is routed to the GamePad at all.

This is also consistent with the negative result reported by TheOneQGuy —
enabling GamePad audio changed nothing, because the theory was wrong, not
because the test was too coarse.

---

## Other gaps found, not yet acted on

### `AXSetAuxAReturnVolume` / `B` / `C` are not implemented

Only the generic `AXSetAuxReturnVolume` exists (`ax_aux.cpp`). The per-bus
aliases are absent from the entire source tree. Aux return volumes stay
pinned at unity, which would make aux returns too *loud* once the aux path
carries real signal — not a cause of the reported symptom, but relevant to
any future proper reverb implementation.

### Uninitialised stack data passed to `AXSetVoiceDeviceMix`

`src/Cafe/OS/libs/snd_user/snd_user.cpp`, inside `MIXUpdateSettings`:

```cpp
// TODO remote mix
AXCHMIX2 mix[4];
for (int j = 0; j < 4; ++j)
{
    AXSetVoiceDeviceMix(voice, AX_DEV_RMT, i, (snd_core::AXCHMIX_DEPR*)mix);
}
```

`mix` is never initialised, `j` is unused, and the device index passed is
`i`, leaked from the preceding DRC loop (always `2`). Currently harmless
only because the `AXIsValidDevice` bug above rejects `AX_DEV_RMT` first.
The two bugs would need to be fixed together.

### `_MIXUpdateTV` surround modes are unimplemented

`snd_user.cpp` — TV sound modes 3 and 4 are `// TODO` and fall through
without updating volume targets. Mode 1 (stereo) is the default, so this
does not affect the case fixed here.

---

## Validation status

Build: applied and compiled clean (`Cemu_release.exe`, Release/Ninja/MSVC),
smoke-tested to launch without crashing.

Not yet done: side-by-side LEGO Dimensions gameplay comparison against an
unmodified Cemu 2.6 Toypad build. Per the engineering rule in
`Audio Fix/README.md`, do not treat this as a confirmed fix until that
comparison has been run.

---

## Source map

Quick reference for anyone picking this up:

```
src/Cafe/OS/libs/snd_core/
  ax_mix.cpp      voice -> bus mixing, aux store/return, master merge, LPF
  ax_out.cpp      final per-mode downmix to the DMA output buffer
  ax_aux.cpp      aux buffers, aux callback dispatch, return volume
  ax_voice.cpp    per-voice setters (DeviceMix, Ve, Lpf, ...), AXIsValidDevice
  ax_ist.cpp      shadow-copy sync between game-visible VPB and internal VPB
src/Cafe/OS/libs/snd_user/
  snd_user.cpp    MIX* layer + all AXFX effect callbacks (mostly stubs)
```
