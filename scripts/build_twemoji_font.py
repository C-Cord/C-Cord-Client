#!/usr/bin/env python3
"""Build assets/fonts/Twemoji.ttf from the pinned upstream Twemoji COLRv0 font.

Upstream source (verified by sha256):
  * iebb/emojifonts 2026.11 `twemoji-colrv0.ttf` (compiled with nanoemoji from
    jdecked/twemoji v17.0.3 SVGs, Unicode Emoji 17.0, artwork CC-BY 4.0).

Post-processing applied deterministically so SDL3_ttf + FreeType + HarfBuzz
render every qualified and unqualified Unicode 17.0 emoji cleanly:
  1. Restore ASCII digit cmap entries (U+0030..U+0039) to their existing keycap
     base glyphs so 0️⃣..9️⃣ keycap ligatures shape when rendered directly with
     Twemoji (NotoSans owns U+0030..U+0039 in fallback chains).
  2. Drop U+FE0F from cmap and strip uniFE0F from GSUB Type 4 ligatures
     (deduplicated, longest component list first), matching NotoColorEmoji so
     both qualified and unqualified emoji sequences shape identically.
  3. Fit the single oversized glyph (u1F349 watermelon, xMax=1713) into the
     standard 1-em square [38..1238] preserving aspect ratio.
  4. Recompute every contoured glyph's bounding box and set hmtx.lsb = xMin
     (upstream normalize_metrics zeroed all layer LSBs, shifting COLR layers
     under FreeType).
  5. Set balanced vertical metrics (ascent=1015, descent=-260, UPM=1275) so
     glyph ink [-258..965] sits centered inside 1.0 em without clipping.
  6. Populate OpenType name records (family "Twemoji", CC-BY 4.0 attribution)
     and freeze head timestamps for deterministic output.

Usage: python3 scripts/build_twemoji_font.py [--input FILE] [--out FILE]
Requires: fontTools (python-fonttools).
"""
import argparse
import hashlib
import os
import pathlib
import sys
import tempfile
import urllib.request

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables

UPSTREAM_URL = "https://github.com/iebb/emojifonts/releases/download/2026.11/twemoji-colrv0.ttf"
UPSTREAM_SHA256 = "3289c530e0f1f823e1fb9b7d1322a458420c7c1949a75374e3d64c1076b164c4"
UPSTREAM_CACHE_NAME = "twemoji-colrv0-2026.11.ttf"

# Deterministic timestamp: 2026-11-01T00:00:00Z in OpenType epoch (seconds since 1904-01-01).
DETERMINISTIC_OT_TIME = 3876336000


def fetch_upstream(cache_dir: pathlib.Path) -> pathlib.Path:
    path = cache_dir / UPSTREAM_CACHE_NAME
    if path.exists():
        data = path.read_bytes()
    else:
        req = urllib.request.Request(UPSTREAM_URL, headers={"User-Agent": "ccord-build-twemoji"})
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = resp.read()
        cache_dir.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    got = hashlib.sha256(data).hexdigest()
    if got != UPSTREAM_SHA256:
        sys.exit(f"sha256 mismatch for {UPSTREAM_URL}\n  expected {UPSTREAM_SHA256}\n  got      {got}")
    return path


def fix_keycap_cmap(font: TTFont) -> None:
    """Restore U+0030..U+0039 -> keycap base glyphs (glyph00017..glyph00026)."""
    order = set(font.getGlyphOrder())
    keycap_bases = {}
    for lk in font["GSUB"].table.LookupList.Lookup:
        if lk.LookupType != 4:
            continue
        for st in lk.SubTable:
            for first, llist in st.ligatures.items():
                if any("uni20E3" in lig.Component for lig in llist) and first.startswith("glyph"):
                    keycap_bases[first] = True
    sorted_bases = sorted(keycap_bases.keys())
    if len(sorted_bases) != 10:
        sys.exit(f"expected 10 digit keycap base glyphs, found {len(sorted_bases)}: {sorted_bases}")
    for table in font["cmap"].tables:
        if hasattr(table, "cmap") and isinstance(table.cmap, dict):
            for idx, gname in enumerate(sorted_bases):
                if gname not in order:
                    sys.exit(f"missing glyph {gname}")
                table.cmap[0x30 + idx] = gname


def strip_fe0f_from_cmap_and_gsub(font: TTFont) -> None:
    """Drop U+FE0F from cmap and GSUB ligatures so qualified/unqualified match identically."""
    for table in font["cmap"].tables:
        if hasattr(table, "cmap") and isinstance(table.cmap, dict) and 0xFE0F in table.cmap:
            del table.cmap[0xFE0F]
    if "GSUB" not in font:
        return
    for lk in font["GSUB"].table.LookupList.Lookup:
        if lk.LookupType != 4:
            continue
        for st in lk.SubTable:
            new_ligs = {}
            for first, llist in st.ligatures.items():
                seen = {}
                for lig in llist:
                    comp = tuple(c for c in lig.Component if c != "uniFE0F")
                    if not comp:
                        continue
                    if comp not in seen:
                        seen[comp] = lig.LigGlyph
                if seen:
                    out = []
                    for comp in sorted(seen.keys(), key=lambda c: (-len(c), c)):
                        nl = otTables.Ligature()
                        nl.Component = list(comp)
                        nl.LigGlyph = seen[comp]
                        nl.CompCount = len(comp) + 1
                        out.append(nl)
                    new_ligs[first] = out
            st.ligatures = new_ligs


def fit_oversized_colr_glyphs(font: TTFont) -> None:
    """Proportionally fit any COLR glyph whose layers exceed the 1-em square."""
    glyf = font["glyf"]
    colr = font["COLR"].ColorLayers
    upm = font["head"].unitsPerEm
    # Count layer usage so we never mutate a shared layer glyph.
    use_count = {}
    for layers in colr.values():
        for layer in layers:
            use_count[layer.name] = use_count.get(layer.name, 0) + 1

    target_min_x, target_max_x = 38.0, float(upm - 37)  # [38..1238] for UPM=1275
    target_w = target_max_x - target_min_x
    center_x = upm * 0.5
    center_y = 350.0  # midpoint of standard [-250..950]

    for gname, layers in colr.items():
        box_g = [glyf[l.name] for l in layers if glyf[l.name].numberOfContours > 0]
        if not box_g:
            continue
        for g in box_g:
            g.recalcBounds(glyf)
        xmin = min(g.xMin for g in box_g)
        xmax = max(g.xMax for g in box_g)
        ymin = min(g.yMin for g in box_g)
        ymax = max(g.yMax for g in box_g)
        if xmin >= 0 and xmax <= upm and ymin >= -260 and ymax <= 1015:
            continue
        if any(use_count.get(l.name, 0) > 1 for l in layers):
            sys.exit(f"refusing to scale shared layers of oversized glyph {gname}")
        w = float(xmax - xmin)
        h = float(ymax - ymin)
        scale = min(target_w / w, target_w / h)
        cx = (xmin + xmax) * 0.5
        cy = (ymin + ymax) * 0.5
        for l in layers:
            g = glyf[l.name]
            if g.numberOfContours <= 0:
                continue
            coords = g.coordinates
            for i in range(len(coords)):
                px, py = coords[i]
                nx = round((px - cx) * scale + center_x)
                ny = round((py - cy) * scale + center_y)
                coords[i] = (nx, ny)
            g.recalcBounds(glyf)
        # Keep the base bounding glyph in sync too.
        bg = glyf[gname]
        if bg.numberOfContours > 0:
            coords = bg.coordinates
            for i in range(len(coords)):
                px, py = coords[i]
                nx = round((px - cx) * scale + center_x)
                ny = round((py - cy) * scale + center_y)
                coords[i] = (nx, ny)
            bg.recalcBounds(glyf)


def fix_hmtx_and_metrics(font: TTFont) -> None:
    """Restore hmtx.lsb = xMin on every contoured glyph and set balanced vertical metrics."""
    glyf = font["glyf"]
    hmtx = font["hmtx"]
    upm = font["head"].unitsPerEm
    for gname in font.getGlyphOrder():
        g = glyf[gname]
        if g.numberOfContours != 0:
            g.recalcBounds(glyf)
            adv, _ = hmtx[gname]
            hmtx[gname] = (adv, g.xMin)

    # Layer ink lives in [-258..965] for UPM=1275 -> ascent=1015, descent=-260 (sum = 1275 = 1.0 em).
    ascent = 1015
    descent = -(upm - ascent)
    font["hhea"].ascent = ascent
    font["hhea"].descent = descent
    font["hhea"].lineGap = 0
    if "OS/2" in font:
        os2 = font["OS/2"]
        os2.sTypoAscender = ascent
        os2.sTypoDescender = descent
        os2.sTypoLineGap = 0
        os2.usWinAscent = ascent
        os2.usWinDescent = -descent


def set_metadata(font: TTFont) -> None:
    name = font["name"]
    name.names = []
    records = {
        0: "Copyright Twitter, Inc and other contributors; jdecked/twemoji contributors",
        1: "Twemoji",
        2: "Regular",
        3: "Twemoji 17.0.3 (jdecked COLRv0)",
        4: "Twemoji",
        5: "Version 17.0.3",
        6: "Twemoji-Regular",
        11: "https://github.com/jdecked/twemoji",
        13: "Twemoji graphics are licensed under CC-BY 4.0 (https://creativecommons.org/licenses/by/4.0/).",
        14: "https://creativecommons.org/licenses/by/4.0/",
    }
    for name_id, text in records.items():
        # Platform 3 (Windows), encoding 1 (Unicode BMP), language 0x0409 (en-US)
        name.setName(text, name_id, 3, 1, 0x0409)
    font["head"].created = DETERMINISTIC_OT_TIME
    font["head"].modified = DETERMINISTIC_OT_TIME


def verify_font(font: TTFont) -> None:
    glyf = font["glyf"]
    upm = font["head"].unitsPerEm
    for gname, layers in font["COLR"].ColorLayers.items():
        if glyf[gname].numberOfContours == 0:
            sys.exit(f"empty base glyf for COLR glyph {gname}")
        for l in layers:
            g = glyf[l.name]
            if g.numberOfContours == 0:
                continue
            if g.xMin < 0 or g.xMax > upm or g.yMin < -260 or g.yMax > 1015:
                sys.exit(f"layer {l.name} of {gname} out of bounds: {(g.xMin, g.yMin, g.xMax, g.yMax)}")
            _, lsb = font["hmtx"][l.name]
            if lsb != g.xMin:
                sys.exit(f"layer {l.name} lsb mismatch: {lsb} != {g.xMin}")


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", type=pathlib.Path, default=None, help="Local upstream twemoji-colrv0.ttf (verified by sha256)")
    ap.add_argument("--cache", type=pathlib.Path, default=pathlib.Path(os.environ.get("CCORD_EMOJI_CACHE", tempfile.gettempdir())) / "ccord-emoji-cache")
    ap.add_argument("--out", type=pathlib.Path, default=root / "assets/fonts/Twemoji.ttf")
    args = ap.parse_args()

    if args.input is not None:
        got = hashlib.sha256(args.input.read_bytes()).hexdigest()
        if got != UPSTREAM_SHA256:
            sys.exit(f"sha256 mismatch for {args.input}\n  expected {UPSTREAM_SHA256}\n  got      {got}")
        src = args.input
    else:
        src = fetch_upstream(args.cache)

    font = TTFont(str(src), lazy=False, recalcTimestamp=False)
    fix_keycap_cmap(font)
    strip_fe0f_from_cmap_and_gsub(font)
    fit_oversized_colr_glyphs(font)
    fix_hmtx_and_metrics(font)
    set_metadata(font)
    verify_font(font)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(args.out))
    font.close()

    digest = hashlib.sha256(args.out.read_bytes()).hexdigest()
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes) sha256={digest}")


if __name__ == "__main__":
    main()
