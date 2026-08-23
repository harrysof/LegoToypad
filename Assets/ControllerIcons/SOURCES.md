# Controller icon sets

One folder per pad style, one PNG per button. The file names are the ones
`generate_assets.py` looks for, so adding a missing button is just dropping a
PNG in with the right name — no code change:

`DpadUp` `DpadDown` `DpadLeft` `DpadRight` `Start` `Back` `LeftStickClick`
`RightStickClick` `LB` `RB` `LT` `RT` `A` `B` `X` `Y`

The names are positional (Xbox naming), not per-style: `Start` is the
DualShock's Options and the Switch's `+`, `LB` is L1 / L, `LT` is L2 / ZL, and
`A`/`B`/`X`/`Y` are Cross/Circle/Square/Triangle on PlayStation. A button with
no PNG falls back to its name drawn in a pill, so an incomplete set degrades
one button at a time.

Currently missing: `Xbox/LB`, `Xbox/RB`, `DualShock4/DpadLeft`.

## Provenance

- **Xbox**, **DualShock4** — file names match Nicolae (Xelu) Berbece's
  *Free Controller & Keyboard Prompts* pack, released under CC0.
- **Switch** — "Switch Button Icons [Essential pack]", Solid Mono / Dark theme.

**Before publishing:** confirm the license of each set and record it here.
Nintendo, Sony and Microsoft button shapes are trademarks of their respective
owners; these are UI prompts, not endorsements.
