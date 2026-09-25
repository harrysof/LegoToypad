# LEGO Dimensions Toypad Tag: Customization Bug Investigation

## Context

Comparing four Toypad NFC tag dumps (180 bytes each) for the "IMF Scrambler" vehicle
to figure out why tags written by our app's customization feature don't work, while a
"blank tag" dump produced by shadPS4 does work.

All four files share the same 180-byte layout. Bytes 8-127 are always zero in every
sample seen so far (not customization-relevant, at least not for this character).
The interesting region is **offset 140-159**.

## Sample data

### 1. Ethan_Hunt_-_1__IMF_Scrambler.bin (stock dump, works)
```
Offset 0-7   (NUID):   04 02 1a 00 b8 5d 45 80
Offset 140-143:        3f 77 31 a6
Offset 144-147:        bc 04 00 00
Offset 148-151:        57 12 07 00
Offset 156:             01
```

### 2. WORLD_MISSION_IMPOSSIBLE_VEH_IMF_SCRAMBLER_1_BIN.bin (stock dump, works)
```
Offset 0-7   (NUID):   04 02 1a 00 b8 5d 45 80
Offset 140-143:        45 f7 60 fd
Offset 144-147:        bc 04 00 00
Offset 148-151:        04 00 00 00
Offset 156:             01
```

### 3. Blank_Tag.bin (customized IMF Scrambler, dumped/produced via shadPS4, WORKS)
```
Offset 0-7   (NUID):   00 00 00 00 00 00 00 00   <- no NUID / zeroed
Offset 140-143:        15 8e 2c 25
Offset 144-147:        bc 04 00 00
Offset 148-151:        05 00 00 00
Offset 156:             00
```

### 4. Our app's output (same customization as #3, real NUID written, DOES NOT WORK)
```
Offset 0-7   (NUID):   04 02 1a 00 b8 5d 45 80   <- real NUID, copied in
Offset 140-143:        15 8e 2c 25                <- copied verbatim from #3
Offset 144-147:        bc 04 00 00
Offset 148-151:        05 00 00 00                <- copied verbatim from #3
Offset 156:             01                         <- flipped 00 -> 01 (only field we changed)
```

## What our app currently does

Takes the blank-tag customization payload (bytes 140-155 from a template like #3),
writes a real tag NUID into bytes 0-7, and flips the flag byte at offset 156 from
00 to 01. Every other byte in 140-155 is copied through unchanged from the template.

## Working hypothesis

Bytes 140-143 differ on **every single sample above**, including between two stock
dumps of the *same* character (#1 vs #2, which share an identical NUID region-format
but have completely different 140-143 values: `3f 77 31 a6` vs `45 f7 60 fd`). This
strongly suggests 140-143 is NOT a static per-character constant -- it looks like a
checksum, CRC, hash, or crypto/HMAC value computed over some combination of:
- the NUID (bytes 0-7)
- the character/customization payload itself
- possibly a nonce or write-counter (would explain why #1 and #2, same character,
  same customization state, still differ)

Because our app copies the 140-143 value verbatim from the blank template instead of
recomputing it for the new NUID, the value no longer matches what it's supposed to
validate against -- the tag is rejected as invalid on read.

Byte 148 also differs across all three template/stock sources (`57`, `04`, `05`) with
no obvious pattern yet against character ID, so it may be part of the same
checksum/nonce system rather than a meaningful "customization" field.

Byte 156 looks like a binary flag (0 = blank/uninitialized-NUID template,
1 = has a real NUID / initialized) based on the samples so far, and our app is already
handling this one correctly.

## Open questions for further investigation

1. What algorithm produces bytes 140-143? Candidates to test: CRC32 (various
   polynomials) or CRC16, straight hash (MD5/SHA truncated - less likely given LEGO's
   era), or a proprietary NFC/Mifare-style crypto check tied to the physical tag's
   secret key material. Need more sample pairs (same customization, different NUIDs;
   or same NUID, different customizations) to isolate what the 4 bytes are actually
   computed over.
2. Does byte 148-151 correlate with anything we can control (upgrade part selected,
   paint color, timestamp of write), or is it also derived/random per write?
3. Is there a public writeup of the LEGO Dimensions / Toypad NFC tag format (community
   reverse-engineering, e.g. from Cemu/Toypad-emulation projects) that documents this
   checksum region? Worth a targeted search before doing brute-force analysis.
4. Get more paired samples: ideally the SAME customization written twice (to see if
   140-143 is deterministic given fixed inputs, or truly random/nonce-based per write)
   and same NUID with different customizations (to isolate customization's effect vs
   NUID's effect on 140-143).

## Suggested next step

Generate multiple tag dumps varying one input at a time (same NUID + different
customization; same customization + different NUID) and diff bytes 140-155 across
them to narrow down which bytes are pure functions of which inputs. Once a formula
is confirmed, our app should compute 140-143 (and possibly 148-151) at write time
instead of copying it from a template.
