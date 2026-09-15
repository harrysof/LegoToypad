# LEGO Dimensions Shadow Rendering Bug in Cemu — Research Dossier

**Scope:** Community/upstream research on the "shadows break above 720p" bug in *LEGO Dimensions* under Cemu (Wii U emulator), compiled from Cemu's official bug trackers, GitHub issues, the Cemu graphic-pack documentation, and inspection of the community partial-fix repo the user linked. This is desk research, not a live debugging session — nothing here was captured from the user's own Cemu log.txt or texture dumps. Written so an AI coding agent can pick it up and continue toward a real fix.

**Repo examined:** https://github.com/williamhackett0/Lego-Dimensions-Partial-Shadow-Fix

---

## 1. The bug, as documented

- The **official Cemu compatibility wiki page for LEGO Dimensions** lists this as a known issue in plain terms: shadows "look bugged when not in 720p" (wiki.cemu.info/wiki/LEGO_Dimensions). The same page also separately lists the game's quiet/poor voice audio — the bug already tracked in the audio analysis (`LEGO_AUDIO_BUG_SOURCE_ANALYSIS.md`) — so both of the user's Cemu/LD investigations trace back to the same compatibility page.
- On the old Redmine tracker, **Bug #57 "Lego Dimensions various lighting issues"** (bugs.cemu.info/issues/57, filed 2019, still open, no resolution) describes broken dynamic lighting and a lighting artifact that follows the player. The reporter's debug log lists a batch of `GX2` calls Cemu didn't support at the time, including several depth/HiZ-related ones: `GX2InitDepthBufferHiZEnable`, `GX2InitHiStencilInfoRegs`, `GX2SetHiStencilInfo`, `GX2SetClearDepth`, `GX2ExpandDepthBuffer`. This predates and is broader than the resolution-specific shadow bug, but it's evidence that LEGO Dimensions' depth/shadow path has been an emulation-accuracy weak spot for a long time, independent of any graphic pack.
- On the current tracker, **Issue #1426 "LEGO DIMENSIONS massive light issues"** (github.com/cemu-project/Cemu/issues/1426, filed Nov 2024) reports shadows as "basically disabled" at the 1920x1080 resolution patch, and buggy even without any graphic pack enabled. It was **closed as "not planned"** with no maintainer investigation, no assignee, and no follow-up comments — i.e. nobody upstream has actually dug into this.
- Net effect: there is **no upstream Cemu-side fix**, and the only remediation that exists is graphic-pack-level community work — which is what the repo the user linked is.

## 2. What the "partial fix" repo actually does

The repo is a modified **Cemu graphic pack** (not a Cemu source patch), forked from an existing LEGO Dimensions resolution pack originally authored by **bloodmc and Xalphenos**. The author (williamhackett0) is explicit in the README that this does not fix shadow *quality* — only shadow *presence*:

> "this doesn't fix the shadows in the game, they are still broken... shadows can be a distraction depending on the level due to the weirdness of shadow rendering."

Pulling the actual pack files (`LEGODimensions_resolution_keep_shadows/rules.txt` and `patches.txt`) confirms exactly what changed. Two new `[TextureRedefine]` blocks were added, explicitly labeled by the author:

```
#Implemented by William Hackett - Shadow Large
[TextureRedefine]
width = 960
height = 3840
overwriteWidth = ($width/$gameWidth) * 960
overwriteHeight  = (($height/$gameHeight) * 960) * 4 #Must be 4 times the size of the calculated dimension

#Implemented by William Hackett - Shadow Smaller
[TextureRedefine]
width = 960
height = 960
overwriteWidth = ($width/$gameWidth) * 960
overwriteHeight  = ($height/$gameHeight) * 960
```

The `960x3840` texture is very likely a **stacked shadow atlas** — 3840 is exactly 4×960, and the comment "must be 4 times" confirms the author found this by trial and error rather than deriving it from a known engine convention. That texture is almost certainly 4 shadow tiles/cascades/views packed into one vertical strip. The `960x960` block is a separate, single shadow-related texture. Both are on top of the resolution pack's pre-existing `1280x720` and `640x368` texture rules (the normal color/main-buffer redefinitions every LEGO-engine resolution pack has).

The `patches.txt` file is unrelated to shadows — it's an assembly-level memory patch (a Cemu/Cemuhook `[patches]` block) that rewrites hardcoded aspect-ratio floats in the executable for menu/UI/event scaling:

```
[LDaspectsUSv320EUv352]
moduleMatches = 0x8A9D0373, 0x8EEE187
0x101408C0 = .float $width/$height        #menu aspect
0x1041F294 = .float (1/($width/$height))  #primary aspect scale
0x10477DF4 = .float (1/($width/$height))  #event scale
0x10147788 = .float 10240
```

**What is conspicuously absent:** there are no `_ps.txt` / `_vs.txt` custom shader files anywhere in the pack. This matters — see §4.

## 3. How Cemu's `TextureRedefine` mechanism works (background)

From the official `cemu_graphic_packs` wiki ("How to create resolution packs", "How to create Graphic Packs") and wiki.cemu.info:

- Cemu identifies a texture at runtime by its physical address, dimensions, and format. A `[TextureRedefine]` rule says "any texture matching this width/height(/format) should be reinterpreted as this other size" — `overwriteWidth`/`overwriteHeight` are expressions using the preset's `$width`/`$height`/`$gameWidth`/`$gameHeight` variables to compute a scale factor.
- If a texture the game uses **isn't covered by any rule**, it stays at its native size while everything else scales up — at best this looks messy, at worst Cemu can't find a matching cached texture at the new resolution and effectively drops it.
- The wiki explicitly warns: **"Since Cemu resets the contents of a texture if any rule is applied, you would end up with a black background"** if a rule's filter is too loose and catches the wrong texture. This is why format filtering and exact width/height matching matter, and why pack authors are told to use Cemu's **Debug → Texture Information / texture dump** tools to find "texture groups" (the same physical address appearing at multiple resolutions/paddings) before writing rules.
- Separately, **Cemu 1.14.0 (Oct 2018)** added: *"GX2: Automatically scale texelFetch() coordinates to match resolution defined via texture rules."* This means shaders that read a redefined texture via raw texel coordinates get their coordinates auto-corrected — but this is specifically about coordinate lookup, not about any blur/kernel-radius math baked into the shader as constants (see §4).
- **Cemu 1.6.3** added proper hardware shadow-sampler support ("Shaders will now correctly use shadow samplers instead of imitating them by manually comparing shadow depth in shader logic") and **1.4.0c** merged depth-buffer handling into the general texture/color-buffer code path. Both are evidence that shadow/depth-texture handling specifically has had multiple non-trivial rewrites in Cemu's history — it's a historically fragile area, not a one-off oversight.

## 4. Root-cause hypothesis (evidence-based, not confirmed by a live capture)

There appear to be **two separate, stackable problems**, and the linked repo only addresses the first:

**(a) Missing texture coverage → shadows vanish above 720p.**
The base resolution pack (bloodmc/Xalphenos) scaled the main color buffer but never added `TextureRedefine` rules for the shadow atlas texture(s). At any resolution other than the native 1280x720, the shadow depth texture(s) go unmatched/unscaled, and the game's shadow pass effectively fails to render or Cemu can't resolve the texture correctly — hence "shadows disappear" above 720p. Adding the two `TextureRedefine` blocks (§2) fixes exactly this: the textures are now found and scaled, so shadows reappear.

**(b) No shader-side rescaling → shadows reappear but look wrong.**
This is a **documented, recurring class of bug** across the Cemu graphic-pack ecosystem, not something specific to LEGO Dimensions:

- **cemu_graphic_packs Issue #143** (Dec 2017, filed against the pack repo itself): *"When scaling the shadow map resolution in rules.txt, the blur part (penumbra) needs to be scaled to the same level in the shader. Otherwise you end up with a shadow like [broken example]. I assume we have the same problem for other games."* This is precisely the mechanism: soft-shadow shaders sample a fixed-radius blur/PCF kernel in texel units, tuned for the shadow map's native resolution. Redefining the texture's resolution via `rules.txt` alone does not touch the shader, so the blur kernel is now wrong relative to the new texel density — shadows come out blocky, over-blurred, aliased, or otherwise "weird" depending on the light angle and geometry, which matches the user's own description almost exactly.
- **cemu_graphic_packs Issue #369** (Wind Waker HD, May 2019): shadows become "very blurry" at higher custom resolutions, worse than at lower ones — a live example of the same class of bug in a different game.
- **cemu_graphic_packs Issues #406 and #300** (Breath of the Wild): users adding new resolution presets without touching the shadow-related shader logic report "messed up lighting/shadows" as a direct side effect.
- **cemu_graphic_packs Issue #132** (Xenoblade Chronicles X): a multi-year, many-commit effort to fully fix resolution-dependent shadow/lighting/DOF artifacts ("floating shadows," "magic z values," buffer alignment "one pixel off leading to leaking light... shadow is another res and stencil") — useful as a reference for how deep this rabbit hole can go for one game, and that partial/iterative fixes (exactly like the pack the user linked) are the normal first step, not a dead end.
- Confirms via inspection (§2): the linked pack has **zero shader files**. It only patches `TextureRedefine` + one aspect-ratio memory patch. By the pattern established in #143/#369/#132, that is consistent with — and probably sufficient to explain — why the author says shadows are "restored but still broken/weird."

**A separate, likely-independent third factor:** Bug #57's unsupported `GX2` depth/HiZ calls (§1) suggest there may also be an emulation-accuracy gap in how Cemu itself handles this game's depth buffer, on top of the graphic-pack-level issue. This is from a 2019 log against an old Cemu build, so it needs to be re-verified against a current build (see §5) rather than assumed still true.

**Explicitly out of scope / do not conflate:** `Cemu` Issue #1176 (AMD RDNA3 + Vulkan renderer causing shadow/texture corruption in unrelated games like Mario Kart 8 and BOTW) is a GPU-driver/renderer-backend bug, not a graphic-pack or LEGO-Dimensions-specific issue. If the user's own captures ever show shadow corruption that looks like garbling/corruption rather than "wrong blur/shape," check GPU vendor and renderer backend (Vulkan vs OpenGL) before assuming it's the same bug documented here.

## 5. Suggested next steps for further investigation

1. **Reproduce with Debug → Texture Relations/Information + texture dump enabled**, at both 720p and 1080p, per the official pack-authoring workflow (§3). Confirm whether any shadow-adjacent textures are still unmatched at 1080p even with the linked pack active — i.e. check whether §2's two rules are actually complete, or whether the "weirdness ... depending on the level" the author mentions means some levels use additional shadow textures the pack doesn't cover.
2. **Dump the game's shaders** (`dump/shaders/`) with the pack active, identify the pixel shader(s) that sample the shadow atlas (look for texel-fetch or shadow-sampler calls plus blur/offset math), and compare their hardcoded texel-size or kernel-radius constants against the shadow texture's now-scaled resolution. This is the concrete, actionable version of the fix Issue #143 describes: add a custom `_ps.txt` shader override to the graphic pack that rescales those constants by the same factor `($width/$gameWidth)` used in the `TextureRedefine` blocks, mirroring how XCX/Wind Waker HD/Twilight Princess HD packs expose `$lightSource`/`$scaleShader` preset variables for exactly this purpose.
3. **Re-verify Bug #57's unsupported-GX2-call list against the current Cemu version** the user is building against (their `Cemu-Toypad-v2.6` source) — grep `src/Cafe/HW/Latte/` and the `GX2` HLE implementation for `GX2InitDepthBufferHiZEnable`, `GX2ExpandDepthBuffer`, and `GX2SetClearDepth` to see whether these are now implemented, partially implemented, or still stubbed. If any depth-clear/HiZ path is still a no-op, a "previously unknown texture's first depth clear being ignored" (a real bug class Cemu fixed elsewhere per its 1.14.0 changelog) is worth ruling out as a contributor to the "weird artifact" specifically at 1080p+.
4. **Treat the 960x3840 "4x960 stacked" texture as a shadow atlas/cascade array**, not a single map — if the fix requires per-tile UV math (rather than one uniform scale), a naive linear `overwriteHeight` scale (as currently written) could itself be introducing part of the visible weirdness, separately from the shader-blur issue in §4b.
5. Since the user already has the Cemu fork source built and running locally, the cheapest first experiment is likely reproducing Issue #1426's claim that shadows are "still really buggy" **even with no graphic pack at all** at non-default settings — that would help separate "graphic-pack scaling bug" from "underlying Cemu depth/shadow emulation bug" before investing more time in shader work.

## 6. Sources

- Repo examined: https://github.com/williamhackett0/Lego-Dimensions-Partial-Shadow-Fix
- Original Reddit report: https://www.reddit.com/r/Legodimensions/comments/qvldp5/cemu_restoring_shadows_with_resolution_pack/
- Cemu wiki, LEGO Dimensions compatibility page: https://wiki.cemu.info/wiki/LEGO_Dimensions
- Cemu bug tracker Bug #57 (lighting issues, 2019): https://bugs.cemu.info/issues/57
- Cemu GitHub Issue #1426 (shadows disabled, closed not planned): https://github.com/cemu-project/Cemu/issues/1426
- Cemu GitHub Issue #1176 (AMD RDNA3/Vulkan shadow corruption, different bug class): https://github.com/cemu-project/Cemu/issues/1176
- cemu_graphic_packs wiki, "How to create resolution packs": https://github.com/cemu-project/cemu_graphic_packs/wiki/How-to-create-resolution-packs
- cemu_graphic_packs wiki, "How to create Graphic Packs": https://github.com/cemu-project/cemu_graphic_packs/wiki/How-to-create-Graphic-Packs
- wiki.cemu.info, "Graphic packs creation": https://wiki.cemu.info/wiki/Graphic_packs_creation
- cemu_graphic_packs Issue #143 (shadow blur/penumbra must scale with shadow map resolution): https://github.com/cemu-project/cemu_graphic_packs/issues/143
- cemu_graphic_packs Issue #369 (Wind Waker HD shadows blurry at high res): https://github.com/cemu-project/cemu_graphic_packs/issues/369
- cemu_graphic_packs Issue #406 (BOTW resolution preset breaking shadows/lighting): https://github.com/cemu-project/cemu_graphic_packs/issues/406
- cemu_graphic_packs Issue #300 (BOTW shadow resolution problems): https://github.com/cemu-project/cemu_graphic_packs/issues/300
- cemu_graphic_packs Issue #132 (Xenoblade Chronicles X, multi-year shadow/lighting scaling fixes): https://github.com/cemu-project/cemu_graphic_packs/issues/132
- Cemu changelog 1.14.0 (texelFetch auto-scaling): http://cemu.info/patreon/changelog/cemu_1_14_0.txt
- Cemu wiki, Release 1.6.3 (hardware shadow samplers): https://wiki.cemu.info/wiki/Release_1.6.3
- Cemu wiki, Release 1.4.0c (depth buffer merged into texture logic): https://wiki.cemu.info/wiki/Release_1.4.0c
- DeepWiki, Cemu architecture overview (renderer/texture cache source layout): https://deepwiki.com/cemu-project/Cemu
