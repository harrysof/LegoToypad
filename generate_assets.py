#!/usr/bin/env python3
"""Build-time asset generator for LegoToypad.

Walks the prepared source layout ("All Bin Files/<World>/{Characters,Vehicules,Logo}"
plus the "Assets" tree, which is organized into category folders:
Wallpapers, Pads, Tiles, Buttons, Branding and Fonts) and emits, into the
output directory:

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
import csv
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
APP_VERSION = "1.8.0"
APP_VERSION_COMMA = "1,8,0,0"
APP_COMPANY = "HarrysofXD"
APP_DESCRIPTION = "Controller-driven LEGO Dimensions Toypad companion for Cemu, RPCS3, shadPS4 and Xenia"

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
CHARACTER_VEHICLE_RE = re.compile(r"^(.+?) - (\d+)[.\-]\s*(.+)$")


def split_vehicle_stem(stem):
    """Returns (owner, build_number, base_name).

    Prepending the owning character ("<Character> - 1. Jakemobile") lets the
    generator group every character's vehicles together. The owner is returned
    as None when the stem has not been renamed (legacy "1. Jakemobile" style)
    or unnumbered. Unnumbered files become build 1.
    """
    renamed = CHARACTER_VEHICLE_RE.match(stem)
    if renamed:
        return renamed.group(1).strip(), int(renamed.group(2)), renamed.group(3).strip()
    legacy = VEHICLE_PREFIX_RE.match(stem)
    if legacy:
        return None, int(legacy.group(1)), legacy.group(2).strip()
    return None, 1, stem.strip()


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


# Subfolders that only hold superseded layouts and must never be scanned for
# active assets (e.g. the legacy "Old" collection of pad/button art).
SKIPPED_ASSET_DIRS = {"old", "legacy", ".old", "unused", "archive"}


def asset_files(assets_root: Path, suffixes):
    """Recursive scan of the Assets tree. The tree is organized into category
    folders (Wallpapers, Pads, Tiles, Buttons, Branding, Fonts), so the active
    assets are gathered from every subfolder while legacy/archive folders are
    skipped. Catches images dropped straight into a category folder and keeps
    the .rc/.cpp output identical regardless of which folder holds a file."""
    out = []
    if not assets_root or not assets_root.is_dir():
        return out
    for child in assets_root.rglob("*"):
        if not child.is_file() or child.stat().st_size == 0:
            continue
        if child.suffix.lower() not in suffixes:
            continue
        parents = [p.casefold() for p in child.relative_to(assets_root).parts[:-1]]
        if any(p in SKIPPED_ASSET_DIRS for p in parents):
            continue
        out.append(child)
    return sorted(out, key=lambda p: str(p).casefold())


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
# Vehicle ownership / family grouping
# ---------------------------------------------------------------------------
# The source of truth for which character owns which vehicle, and which of a
# vehicle's two alternate builds belong with it, is the hand-researched
# vehicles.csv at the project root (World, Character, Build, Vehicle, Family).
# The generator reads it purely to group each character's vehicles under the
# multi-build "Family" so the in-app "+ Build picker" lists Build 1/2/3 of one
# vehicle together, and to order each world's vehicles by its characters. The
# rename that produces the "<Character> - 1. Name" stems is what the app sorts
# on; the CSV only supplies the family (base-build) association, which the
# filename cannot carry.

def load_vehicle_families(csv_path):
    """Returns {world_fold: {vehicle_name_fold: (character, family_base)}}."""
    families = {}
    if not csv_path or not csv_path.is_file():
        return families
    try:
        with open(csv_path, "r", newline="", encoding="utf-8") as fp:
            reader = csv.reader(fp)
            header = next(reader, None)
            for row in reader:
                if len(row) < 5:
                    continue
                world, character, _build, vehicle, family = row[:5]
                world_fold = world.strip().casefold()
                vehicle_fold = vehicle.strip().casefold()
                families.setdefault(world_fold, {})[vehicle_fold] = (
                    character.strip(), family.strip())
    except OSError as exc:
        return families
    return families


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

    # (world folder -> {vehicle name -> (character, family base)}) used to group
    # each character's vehicle builds (the "family") and order vehicles by
    # character. Optional: without it every build becomes its own group.
    vehicle_families = load_vehicle_families(root / "vehicles.csv")

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
        # Grouped by (owning character, family base) so each of a character's
        # vehicles appears as one multi-build group (Build 1/2/3 together) and
        # the world's vehicles are ordered by the characters that own them.
        veh_groups = {}
        veh_folder = discover_case_insensitive(world, "Vehicules")
        veh_bins = [f for f in (veh_folder.iterdir() if veh_folder else [])
                    if f.is_file() and f.suffix.lower() == ".bin"]
        world_families = vehicle_families.get(world.name.casefold(), {})
        for binary, portrait in pair_bins_and_images(veh_bins, image_files(veh_folder), warnings):
            owner, build, name = split_vehicle_stem(binary.stem)
            name_id = sanitize(name)
            bin_name, png_name = make_resource_names(world_id, "VEH", name_id, build)
            bin_sym = symbols.allocate(bin_name, binary)
            png_sym = symbols.allocate(png_name, portrait) if portrait else alloc(png_name)
            if portrait:
                ascii_ok([str(binary), str(portrait)], warnings)

            owner_key = owner.casefold() if owner else ""
            family = name  # fallback: the build is its own group
            owned = world_families.get(name.casefold())
            if owned is not None and (not owner_key or owned[0].casefold() == owner_key):
                family = owned[1]
            group_key = (owner_key, family.casefold())
            group = veh_groups.get(group_key)
            if group is None:
                group = {"character": owner or "", "base": family, "builds": []}
                veh_groups[group_key] = group
            group["builds"].append({
                "name": name, "bin": bin_sym, "png": png_sym, "build": build,
            })

        vehicles = []
        # Order by owning character (alphabetical, matching the roster order),
        # then by the vehicle family's base name, then builds in build order.
        for group_key in sorted(veh_groups.keys()):
            group = veh_groups[group_key]
            group["builds"] = sorted(group["builds"], key=lambda e: e["build"])
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
    # The Assets tree is organized into category folders (Wallpapers, Pads,
    # Tiles, Buttons, Branding, Fonts); asset_files() walks it recursively so
    # the lookup helpers below find each asset wherever it lives.
    assets_files = asset_files(assets_root, (".png", ".jpg", ".jpeg"))

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

    # Pad art lives in Assets/Pads/<skin>/, one folder per selectable skin and
    # seven PNGs inside it (one per toypad slot, named after the slot). The
    # folder called "default" is the built-in look and always sorts first; any
    # other folder is an extra skin the Settings screen can switch to on the
    # fly. A folder missing one of the seven files is skipped with a warning
    # rather than producing a skin with holes in it.
    PAD_SLOT_STEMS = [
        ("LEFT_UPPER", "left_upper"),
        ("CENTER", "center"),
        ("RIGHT_UPPER", "right_upper"),
        ("LEFT_LOWER_LEFT", "left_lower_left"),
        ("LEFT_LOWER_RIGHT", "left_lower_right"),
        ("RIGHT_LOWER_LEFT", "right_lower_left"),
        ("RIGHT_LOWER_RIGHT", "right_lower_right"),
    ]

    def pad_skin_label(folder_name):
        if folder_name.casefold() == "default":
            return "Default"
        pretty = re.sub(r"[_\-]+", " ", folder_name).strip()
        return pretty[:1].upper() + pretty[1:] if pretty else folder_name

    pads_root = discover_case_insensitive(assets_root, "Pads")
    pad_skins = []
    if pads_root:
        # SKIPPED_ASSET_DIRS is deliberately NOT applied here: it exists to
        # keep the unrelated top-level Assets/Old folder (a dump of
        # superseded UI art) out of the generic asset scanner, not to judge
        # skin folders by name. A folder under Assets/Pads is a skin - even
        # one named "Old" - purely by whether it has the seven pad images;
        # excluding it by name once made "Assets/Pads/Old" silently invisible.
        skin_dirs = [d for d in sorted(pads_root.iterdir(), key=lambda p: p.name.casefold())
                     if d.is_dir()]
        # "default" first, everything else alphabetically after it.
        skin_dirs.sort(key=lambda d: (0 if d.name.casefold() == "default" else 1, d.name.casefold()))
        for skin_dir in skin_dirs:
            by_stem = {p.stem.casefold(): p for p in image_files(skin_dir)}
            row = []
            missing = []
            for slot_name, stem in PAD_SLOT_STEMS:
                file = by_stem.get(stem)
                if file is None:
                    missing.append(stem)
                    row.append(None)
                    continue
                ascii_ok([str(file)], warnings)
                row.append(symbols.allocate(
                    "ASSET_PAD_BG_%s_%s" % (sanitize(skin_dir.name), slot_name), file))
            if missing:
                warnings.append("pad skin '%s' is missing %s; skipped"
                                % (skin_dir.name, ", ".join(m + ".png" for m in missing)))
                continue
            pad_skins.append({"label": pad_skin_label(skin_dir.name), "symbols": row})
    if not pad_skins:
        warnings.append("no complete pad skin found under Assets/Pads/<skin>/ "
                        "(expected %s)" % ", ".join(s + ".png" for _, s in PAD_SLOT_STEMS))
    # kPadBackgroundResourceIds stays the first (default) skin, so anything
    # that wants the built-in art without going through the skin table still
    # has it.
    pad_bg_syms = pad_skins[0]["symbols"] if pad_skins else [None] * 7

    world_tile = pick_named({"world_tile"}, "world tile background")
    characters_tile = pick_named({"characters_tile"}, "characters tile background")
    # Optional: falls back to characters_tile at runtime (see
    # kSettingsTileResourceId in main.cpp) if this file isn't present, so an
    # older Assets tree without it still builds.
    settings_tile = None
    for candidate in assets_files:
        if candidate.stem.casefold() == "settings_tile":
            settings_tile = candidate
            break
    world_tile_sym = symbols.allocate("ASSET_WORLD_TILE", world_tile) if world_tile else None
    characters_tile_sym = symbols.allocate("ASSET_CHARACTERS_TILE", characters_tile) if characters_tile else None
    settings_tile_sym = symbols.allocate("ASSET_SETTINGS_TILE", settings_tile) if settings_tile else None
    if world_tile:
        ascii_ok([str(world_tile)], warnings)
    if characters_tile:
        ascii_ok([str(characters_tile)], warnings)
    if settings_tile:
        ascii_ok([str(settings_tile)], warnings)

    # ---- action button assets ---------------------------------------------
    textfield_bar   = None
    load_button     = pick_named({"load_button", "load"},     "load button")
    clear_button    = pick_named({"clear_button", "clear"},   "clear button")
    move_button     = pick_named({"move_button", "move"},     "move button")
    scroll_bar      = pick_named({"scroll_bar"},      "scroll bar")
    by_harrysof     = pick_named({"by_harrysof"},      "by harrysof watermark")
    y_button        = pick_named({"y_button", "ybutton"}, "y button hint")
    settings_text   = pick_named({"settings_text", "settings"}, "settings hint text")
    textfield_bar_sym  = symbols.allocate("ASSET_TEXTFIELD_BAR",  textfield_bar)  if textfield_bar  else None
    load_button_sym    = symbols.allocate("ASSET_LOAD_BUTTON",    load_button)    if load_button    else None
    clear_button_sym   = symbols.allocate("ASSET_CLEAR_BUTTON",   clear_button)   if clear_button   else None
    move_button_sym    = symbols.allocate("ASSET_MOVE_BUTTON",    move_button)    if move_button    else None
    scroll_bar_sym     = symbols.allocate("ASSET_SCROLL_BAR",     scroll_bar)     if scroll_bar     else None
    by_harrysof_sym    = symbols.allocate("ASSET_BY_HARRYSOF",    by_harrysof)    if by_harrysof    else None
    y_button_sym       = symbols.allocate("ASSET_Y_BUTTON",       y_button)       if y_button       else None
    settings_text_sym  = symbols.allocate("ASSET_SETTINGS_TEXT",  settings_text)  if settings_text  else None
    for name, asset in [("load_button", load_button), ("clear_button", clear_button),
                        ("move_button", move_button), ("scroll_bar", scroll_bar),
                        ("by_harrysof", by_harrysof),
                        ("y_button", y_button), ("settings_text", settings_text)]:
        if asset:
            ascii_ok([str(asset)], warnings)
        else:
            warnings.append("missing action-bar asset: %s" % name)

    # ---- franchise-sort badges ---------------------------------------------
    # Word-art name plates shown at top-centre of the browse screens, one per
    # sort mode (Default / User / Starter). Favorites has no name plate - that
    # roster shows the app wordmark enlarged instead (see DrawSortBadge in
    # main.cpp).
    sort_default  = pick_named({"default_sort"}, "default sort badge")
    sort_user     = pick_named({"user_sort"},    "user sort badge")
    sort_starter  = pick_named({"starter_sort"}, "starter sort badge")
    sort_default_sym = symbols.allocate("ASSET_SORT_DEFAULT", sort_default) if sort_default else None
    sort_user_sym    = symbols.allocate("ASSET_SORT_USER",    sort_user)    if sort_user    else None
    sort_starter_sym = symbols.allocate("ASSET_SORT_STARTER", sort_starter) if sort_starter else None
    for name, asset in [("default_sort", sort_default), ("user_sort", sort_user),
                        ("starter_sort", sort_starter)]:
        if asset:
            ascii_ok([str(asset)], warnings)
        else:
            warnings.append("missing sort badge asset: %s" % name)

    # ---- custom-bin icon ---------------------------------------------------
    # Portrait art stamped on every captured custom tag (Assets/custombins).
    custom_bin_icon = pick_named({"custom_bin", "custombin"}, "custom bin icon")
    custom_bin_icon_sym = symbols.allocate("ASSET_CUSTOM_BIN_ICON", custom_bin_icon) if custom_bin_icon else None
    if custom_bin_icon:
        ascii_ok([str(custom_bin_icon)], warnings)

    # ---- UI sound effects --------------------------------------------------
    # Assets/SFX/Navigate.wav and Select.wav, embedded verbatim and played
    # from memory with PlaySound(SND_MEMORY). Missing files just mean that
    # sound never plays; the Settings toggle still works.
    sfx_files = asset_files(assets_root, (".wav",))
    sfx_by_stem = {p.stem.casefold(): p for p in sfx_files}

    def pick_sfx(stem, what):
        file = sfx_by_stem.get(stem)
        if file is None:
            warnings.append("missing %s sound (expected Assets/SFX/%s.wav)" % (what, stem))
            return None
        ascii_ok([str(file)], warnings)
        return file

    sfx_navigate = pick_sfx("navigate", "navigation")
    sfx_select = pick_sfx("select", "select")
    sfx_move = pick_sfx("move", "move")
    sfx_remove = pick_sfx("remove", "remove")
    sfx_navigate_sym = symbols.allocate("ASSET_SFX_NAVIGATE", sfx_navigate) if sfx_navigate else None
    sfx_select_sym = symbols.allocate("ASSET_SFX_SELECT", sfx_select) if sfx_select else None
    sfx_move_sym = symbols.allocate("ASSET_SFX_MOVE", sfx_move) if sfx_move else None
    sfx_remove_sym = symbols.allocate("ASSET_SFX_REMOVE", sfx_remove) if sfx_remove else None

    # ---- controller button icons ------------------------------------------
    # Assets/ControllerIcons/<Style>/<Button>.png: one folder per pad style,
    # one file per button, both in the fixed orders below (the button order
    # matches kButtonNames in main.cpp). A missing file is left as id 0 and
    # the app draws that button's name instead, so an incomplete icon set
    # degrades one button at a time rather than breaking the build.
    controller_icon_styles = ["Xbox", "DualShock4", "Switch"]
    controller_icon_buttons = [
        "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Start", "Back",
        "LeftStickClick", "RightStickClick", "LB", "RB", "LT", "RT",
        "A", "B", "X", "Y",
    ]
    controller_icons_root = discover_case_insensitive(assets_root, "ControllerIcons")
    controller_icon_syms = []
    for style in controller_icon_styles:
        style_dir = discover_case_insensitive(controller_icons_root, style) if controller_icons_root else None
        by_stem = {p.stem.casefold(): p for p in image_files(style_dir)} if style_dir else {}
        row = []
        for button in controller_icon_buttons:
            file = by_stem.get(button.casefold())
            if file is None:
                warnings.append("missing controller icon: ControllerIcons/%s/%s.png" % (style, button))
                row.append(None)
                continue
            ascii_ok([str(file)], warnings)
            row.append(symbols.allocate(
                "ASSET_PADICON_%s_%s" % (sanitize(style), sanitize(button)), file))
        controller_icon_syms.append(row)

    # ---- UI font -----------------------------------------------------------
    # The Compacta-typeface font embedded for GDI/GDI+ in-memory loading.
    # GDI/GDI+ only consume raw SFNT (.ttf/.otf) bytes in memory, so web-font
    # formats (.woff/.woff2) are decompressed here at build time and staged
    # into the output dir; the runtime app path is unchanged.
    ui_font = None
    font_rc_path = None
    font_candidates = asset_files(assets_root, (".ttf", ".otf", ".woff", ".woff2"))
    if font_candidates:
        # A raw .ttf/.otf outranks a web format: it is what GDI and GDI+ load
        # directly, and it needs no build-time decompression step (which in
        # turn needs fonttools + brotli installed). Within a group the order
        # is the stable alphabetical one asset_files already produced, so the
        # choice never depends on directory iteration order.
        def font_rank(path):
            return 0 if path.suffix.lower() in (".ttf", ".otf") else 1

        font_candidates.sort(key=font_rank)
        ui_font = font_candidates[0]
        if len(font_candidates) > 1:
            warnings.append("multiple fonts in Assets, using %s" % ui_font.name)

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

    # The same face in original web format (woff/woff2) is embedded separately
    # so the phone UI can load the compact typeface without a multi-megabyte
    # raw ttf download. Served straight from the resource bytes; the HTTP
    # layer sniffs the magic to pick the right content type.
    ui_font_web_sym = None
    if ui_font and ui_font.suffix.lower() in (".woff", ".woff2"):
        ui_font_web_sym = symbols.allocate("ASSET_UI_FONT_WEB", ui_font)
        ascii_ok([str(ui_font)], warnings)

    # ---- web remote UI -----------------------------------------------------
    # Files in the "Web" folder (index.html / style.css / app.js) are embedded
    # verbatim as RCDATA resources and handed to the built-in HTTP server via
    # the kWebFiles table below. Missing folder just means no remote UI is
    # bundled; the server still answers the API endpoints.
    web_files = []
    web_root = discover_case_insensitive(root, "Web")
    if web_root and web_root.is_dir():
        for web_file in sorted(web_root.iterdir(), key=lambda p: p.name.casefold()):
            if not web_file.is_file() or web_file.stat().st_size == 0:
                continue
            resource_name = "WEB_%s" % sanitize(web_file.name)
            sym = symbols.allocate(resource_name, web_file)
            ascii_ok([str(web_file)], warnings)
            web_files.append({"name": web_file.name, "symbol": sym})
    else:
        warnings.append("no Web folder found (web remote UI will not be bundled)")

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
    header.append("struct PadSkin")
    header.append("{")
    header.append("    std::wstring name;")
    header.append("    int slotResourceIds[7];  // kPadCells order")
    header.append("};")
    header.append("")
    header.append("struct WebFile")
    header.append("{")
    header.append("    const char* name;")
    header.append("    int resourceId;")
    header.append("};")
    header.append("")
    header.append("extern const Franchise kFranchises[];")
    header.append("extern const size_t kFranchiseCount;")
    header.append("extern const BackgroundChoice kBackgroundChoices[];")
    header.append("extern const size_t kBackgroundChoiceCount;")
    header.append("extern const WebFile kWebFiles[];")
    header.append("extern const size_t kWebFileCount;")
    header.append("extern const int kWordmarkResourceId;")
    header.append("extern const int kBackgroundResourceId;")
    header.append("extern const int kAppIconResourceId;")
    header.append("extern const int kUIFontResourceId;")
    header.append("extern const int kUIFontWebResourceId;")
    header.append("extern const int kWorldTileResourceId;")
    header.append("extern const int kCharactersTileResourceId;")
    header.append("extern const int kSettingsTileResourceId;")
    header.append("extern const int kPadBackgroundResourceIds[7];")
    header.append("extern const PadSkin kPadSkins[];")
    header.append("extern const size_t kPadSkinCount;")
    header.append("extern const int kCustomBinIconResourceId;")
    header.append("extern const int kSfxNavigateResourceId;")
    header.append("extern const int kSfxSelectResourceId;")
    header.append("extern const int kSfxMoveResourceId;")
    header.append("extern const int kSfxRemoveResourceId;")
    header.append("extern const int kTextfieldBarResourceId;")
    header.append("extern const int kLoadButtonResourceId;")
    header.append("extern const int kClearButtonResourceId;")
    header.append("extern const int kMoveButtonResourceId;")
    header.append("extern const int kScrollBarResourceId;")
    header.append("extern const int kByHarrysofResourceId;")
    header.append("extern const int kYButtonResourceId;")
    header.append("extern const int kSettingsTextResourceId;")
    header.append("extern const int kSortDefaultResourceId;")
    header.append("extern const int kSortUserResourceId;")
    header.append("extern const int kSortStarterResourceId;")
    header.append("// Single source of truth for the app version shown in the UI (web")
    header.append("// catalog's \"version\" field) - kept in lockstep with APP_VERSION")
    header.append("// below, which also stamps the exe's own FILEVERSION/ProductVersion.")
    header.append("extern const wchar_t kAppVersion[];")
    header.append("// [style][button]; style order Xbox, DualShock4, Switch, button")
    header.append("// order as in kButtonNames. 0 = no icon bundled for that button.")
    header.append("extern const int kControllerIconResourceIds[%d][%d];"
                  % (len(controller_icon_styles), len(controller_icon_buttons)))
    header.append("extern const size_t kControllerIconStyleCount;")
    header.append("extern const size_t kControllerIconButtonCount;")
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
    cpp.append("const int kUIFontWebResourceId = %s;" % (ui_font_web_sym["name"] if ui_font_web_sym else "0"))
    cpp.append("const int kWorldTileResourceId = %s;" % (world_tile_sym["name"] if world_tile_sym else "0"))
    cpp.append("const int kCharactersTileResourceId = %s;" % (characters_tile_sym["name"] if characters_tile_sym else "0"))
    cpp.append("const int kSettingsTileResourceId = %s;" % (settings_tile_sym["name"] if settings_tile_sym else "0"))
    cpp.append("const int kPadBackgroundResourceIds[7] = { %s };"
               % ", ".join(sym["name"] if sym else "0" for sym in pad_bg_syms))
    cpp.append("const int kCustomBinIconResourceId = %s;" % (custom_bin_icon_sym["name"] if custom_bin_icon_sym else "0"))
    cpp.append("const int kSfxNavigateResourceId = %s;" % (sfx_navigate_sym["name"] if sfx_navigate_sym else "0"))
    cpp.append("const int kSfxSelectResourceId = %s;" % (sfx_select_sym["name"] if sfx_select_sym else "0"))
    cpp.append("const int kSfxMoveResourceId = %s;" % (sfx_move_sym["name"] if sfx_move_sym else "0"))
    cpp.append("const int kSfxRemoveResourceId = %s;" % (sfx_remove_sym["name"] if sfx_remove_sym else "0"))
    cpp.append("const int kTextfieldBarResourceId = %s;" % (textfield_bar_sym["name"] if textfield_bar_sym else "0"))
    cpp.append("const int kLoadButtonResourceId = %s;" % (load_button_sym["name"] if load_button_sym else "0"))
    cpp.append("const int kClearButtonResourceId = %s;" % (clear_button_sym["name"] if clear_button_sym else "0"))
    cpp.append("const int kMoveButtonResourceId = %s;" % (move_button_sym["name"] if move_button_sym else "0"))
    cpp.append("const int kScrollBarResourceId = %s;" % (scroll_bar_sym["name"] if scroll_bar_sym else "0"))
    cpp.append("const int kByHarrysofResourceId = %s;" % (by_harrysof_sym["name"] if by_harrysof_sym else "0"))
    cpp.append("const int kYButtonResourceId = %s;" % (y_button_sym["name"] if y_button_sym else "0"))
    cpp.append("const int kSettingsTextResourceId = %s;" % (settings_text_sym["name"] if settings_text_sym else "0"))
    cpp.append("const int kSortDefaultResourceId = %s;" % (sort_default_sym["name"] if sort_default_sym else "0"))
    cpp.append("const int kSortUserResourceId = %s;" % (sort_user_sym["name"] if sort_user_sym else "0"))
    cpp.append("const int kSortStarterResourceId = %s;" % (sort_starter_sym["name"] if sort_starter_sym else "0"))
    cpp.append("const wchar_t kAppVersion[] = L\"%s\";" % APP_VERSION)
    cpp.append("")
    cpp.append("const int kControllerIconResourceIds[%d][%d] = {"
               % (len(controller_icon_styles), len(controller_icon_buttons)))
    for style, row in zip(controller_icon_styles, controller_icon_syms):
        cpp.append("    { %s }, // %s"
                   % (", ".join(sym["name"] if sym else "0" for sym in row), style))
    cpp.append("};")
    cpp.append("const size_t kControllerIconStyleCount = %d;" % len(controller_icon_styles))
    cpp.append("const size_t kControllerIconButtonCount = %d;" % len(controller_icon_buttons))
    cpp.append("")
    cpp.append("const PadSkin kPadSkins[] = {")
    # An empty initializer list is not valid C++, so a build with no complete
    # skin folder still emits one (empty) entry and the app falls back to
    # drawing bare pad outlines.
    for skin in (pad_skins or [{"label": "Default", "symbols": [None] * 7}]):
        cpp.append("    { %s, { %s } }," % (wstr(skin["label"]),
                   ", ".join(sym["name"] if sym else "0" for sym in skin["symbols"])))
    cpp.append("};")
    cpp.append("const size_t kPadSkinCount = sizeof(kPadSkins) / sizeof(kPadSkins[0]);")
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
    cpp.append("const WebFile kWebFiles[] = {")
    for entry in sorted(web_files, key=lambda w: w["name"].casefold()):
        cpp.append('    { "%s", %s },' % (entry["name"].replace("\\", "\\\\").replace('"', '\\"'),
                                          entry["symbol"]["name"]))
    cpp.append("};")
    cpp.append("const size_t kWebFileCount = sizeof(kWebFiles) / sizeof(kWebFiles[0]);")
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

        # Trim the transparent margins around the logo art before building the
        # ICO frames. The raw logo PNG carries generous padding (and stray low-alpha
        # artifact pixels that fool naive getbbox()). We use an alpha threshold
        # to find the real artwork bounding box, crop it, and place it centered
        # onto a square canvas with a minimal breathing margin so each ICO frame
        # (16x16, 24x24, 32x32, 48x48, 256x256) is properly maximized and crisp.
        if src.mode not in ("RGBA", "LA"):
            src = src.convert("RGBA")
        alpha = src.split()[-1]
        mask = alpha.point(lambda p: 255 if p > 30 else 0)
        bbox = mask.getbbox()
        if bbox:
            cropped = src.crop(bbox)
            max_dim = max(cropped.width, cropped.height)
            margin = max(2, int(max_dim * 0.02))
            canvas_dim = max_dim + margin * 2
            square = Image.new("RGBA", (canvas_dim, canvas_dim), (0, 0, 0, 0))
            square.paste(cropped, ((canvas_dim - cropped.width) // 2, (canvas_dim - cropped.height) // 2))
            src = square

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
