#!/usr/bin/env python3
"""Regenerate the bundled OFL fallback-font subsets in assets/fonts/ (M6-C).

Sources (host packages, SIL OFL 1.1 — see assets/LICENSES.md):
  /usr/share/fonts/noto/NotoColorEmoji.ttf            (Arch noto-fonts-emoji)
  /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc   (Arch noto-fonts-cjk, face 2 = SC)

Emoji subset: every codepoint in the client's generated emoji table
(src/emoji/emoji_data.c) plus ZWJ/variation-selector/skin-tone/regional-indicator
and tag helpers, so all table emoji and flags shape correctly.

CJK subset: GB2312 common simplified hanzi + CJK punctuation/fullwidth forms,
hiragana/katakana, hangul compatibility jamo and all precomposed hangul
syllables. A single SC face is emitted as .ttf because TTF_OpenFont on a .ttc
always opens face 0 (JP), which would give the wrong family name.

Usage: python3 scripts/subset_fallback_fonts.py [repo_root] [out_dir]
Requires: fontTools (python-fonttools). Outputs are deterministic given the
same source fonts; sha256 sums are recorded in assets/LICENSES.md.
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

# ---- emoji codepoints from the generated table ----
src = (ROOT / "src/emoji/emoji_data.c").read_text()
start = src.index("ccord_emoji_utf8_index[]")
end = src.index("ccord_emoji_utf8_index_count")
section = src[start:end]
cps = set()
for lit in re.findall(r'"((?:\\x[0-9a-fA-F]{2})+)"', section):
    data = bytes(int(h, 16) for h in re.findall(r"\\x([0-9a-fA-F]{2})", lit))
    for ch in data.decode("utf-8"):
        cps.add(ord(ch))
# shaping helpers that may not appear literally in the table
for cp in [0x200D, 0xFE0F, 0xFE0E, 0x20E3, 0x2049, 0x203C]:
    cps.add(cp)
cps.update(range(0x1F3FB, 0x1F400))          # skin tone modifiers
cps.update(range(0x1F1E6, 0x1F200))          # regional indicators
cps.update(range(0xE0061, 0xE007F + 1))      # tag chars for subdivision flags
cps.update(range(0xE0020, 0xE0061))          # remaining tag chars (cheap if present)
print(f"emoji codepoints: {len(cps)}")

# Keep layout features and name records (family name + OFL license text) intact.
opts = subset.Options()
opts.layout_features = ["*"]
opts.name_IDs = ["*"]
opts.notdef_outline = True
opts.recalc_bounds = True
opts.drop_tables = []

f = TTFont("/usr/share/fonts/noto/NotoColorEmoji.ttf", lazy=False)
ss = subset.Subsetter(options=opts)
ss.populate(unicodes=cps)
ss.subset(f)
emoji_out = OUT / "NotoColorEmoji-Subset.ttf"
f.save(emoji_out)
print(f"emoji subset: {emoji_out.stat().st_size} bytes")

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
