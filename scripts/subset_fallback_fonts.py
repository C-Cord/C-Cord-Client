#!/usr/bin/env python3
"""Regenerate the bundled OFL CJK fallback-font subset in assets/fonts/ (M6-C).

(The bundled emoji font is Twemoji COLRv0 — regenerate it with
`python3 scripts/build_twemoji_font.py`.)

Source (host package, SIL OFL 1.1 — see assets/LICENSES.md):
  /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc   (Arch noto-fonts-cjk, face 2 = SC)

CJK subset: GB2312 common simplified hanzi + CJK punctuation/fullwidth forms,
hiragana/katakana, hangul compatibility jamo and all precomposed hangul
syllables. A single SC face is emitted as .ttf because TTF_OpenFont on a .ttc
always opens face 0 (JP), which would give the wrong family name.

Usage: python3 scripts/subset_fallback_fonts.py [repo_root] [out_dir]
Requires: fontTools (python-fonttools). Output is deterministic given the
same source font; sha256 sum is recorded in assets/LICENSES.md.
"""
import gzip
import pathlib
import re
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
OUT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "assets/fonts"
OUT.mkdir(parents=True, exist_ok=True)

# Keep layout features and name records (family name + OFL license text) intact.
opts = subset.Options()
opts.layout_features = ["*"]
opts.name_IDs = ["*"]
opts.notdef_outline = True
opts.recalc_bounds = True
opts.drop_tables = []

# ---- CJK charset ----
cjk = set()
# GB2312 charmap: 6763 common simplified hanzi + CJK punctuation/fullwidth forms
with gzip.open("/usr/share/i18n/charmaps/GB2312.gz", "rt") as fh:
    for line in fh:
        m = re.match(r"<U([0-9A-F]{4,6})>", line)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if (0x2E80 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0xFE30 <= cp <= 0xFE4F
                or 0xFF00 <= cp <= 0xFFEF or 0x3000 <= cp <= 0x33FF):
            cjk.add(cp)
cjk.update(range(0x3041, 0x3100))            # hiragana + katakana
cjk.update(range(0x31F0, 0x3200))            # katakana phonetic extensions
cjk.update(range(0xFF61, 0xFFA0))            # halfwidth katakana
cjk.update(range(0x3130, 0x3190))            # hangul compatibility jamo
cjk.update(range(0xAC00, 0xD7A4))            # all precomposed hangul syllables
print(f"cjk codepoints: {len(cjk)}")

f = TTFont("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", fontNumber=2, lazy=False)
ss = subset.Subsetter(options=opts)
ss.populate(unicodes=cjk)
ss.subset(f)
cjk_out = OUT / "NotoSansCJK-Subset.ttf"
f.save(cjk_out)
print(f"cjk subset: {cjk_out.stat().st_size} bytes")
