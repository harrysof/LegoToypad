#!/usr/bin/env python3
"""Build-time asset generator for LegoToypad.

Walks the prepared source layout ("All Bin Files/<World>/{Characters,Vehicules,Logo}"
plus the "Assets" folder) and emits, into the output directory:

  - resources.rc              : one RCDATA entry per .bin/.png, plus the app ICON
  - GeneratedAssetTable.h     : resource id defines (also consumed by rc.exe) and
                                the Franchise / VehicleGroup / RosterEntry structs
  - GeneratedAssetTable.cpp   : the compiled-in NBA-table data with precomputed
                                deterministic ring colors
  - app.ico                   : the app icon (PNG payload wrapped in an ICO frame)

The app itself never reads from disk at runtime - everything is embedded as Win32
resources, loaded via FindResource/LoadResource/LockResource.

Usage:
    python generate_assets.py [--root <project dir>] [--out <output dir>]

--root defaults to the script's own directory and must contain the "All Bin Files"
and "Assets" folders. --out defaults to <root>/generated.

The script is intentionally deterministic: identical inputs produce byte-identical
outputs, and files are only rewritten when their content actually changes, so an
"always regenerate" CMake step does not cause rebuild churn.
"""

import argparse
import io
import os
import re
import struct
import sys
from pathlib import Path

RCDATA_BASE_ID = 1000  # numeric ids start here, one per resource name
APP_ICON_ID = 100

# Version metadata stamped into the exe's VERSIONINFO resource. Bump these
# together with the release tag; the comma form is what rc.exe's
# FILEVERSION/PRODUCTVERSION statements require.
APP_VERSION = "1.3.0"
APP_VERSION_COMMA = "1,3,0,0"
APP_COMPANY = "HarrysofXD"
APP_DESCRIPTION = "Controller-driven LEGO Dimensions Toypad companion for Cemu and Rpcs3"

# ---------------------------------------------------------------------------
# Deterministic hash -> color
# ---------------------------------------------------------------------------

def fnv1a(value: str) -> int:
    h = 0x811C9DC5
    for byte in value.encode("ascii"):
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def hue_from_hash(h: int) -> int:
    # Golden-angle decorrelation: nearby hashes still get well spread hues.
    return int(((h & 0xFFFFFFFF) / 4294967296.0 * 0.6180339887498949 % 1.0) * 360.0)


def hsv_to_rgb(h: float, s: float, v: float):
    c = v * s
    x = c * (1 - abs((h / 60.0) % 2 - 1))
    m = v - c
    if h < 60:
        r, g, b = c, x, 0
    elif h < 120:
        r, g, b = x, c, 0
    elif h < 180:
        r, g, b = 0, c, x
    elif h < 240:
        r, g, b = 0, x, c
    elif h < 300:
        r, g, b = x, 0, c
    else:
        r, g, b = c, 0, x
    return round((r + m) * 255), round((g + m) * 255), round((b + m) * 255)


def ring_color_for(resource_name: str) -> str:
    h = hue_from_hash(fnv1a(resource_name))
    r, g, b = hsv_to_rgb(float(h), 0.62, 0.96)
    # GDI COLORREF layout is 0x00BBGGRR; the RGB() macro takes care of it.
    return "RGB(0x%02X, 0x%02X, 0x%02X)" % (r, g, b)


# ---------------------------------------------------------------------------
# Resource naming
# ---------------------------------------------------------------------------

def sanitize(component: str) -> str:
    out = re.sub(r"[^A-Za-z0-9]+", "_", component.upper()).strip("_")
    return re.sub(r"_+", "_", out) or "EMPTY"


def make_resource_names(world_id, kind, name_id, build=None):
    if kind == "CHAR":
        return (
            "WORLD_%s_CHAR_%s_BIN" % (world_id, name_id),
            "WORLD_%s_CHAR_%s_PNG" % (world_id, name_id),
        )
    if kind == "VEH":
        return (
            "WORLD_%s_VEH_%s_%d_BIN" % (world_id, name_id, build),
            "WORLD_%s_VEH_%s_%d_PNG" % (world_id, name_id, build),
        )
    raise ValueError(kind)


# ---------------------------------------------------------------------------
# Pairing helpers
# ---------------------------------------------------------------------------

VEHICLE_PREFIX_RE = re.compile(r"^(\d+)[.\-]\s*(.+)$")


def split_vehicle_stem(stem):
    """Returns (build_number, base_name); unnumbered files become build 1."""
    match = VEHICLE_PREFIX_RE.match(stem)
    if match:
        return int(match.group(1)), match.group(2).strip()
    return 1, stem.strip()


def normalize(stem: str) -> str:
    return "".join(ch for ch in stem.lower() if ch.isalnum())


def discover_case_insensitive(directory: Path, wanted: str):
    if not directory.is_dir():
        return None
    wanted_l = wanted.lower()
    for child in directory.iterdir():
        if child.is_dir() and child.name.lower() == wanted_l:
            return child
    return None


def image_files(directory: Path):
    out = []
    if directory and directory.is_dir():
        for child in directory.iterdir():
            if child.is_file() and child.suffix.lower() in (".png", ".jpg", ".jpeg"):
                if child.stat().st_size > 0:
                    out.append(child)
    return sorted(out, key=lambda p: p.name.casefold())


def pair_bins_and_images(bin_files, png_files, warnings):
    """One portrait per .bin: exact stem first, normalized-stem fallback."""
    by_stem = {p.stem: p for p in png_files}
    by_norm = {}
    for p in png_files:
        by_norm.setdefault(normalize(p.stem), []).append(p)

    pairs = []
    for binary in sorted(bin_files, key=lambda p: p.stem.casefold()):
        portrait = by_stem.get(binary.stem)
        if portrait is None:
            candidates = by_norm.get(normalize(binary.stem), [])
            if len(candidates) == 1:
                portrait = candidates[0]
                warnings.append(
                    "portrait name mismatch (paired): %s <-> %s" % (binary.name, portrait.name))
            elif len(candidates) > 1:
                warnings.append("ambiguous portrait, using placeholder: %s" % binary.name)
            else:
                warnings.append("no portrait found, using placeholder: %s" % binary.name)
        pairs.append((binary, portrait))
    return pairs


# ---------------------------------------------------------------------------
# Symbol allocation
# ---------------------------------------------------------------------------

class SymbolTable:
    def __init__(self):
        self.by_name = {}
        self.next_id = RCDATA_BASE_ID

    def allocate(self, name, path):
        if name in self.by_name:
            raise ValueError("duplicate resource name: %s" % name)
        symbol = self.by_name[name] = {
            "name": name,
            "path": path,
            "id": self.next_id,
        }
        self.next_id += 1
        return symbol


def ascii_ok(parts, warnings):
    bad = []
    for part in parts:
        try:
            part.encode("ascii")
        except UnicodeEncodeError:
            bad.append(part)
    if bad:
        for b in bad:
            warnings.append("NON-ASCII path component (rc.exe will mangle it): %s" % b)
        return False
    return True


# ---------------------------------------------------------------------------
# Main generation
# ---------------------------------------------------------------------------

def generate(root: Path, out_dir: Path) -> int:
    warnings = []
    root = root.resolve()
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    source_root = discover_case_insensitive(root, "All Bin Files")
    assets_root = discover_case_insensitive(root, "Assets")
    if source_root is None:
        print("ERROR: no 'All Bin Files' folder found under %s" % root)
        return 1
    if assets_root is None:
        print("ERROR: no 'Assets' folder found under %s" % root)
        return 1

    symbols = SymbolTable()
    log_lines = []  # (resource_name, rc_line, color_line)
    franchises = []

    world_folders = sorted(
        [d for d in source_root.iterdir() if d.is_dir()], key=lambda d: d.name.casefold())

    for world in world_folders:
        world_id = sanitize(world.name)
        logo_folder = discover_case_insensitive(world, "Logo")
        logos = image_files(logo_folder)

        def alloc(name):
            path = None  # placeholder ids (missing files) never map to a path
            return symbols.allocate(name, path)

        world_resources = {}

        if len(logos) == 1:
            logo_sym = symbols.allocate("WORLD_%s_LOGO" % world_id, logos[0])
            ascii_ok([str(logos[0])], warnings)
        elif not logos:
            warnings.append("missing logo folder for world: %s" % world.name)
            logo_sym = alloc("WORLD_%s_LOGO" % world_id)
        else:
            logo_sym = symbols.allocate("WORLD_%s_LOGO" % world_id, logos[0])
            warnings.append("multiple logo images, using %s for world %s"
                            % (logos[0].name, world.name))

        # ---- characters ---------------------------------------------------
        characters = []
        chars_folder = discover_case_insensitive(world, "Characters")
        char_bins = [f for f in (chars_folder.iterdir() if chars_folder else [])
                     if f.is_file() and f.suffix.lower() == ".bin"]
        for binary, portrait in pair_bins_and_images(char_bins, image_files(chars_folder), warnings):
            name_id = sanitize(binary.stem)
            bin_name, png_name = make_resource_names(world_id, "CHAR", name_id)
            bin_sym = symbols.allocate(bin_name, binary)
            png_sym = symbols.allocate(png_name, portrait) if portrait else alloc(png_name)
            if portrait:
                ascii_ok([str(binary), str(portrait)], warnings)
            characters.append({
                "name": binary.stem,
                "bin": bin_sym,
                "png": png_sym,
                "build": 0,
            })

        # ---- vehicles -----------------------------------------------------
        veh_groups = {}
        veh_folder = discover_case_insensitive(world, "Vehicules")
        veh_bins = [f for f in (veh_folder.iterdir() if veh_folder else [])
                    if f.is_file() and f.suffix.lower() == ".bin"]
        for binary, portrait in pair_bins_and_images(veh_bins, image_files(veh_folder), warnings):
            build, base = split_vehicle_stem(binary.stem)
            name_id = sanitize(base)
            bin_name, png_name = make_resource_names(world_id, "VEH", name_id, build)
            bin_sym = symbols.allocate(bin_name, binary)
            png_sym = symbols.allocate(png_name, portrait) if portrait else alloc(png_name)
            if portrait:
                ascii_ok([str(binary), str(portrait)], warnings)
            entry = {"name": base, "bin": bin_sym, "png": png_sym, "build": build}
            veh_groups.setdefault(base.casefold(), {"base": base, "builds": []})
            veh_groups[base.casefold()]["builds"].append(entry)

        vehicles = []
        for key in sorted(veh_groups.keys()):
            group = veh_groups[key]
            group["builds"] = sorted(group["builds"], key=lambda e: e["build"])
            base_name = group["base"]
            # Several builds whose printable names differ only by case collapse
            # into one group key; keep the most common original spelling.
            if len(set(x["name"] for x in group["builds"])) > 1:
                warnings.append("vehicle builds with differing spellings merged: %s"
                                % ", ".join(x["name"] for x in group["builds"]))
            claimed = max(set(x["name"] for x in group["builds"]),
                          key=lambda n: sum(1 for x in group["builds"] if x["name"] == n))
            group["base"] = claimed
            vehicles.append(group)

        franchises.append({
            "name": world.name,
            "logosym": logo_sym,
            "characters": characters,
            "vehicles": vehicles,
        })

    # order ids so the .rc and the table agree; ids already assigned in walk
    # order above, which is deterministic because world/char/vehicle iteration
    # is sorted.

    # ---- app-level assets -------------------------------------------------
    assets_files = image_files(assets_root)

    def pick_asset(expected_names, keyword, what):
        for candidate in sorted(assets_files, key=lambda p: p.stem.casefold()):
            if candidate.stem.casefold() in expected_names:
                return candidate
        matches = [p for p in assets_files if keyword.casefold() in p.name.casefold()]
        if len(matches) == 1:
            return matches[0]
        if matches:
            warnings.append("multiple %s candidates, using %s; expected %s"
                            % (what, matches[0].name, "/".join(expected_names)))
            return matches[0]
        return None

    wordmark = pick_asset({"lego_toypad_wordmark", "wordmark"}, "wordmark", "wordmark")
    background = pick_asset({"background"}, "background", "background")

    def background_sort_key(path):
        stem = path.stem.casefold()
        if stem == "background":
            return (0, 0, path.name.casefold())
        match = re.match(r"^background_(\d+)$", stem)
        if match:
            return (1, int(match.group(1)), path.name.casefold())
        return (2, 0, path.name.casefold())

    numbered_backgrounds = [
        p for p in assets_files
        if re.match(r"^background_(\d+)$", p.stem.casefold())
    ]
    background_choices = []
    if background:
        background_choices.append(("Default", background, "ASSET_BACKGROUND"))
    for candidate in sorted(numbered_backgrounds, key=background_sort_key):
        number = re.match(r"^background_(\d+)$", candidate.stem.casefold()).group(1)
        background_choices.append(("Background %s" % number, candidate,
                                   "ASSET_BACKGROUND_%s" % sanitize(number)))

    app_icon = pick_asset({"legotoypad_logo", "app_icon", "app_logo", "appicon"},
                          "logo", "app icon")

    wordmark_sym = symbols.allocate("ASSET_WORDMARK", wordmark) if wordmark else None
    background_choice_syms = [
        {"label": label, "symbol": symbols.allocate(symbol_name, path)}
        for label, path, symbol_name in background_choices
    ]
    background_sym = background_choice_syms[0]["symbol"] if background_choice_syms else None
    icon_sym = symbols.allocate("ASSET_APPICON", app_icon) if app_icon else None
    if wordmark:
        ascii_ok([str(wordmark)], warnings)
    for _, path, _ in background_choices:
        ascii_ok([str(path)], warnings)
    if app_icon:
        ascii_ok([str(app_icon)], warnings)

    # ---- pad backgrounds + tile panels ------------------------------------
    # Per-toypad-slot background art, one PNG per slot in kPadCells index
    # order (left-upper, center, right-upper, left-lower-left,
    # left-lower-right, right-lower-left, right-lower-right), plus the
    # single franchise-tile background and the full roster-grid panel.
    def pick_named(expected_stems, what):
        for candidate in sorted(assets_files, key=lambda p: p.stem.casefold()):
            if candidate.stem.casefold() in expected_stems:
                return candidate
        warnings.append("missing %s image in Assets (expected filename: %s)"
                        % (what, " or ".join(sorted(expected_stems))))
        return None

    pad_bg_spec = [
        ("ASSET_PAD_BG_LEFT_UPPER", "left_upper"),
        ("ASSET_PAD_BG_CENTER", "center"),
        ("ASSET_PAD_BG_RIGHT_UPPER", "right_upper"),
        ("ASSET_PAD_BG_LEFT_LOWER_LEFT", "left_lower_left"),
        ("ASSET_PAD_BG_LEFT_LOWER_RIGHT", "left_lower_right"),
        ("ASSET_PAD_BG_RIGHT_LOWER_LEFT", "right_lower_left"),
        ("ASSET_PAD_BG_RIGHT_LOWER_RIGHT", "right_lower_right"),
    ]
    pad_bg_syms = []
    for asset_name, stem in pad_bg_spec:
        file = pick_named({stem}, "pad background")
        sym = symbols.allocate(asset_name, file) if file else None
        if file:
            ascii_ok([str(file)], warnings)
        pad_bg_syms.append(sym)

    world_tile = pick_named({"world_tile"}, "world tile background")
    characters_tile = pick_named({"characters_tile"}, "characters tile background")
    world_tile_sym = symbols.allocate("ASSET_WORLD_TILE", world_tile) if world_tile else None
    characters_tile_sym = symbols.allocate("ASSET_CHARACTERS_TILE", characters_tile) if characters_tile else None
    if world_tile:
        ascii_ok([str(world_tile)], warnings)
    if characters_tile:
        ascii_ok([str(characters_tile)], warnings)

    # ---- action button assets ---------------------------------------------
    textfield_bar   = None
    load_button     = pick_named({"load_button", "load"},     "load button")
    clear_button    = pick_named({"clear_button", "clear"},   "clear button")
    move_button     = pick_named({"move_button", "move"},     "move button")
    scroll_bar      = pick_named({"scroll_bar"},      "scroll bar")
    by_harrysof     = pick_named({"by_harrysof"},      "by harrysof watermark")
    textfield_bar_sym  = symbols.allocate("ASSET_TEXTFIELD_BAR",  textfield_bar)  if textfield_bar  else None
    load_button_sym    = symbols.allocate("ASSET_LOAD_BUTTON",    load_button)    if load_button    else None
    clear_button_sym   = symbols.allocate("ASSET_CLEAR_BUTTON",   clear_button)   if clear_button   else None
    move_button_sym    = symbols.allocate("ASSET_MOVE_BUTTON",    move_button)    if move_button    else None
    scroll_bar_sym     = symbols.allocate("ASSET_SCROLL_BAR",     scroll_bar)     if scroll_bar     else None
    by_harrysof_sym    = symbols.allocate("ASSET_BY_HARRYSOF",    by_harrysof)    if by_harrysof    else None
    for name, asset in [("load_button", load_button), ("clear_button", clear_button),
                        ("move_button", move_button), ("scroll_bar", scroll_bar),
                        ("by_harrysof", by_harrysof)]:
        if asset:
            ascii_ok([str(asset)], warnings)
        else:
            warnings.append("missing action-bar asset: %s" % name)

    # ---- UI font -----------------------------------------------------------
    # The Compacta-typeface font embedded for GDI/GDI+ in-memory loading.
    # GDI/GDI+ only consume raw SFNT (.ttf/.otf) bytes in memory, so web-font
    # formats (.woff/.woff2) are decompressed here at build time and staged
    # into the output dir; the runtime app path is unchanged.
    ui_font = None
    font_rc_path = None
    if assets_root:
        font_candidates = [p for p in assets_root.iterdir()
                           if p.is_file() and p.suffix.lower() in (".ttf", ".otf", ".woff", ".woff2")]
        if font_candidates:
            font_candidates.sort(key=lambda p: p.stem.casefold())
            ui_font = font_candidates[0]
            if len(font_candidates) > 1:
                warnings.append("multiple fonts in Assets, using %s" % font_candidates[0].name)

    if ui_font and ui_font.suffix.lower() in (".woff", ".woff2"):
        try:
            from io import BytesIO
            from fontTools.ttLib import woff2
            payload = BytesIO()
            with open(ui_font, "rb") as f:
                woff2.decompress(f, payload)
            sfnt = payload.getvalue()
            if len(sfnt) < 4:
                raise ValueError("decompressed font is empty")
        except ImportError:
            print("ERROR: decompressing .woff2 UI font %s requires the Python"
                  % ui_font)
            print("       packages 'fonttools' and 'brotli' (pip install fonttools brotli).")
            return 1
        except Exception as exc:
            print("ERROR: could not decompress UI font %s: %s" % (ui_font, exc))
            return 1
        # Pick a faithful extension for the staged copy (name only; the RCDATA
        # payload is embedded verbatim either way).
        ext = ".ttf" if sfnt[:4] in (b"\x00\x01\x00\x00", b"true", b"typ1") else ".otf"
        font_rc_path = out_dir / ("ui_font" + ext)
        if not font_rc_path.exists() or font_rc_path.read_bytes() != sfnt:
            font_rc_path.write_bytes(sfnt)
    elif ui_font:
        font_rc_path = ui_font
    font_sym = symbols.allocate("ASSET_UI_FONT", font_rc_path) if font_rc_path else None
    if font_rc_path:
        ascii_ok([str(font_rc_path)], warnings)

    if not wordmark:
        print("ERROR: could not find the wordmark image in %s" % assets_root)
        return 1
    if not icon_sym:
        print("ERROR: could not find the app icon image (expects *logo*.png) in %s" % assets_root)
        return 1
    if not font_sym:
        print("ERROR: could not find a .ttf/.otf/.woff2 UI font in %s" % assets_root)
        return 1

    # colliding normalized ids are impossible per-folder (unique stems), but
    # cross-folder word/character collisions can exist (e.g. "E.T." vs "ET");
    # detect and resolve deterministically.
    used = {}
    for symbol in symbols.by_name.values():
        name = symbol["name"]
        if name in used:
            suffix = 2
            while ("%s_%d" % (name, suffix)) in used:
                suffix += 1
            new_name = "%s_%d" % (name, suffix)
            warnings.append("resource name collision resolved: %s -> %s" % (name, new_name))
            used[new_name] = True
            symbol["name"] = new_name
        else:
            used[name] = True

    # -----------------------------------------------------------------------
    # Emit resources.rc
    # -----------------------------------------------------------------------
    rc_lines = ['#include "GeneratedAssetTable.h"', '#include <winver.h>', ""]
    rc_lines.append("// Numeric ids are defined in GeneratedAssetTable.h; the")
    rc_lines.append("// RCDATA payloads below are the embedded asset payloads.")
    rc_lines.append("")
    # A real VERSIONINFO block keeps the signed metadata rich instead of
    # anonymous; unsigned binaries with zero version info and huge opaque
    # resource payloads are exactly the profile Windows Defender's ML
    # heuristics (Wacatac.C!ml) score as suspicious.
    rc_lines.append("VS_VERSION_INFO VERSIONINFO")
    rc_lines.append(" FILEVERSION %s" % APP_VERSION_COMMA)
    rc_lines.append(" PRODUCTVERSION %s" % APP_VERSION_COMMA)
    rc_lines.append(" FILEFLAGSMASK 0x3fL")
    rc_lines.append(" FILEFLAGS 0x0L")
    rc_lines.append(" FILEOS 0x40004L")  # VOS_NT_WINDOWS32
    rc_lines.append(" FILETYPE 0x1L")    # VFT_APP
    rc_lines.append(" FILESUBTYPE 0x0L")
    rc_lines.append("BEGIN")
    rc_lines.append("    BLOCK \"StringFileInfo\"")
    rc_lines.append("    BEGIN")
    rc_lines.append("        BLOCK \"040904b0\"")
    rc_lines.append("        BEGIN")
    rc_lines.append('            VALUE "CompanyName", "%s"' % APP_COMPANY)
    rc_lines.append('            VALUE "FileDescription", "%s"' % APP_DESCRIPTION)
    rc_lines.append('            VALUE "FileVersion", "%s"' % APP_VERSION)
    rc_lines.append('            VALUE "InternalName", "LegoToypad"')
    rc_lines.append('            VALUE "OriginalFilename", "LegoToypad.exe"')
    rc_lines.append('            VALUE "ProductName", "LegoToypad"')
    rc_lines.append('            VALUE "ProductVersion", "%s"' % APP_VERSION)
    rc_lines.append("        END")
    rc_lines.append("    END")
    rc_lines.append("    BLOCK \"VarFileInfo\"")
    rc_lines.append("    BEGIN")
    rc_lines.append('        VALUE "Translation", 0x409, 1200')
    rc_lines.append("    END")
    rc_lines.append("END")
    rc_lines.append("")
    icon_path = out_dir / "app.ico"
    rc_lines.append("IDI_APP_ICON ICON \"app.ico\"")
    for symbol in symbols.by_name.values():
        if symbol["path"] is None:
            continue
        rc_lines.append("%s RCDATA \"%s\"" % (symbol["name"], str(symbol["path"]).replace("\\", "\\\\")))

    # -----------------------------------------------------------------------
    # Emit GeneratedAssetTable.h
    # -----------------------------------------------------------------------
    header = []
    header.append("// Generated by generate_assets.py - DO NOT EDIT BY HAND.")
    header.append("// Re-run: python generate_assets.py")
    header.append("#pragma once")
    header.append("")
    header.append("// Resource ids. rc.exe defines RC_INVOKED, so the C++")
    header.append("// struct declarations below are excluded from the .rc pass.")
    header.append("")
    header.append("#define IDI_APP_ICON %d" % APP_ICON_ID)
    for symbol in sorted(symbols.by_name.values(), key=lambda s: s["id"]):
        header.append("#define %s %d" % (symbol["name"], symbol["id"]))
    header.append("")
    header.append("#ifndef RC_INVOKED")
    header.append("")
    header.append("#include <string>")
    header.append("#include <vector>")
    header.append("")
    header.append("struct RosterEntry")
    header.append("{")
    header.append("    int binResourceId;       // RCDATA id of the .bin payload")
    header.append("    int portraitResourceId;  // RCDATA id of the .png, 0 = none")
    header.append("    std::wstring name;       // character name or vehicle base name")
    header.append("    unsigned int ringColor;  // deterministic, see generator")
    header.append("    int buildNumber;         // 0 = character, 1/2/3 = vehicle build")
    header.append("};")
    header.append("")
    header.append("struct VehicleGroup")
    header.append("{")
    header.append("    std::wstring baseName;")
    header.append("    std::vector<RosterEntry> builds;  // sorted by buildNumber")
    header.append("};")
    header.append("")
    header.append("struct Franchise")
    header.append("{")
    header.append("    std::wstring name;")
    header.append("    int logoResourceId;")
    header.append("    std::vector<RosterEntry> characters;")
    header.append("    std::vector<VehicleGroup> vehicles;")
    header.append("};")
    header.append("")
    header.append("struct BackgroundChoice")
    header.append("{")
    header.append("    std::wstring name;")
    header.append("    int resourceId;")
    header.append("};")
    header.append("")
    header.append("extern const Franchise kFranchises[];")
    header.append("extern const size_t kFranchiseCount;")
    header.append("extern const BackgroundChoice kBackgroundChoices[];")
    header.append("extern const size_t kBackgroundChoiceCount;")
    header.append("extern const int kWordmarkResourceId;")
    header.append("extern const int kBackgroundResourceId;")
    header.append("extern const int kAppIconResourceId;")
    header.append("extern const int kUIFontResourceId;")
    header.append("extern const int kWorldTileResourceId;")
    header.append("extern const int kCharactersTileResourceId;")
    header.append("extern const int kPadBackgroundResourceIds[7];")
    header.append("extern const int kTextfieldBarResourceId;")
    header.append("extern const int kLoadButtonResourceId;")
    header.append("extern const int kClearButtonResourceId;")
    header.append("extern const int kMoveButtonResourceId;")
    header.append("extern const int kScrollBarResourceId;")
    header.append("extern const int kByHarrysofResourceId;")
    header.append("")
    header.append("#endif  // RC_INVOKED")
    header.append("")

    # -----------------------------------------------------------------------
    # Emit GeneratedAssetTable.cpp
    # -----------------------------------------------------------------------

    def wstr(value):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return 'L"%s"' % escaped

    cpp = []
    cpp.append("// Generated by generate_assets.py - DO NOT EDIT BY HAND.")
    cpp.append("// Re-run: python generate_assets.py")
    cpp.append('#include "GeneratedAssetTable.h"')
    cpp.append("#include <windows.h>")
    cpp.append("")
    cpp.append("const int kWordmarkResourceId = %s;" % (wordmark_sym["name"] if wordmark_sym else "0"))
    cpp.append("const int kBackgroundResourceId = %s;" % (background_sym["name"] if background_sym else "0"))
    cpp.append("const int kAppIconResourceId = %s;" % icon_sym["name"])
    cpp.append("const int kUIFontResourceId = %s;" % (font_sym["name"] if font_sym else "0"))
    cpp.append("const int kWorldTileResourceId = %s;" % (world_tile_sym["name"] if world_tile_sym else "0"))
    cpp.append("const int kCharactersTileResourceId = %s;" % (characters_tile_sym["name"] if characters_tile_sym else "0"))
    cpp.append("const int kPadBackgroundResourceIds[7] = { %s };"
               % ", ".join(sym["name"] if sym else "0" for sym in pad_bg_syms))
    cpp.append("const int kTextfieldBarResourceId = %s;" % (textfield_bar_sym["name"] if textfield_bar_sym else "0"))
    cpp.append("const int kLoadButtonResourceId = %s;" % (load_button_sym["name"] if load_button_sym else "0"))
    cpp.append("const int kClearButtonResourceId = %s;" % (clear_button_sym["name"] if clear_button_sym else "0"))
    cpp.append("const int kMoveButtonResourceId = %s;" % (move_button_sym["name"] if move_button_sym else "0"))
    cpp.append("const int kScrollBarResourceId = %s;" % (scroll_bar_sym["name"] if scroll_bar_sym else "0"))
    cpp.append("const int kByHarrysofResourceId = %s;" % (by_harrysof_sym["name"] if by_harrysof_sym else "0"))
    cpp.append("")
    cpp.append("const BackgroundChoice kBackgroundChoices[] = {")
    for item in background_choice_syms:
        cpp.append("    { %s, %s }," % (wstr(item["label"]), item["symbol"]["name"]))
    cpp.append("};")
    cpp.append("const size_t kBackgroundChoiceCount = sizeof(kBackgroundChoices) / sizeof(kBackgroundChoices[0]);")
    cpp.append("")

    def emit_entry(entry):
        return ("{ %s, %s, %s, %s, %d }"
                % (entry["bin"]["name"], entry["png"]["name"], wstr(entry["name"]),
                   ring_color_for(entry["bin"]["name"]), entry["build"]))

    def emit_group(group):
        build_entries = ", ".join(emit_entry(e) for e in group["builds"])
        return ("{ %s, { %s } }" % (wstr(group["base"]), build_entries))

    cpp.append("const Franchise kFranchises[] = {")
    for franchise in franchises:
        cpp.append("    {")
        cpp.append("        %s," % wstr(franchise["name"]))
        cpp.append("        %s," % franchise["logosym"]["name"])
        cpp.append("        { %s }," % ", ".join(emit_entry(c) for c in franchise["characters"]))
        cpp.append("        { %s }," % ", ".join(emit_group(v) for v in franchise["vehicles"]))
        cpp.append("    },")
    cpp.append("};")
    cpp.append("")
    cpp.append("const size_t kFranchiseCount = %d;" % len(franchises))
    cpp.append("")

    # -----------------------------------------------------------------------
    # Emit app.ico. A real multi-resolution ICO (16/24/32/48/256) so the
    # system tray, taskbar and title bar each get a native-size frame instead
    # of a single 256px PNG downscaled for the small tray icon. Falls back to
    # the old single PNG frame if Pillow isn't installed.
    # -----------------------------------------------------------------------
    png_bytes = app_icon.read_bytes()
    try:
        from PIL import Image

        src = Image.open(io.BytesIO(png_bytes))
        src.load()
        buf = io.BytesIO()
        src.save(buf, format="ICO", sizes=[(s, s) for s in (16, 24, 32, 48, 256)])
        ico = buf.getvalue()
    except Exception:
        ico = struct.pack("<HHH", 0, 1, 1)                      # ICONDIR
        ico += struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(png_bytes), 22)  # ICONDIRENTRY
        ico += png_bytes

    files = {
        out_dir / "resources.rc": "\n".join(rc_lines) + "\n",
        out_dir / "GeneratedAssetTable.h": "\n".join(header) + "\n",
        out_dir / "GeneratedAssetTable.cpp": "\n".join(cpp) + "\n",
        out_dir / "app.ico": ico,
    }

    written = []
    for path, content in files.items():
        encoded = content if isinstance(content, bytes) else content.encode("utf-8")
        if path.exists() and path.read_bytes() == encoded:
            continue
        path.write_bytes(encoded)
        written.append(path.name)

    char_count = sum(len(f["characters"]) for f in franchises)
    veh_count = sum(len(v["builds"]) for f in franchises for v in f["vehicles"])
    print("source root: %s" % source_root)
    print("assets root: %s" % assets_root)
    print("worlds: %d, characters: %d, vehicles: %d, resources: %d"
          % (len(franchises), char_count, veh_count, len(symbols.by_name)))
    print("wrote: %s" % (", ".join(written) if written else "nothing changed"))
    for warning in warnings:
        print("WARNING: %s" % warning)
    if written:
        sample = [s for s in sorted(symbols.by_name.values(), key=lambda s: s["id"]) if s["path"]]
        for symbol in sample[:8]:
            print("sample %04d  %s  <-  %s" % (symbol["id"], symbol["name"], symbol["path"]))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=None,
                        help="project directory containing 'All Bin Files' and 'Assets'")
    parser.add_argument("--out", type=Path, default=None,
                        help="output directory for generated files")
    args = parser.parse_args()

    root = args.root or Path(__file__).resolve().parent
    out = args.out or (root / "generated")
    sys.exit(generate(root, out))


if __name__ == "__main__":
    main()
