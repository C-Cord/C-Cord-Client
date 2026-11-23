#!/usr/bin/env python3
"""Generates src/emoji/emoji_data.c from pinned upstream sources.

Sources (every download is verified against the sha256 below; nothing is fetched unpinned):
  * Unicode emoji-test.txt, Emoji 17.0   -> order, groups, CLDR names, skin-tone variants,
                                            unqualified / minimally-qualified spellings
  * emojibase-data 17.0.0 (npm, via jsDelivr) shortcode sets joypixels / iamcal / github
                                         -> shortcodes (joypixels first: closest to Discord's names)

Usage: scripts/gen_emoji_data.py [--cache DIR] [--out FILE]
Downloads are cached in DIR (default: $CCORD_EMOJI_CACHE or <tmp>/ccord-emoji-cache); the cache is never
committed. Re-running with the same pins produces a byte-identical output file.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import unicodedata
import urllib.request

UNICODE_BASE = "https://www.unicode.org/Public/17.0.0/emoji/"
EMOJIBASE = "https://cdn.jsdelivr.net/npm/emojibase-data@17.0.0/"

SOURCES = {
    "emoji-test.txt": (
        UNICODE_BASE + "emoji-test.txt",
        "1d8a944f88d7952f7ef7c5167fef3c67995bcae24543949710231b03a201acda",
    ),
    "joypixels.json": (
        EMOJIBASE + "en/shortcodes/joypixels.json",
        "71c0608ffb9715b0bcd7dcc5001218fee07ddbd95635e328a392b9cc07a70ab8",
    ),
    "iamcal.json": (
        EMOJIBASE + "en/shortcodes/iamcal.json",
        "b1952bd90cb8580224434eaf8f1911265d23a78c7ed36f39f8590c90847443eb",
    ),
    "github.json": (
        EMOJIBASE + "en/shortcodes/github.json",
        "d6bb7101e7bab1e52b30f4b3562da2319b484a6266352a1119f0d1ba52db23db",
    ),
}

# Discord's picker order. Smileys & Emotion + People & Body are merged into "People"; Component is skipped.
CATEGORIES = ["People", "Nature", "Food", "Activities", "Travel", "Objects", "Symbols", "Flags"]
GROUP_TO_CATEGORY = {
    "Smileys & Emotion": 0,
    "People & Body": 0,
    "Animals & Nature": 1,
    "Food & Drink": 2,
    "Activities": 3,
    "Travel & Places": 4,
    "Objects": 5,
    "Symbols": 6,
    "Flags": 7,
}
TONES = ["1F3FB", "1F3FC", "1F3FD", "1F3FE", "1F3FF"]
FE0F = "FE0F"
NAME_RE = re.compile(r"^[a-z0-9_+-]+$")


def fetch(name, cache):
    url, want = SOURCES[name]
    path = os.path.join(cache, name)
    if os.path.exists(path):
        data = open(path, "rb").read()
    else:
        req = urllib.request.Request(url, headers={"User-Agent": "ccord-gen-emoji-data"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        sys.exit(f"sha256 mismatch for {url}\n  expected {want}\n  got      {got}")
    os.makedirs(cache, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data


def parse_emoji_test(text):
    """Returns list of (codepoint-hex-list, status, group, cldr_name) in file order (components skipped)."""
    out = []
    group = None
    rx = re.compile(r"^([0-9A-F ]+?)\s*;\s*([a-z-]+)\s*#\s*\S+\s+E[\d.]+\s+(.*)$")
    for line in text.splitlines():
        if line.startswith("# group:"):
            group = line.split(":", 1)[1].strip()
            continue
        m = rx.match(line)
        if not m:
            continue
        status = m.group(2)
        if status == "component" or group == "Component":
            continue
        out.append((m.group(1).split(), status, group, m.group(3).strip()))
    return out


def strip_key(cps):
    """Identity key: drop emoji-presentation selectors and skin-tone modifiers."""
    return tuple(c for c in cps if c != FE0F and c not in TONES)


def feff_key(cps):
    return tuple(c for c in cps if c != FE0F)


def to_utf8(cps):
    return "".join(chr(int(c, 16)) for c in cps).encode("utf-8")


def c_string(b):
    return '"' + "".join("\\x%02x" % x for x in b) + '"'


def c_text(s):
    """C literal for UTF-8 text: printable ASCII verbatim, other bytes as (3-digit, so unambiguous) octal."""
    out = []
    for x in s.encode("utf-8"):
        ch = chr(x)
        if 0x20 <= x < 0x7F and ch not in '"\\':
            out.append(ch)
        else:
            out.append("\\%03o" % x)
    return '"' + "".join(out) + '"'


def c_ascii(s):
    assert re.fullmatch(r"[ -~]*", s) and '"' not in s and "\\" not in s, s
    return '"' + s + '"'


def c_strtab_byte(x):
    if x == 0:
        return "0"
    if 0x20 <= x < 0x7F and chr(x) not in "'\\":
        return "'%c'" % x
    return "'\\x%02x'" % x


def tone_of(cps):
    """Returns tone index 1..5 if ALL tone modifiers in the sequence are the same tone, 0 if none, -1 mixed."""
    ts = [c for c in cps if c in TONES]
    if not ts:
        return 0
    if len(set(ts)) != 1:
        return -1
    return TONES.index(ts[0]) + 1


def slugify(name):
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode("ascii")
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "emoji"


def as_list(v):
    if v is None:
        return []
    return [v] if isinstance(v, str) else list(v)


def normalise_names(raw, from_iamcal):
    """Lower-case a shortcode list. Interior '-' becomes '_' (Discord style) and, except for iamcal (which
    only uses dashes as separators), the dashed spelling is kept as a later alias."""
    names = []
    for n in raw:
        n = n.lower()
        if not NAME_RE.match(n):
            continue
        under = n[0] + n[1:].replace("-", "_") if n.startswith("-") else n.replace("-", "_")
        names.append(under)
        if under != n and not from_iamcal:
            names.append(n)
    return names


def build(cache):
    test = parse_emoji_test(fetch("emoji-test.txt", cache).decode("utf-8"))
    sc = {k: json.loads(fetch(k + ".json", cache)) for k in ("joypixels", "iamcal", "github")}

    # 1. Base emoji: fully-qualified, no tone modifiers, in Discord category order (stable).
    bases = []  # dicts
    by_strip = {}
    for cps, status, group, name in test:
        if status != "fully-qualified" or any(c in TONES for c in cps):
            continue
        e = {"cps": cps, "name": name, "cat": GROUP_TO_CATEGORY[group], "tones": [None] * 5}
        k = strip_key(cps)
        if k in by_strip:
            sys.exit("duplicate identity key for " + name)
        bases.append(e)
        by_strip[k] = e
    bases = sorted(bases, key=lambda e: e["cat"])  # sorted() is stable: keeps Unicode order in a category
    for i, e in enumerate(bases):
        e["idx"] = i

    # 2. Uniform skin tone variants of the fully-qualified set; all other spellings of base/toned forms.
    spellings = {}  # utf8 bytes -> (entry idx, tone)

    def add_spelling(cps, e, tone):
        b = to_utf8(cps)
        prev = spellings.get(b)
        if prev is not None and prev != (e["idx"], tone):
            sys.exit(f"ambiguous spelling {cps}: {prev} vs {(e['idx'], tone)}")
        spellings[b] = (e["idx"], tone)

    for cps, status, group, name in test:
        t = tone_of(cps)
        if t < 0:
            continue
        e = by_strip.get(strip_key(cps))
        if e is None:
            if status == "fully-qualified":
                sys.exit("toned/odd fully-qualified without base: " + name)
            continue
        if status == "fully-qualified":
            if t == 0:
                add_spelling(cps, e, 0)
            else:
                if e["tones"][t - 1] is not None:
                    sys.exit("duplicate tone variant for " + name)
                e["tones"][t - 1] = to_utf8(cps)
                add_spelling(cps, e, t)
        else:
            add_spelling(cps, e, t)
    # also the plain FE0F-less spelling of every default / toned sequence
    for cps, status, group, name in test:
        t = tone_of(cps)
        if t < 0 or status != "fully-qualified":
            continue
        e = by_strip.get(strip_key(cps))
        add_spelling(list(feff_key(cps)), e, t)

    for e in bases:
        have = sum(1 for t in e["tones"] if t is not None)
        if have not in (0, 5):
            sys.exit(f"{e['name']}: {have} tone variants")
        e["has_tones"] = have == 5

    # 3. Shortcodes. Primaries first (so an alias never steals another emoji's primary), then aliases.
    def candidates(e):
        h = "-".join(e["cps"])
        h2 = "-".join(c for c in e["cps"] if c != FE0F)
        out = []
        for src in ("joypixels", "iamcal", "github"):
            raw = None
            for k in (h, h2):
                if k in sc[src]:
                    raw = sc[src][k]
                    break
            out.append(normalise_names(as_list(raw), src == "iamcal"))
        return out

    claimed = {}
    cand = [candidates(e) for e in bases]
    for e, c in zip(bases, cand):
        e["name_sc"] = None
        e["aliases"] = []
        flat = [n for lst in c for n in lst]
        for n in flat:
            if n not in claimed:
                claimed[n] = e["idx"]
                e["name_sc"] = n
                break
    for e in bases:
        if e["name_sc"] is None:
            base = slugify(e["name"])
            n, i = base, 2
            while n in claimed:
                n = f"{base}_{i}"
                i += 1
            claimed[n] = e["idx"]
            e["name_sc"] = n
            e["slug"] = True
    for e, c in zip(bases, cand):
        for n in [x for lst in c for x in lst]:
            if n not in claimed:
                claimed[n] = e["idx"]
                e["aliases"].append(n)
    return bases, spellings, claimed


def emit(bases, spellings, claimed, out_path):
    L = []
    w = L.append
    w("// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception")
    w("// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt")
    w("// GENERATED by scripts/gen_emoji_data.py - do not edit.")
    w("// Sources: Unicode emoji-test.txt 17.0 (order/names/tones); emojibase-data 17.0.0 shortcodes")
    w("// (joypixels, iamcal, github). Pins + licences: scripts/gen_emoji_data.py, assets/LICENSES.md.")
    w('#include "emoji_data.h"')
    w("")

    # Single packed NUL-terminated string table in .rodata so private lookup tables hold uint32_t offsets
    # instead of pointers (zero PIE dynamic relocations; shared read-only pages).
    strtab = bytearray()
    strtab_off = {}

    def intern_bytes(b):
        assert b"\x00" not in b
        off = strtab_off.get(b)
        if off is not None:
            return off
        off = len(strtab)
        strtab_off[b] = off
        strtab.extend(b)
        strtab.append(0)
        return off

    keys = sorted(claimed.items(), key=lambda kv: kv[0].encode("ascii"))
    for k, _ in keys:
        assert len(k) < 200
        intern_bytes(k.encode("ascii"))
    sp = sorted(spellings.items(), key=lambda kv: kv[0])
    for b, _ in sp:
        assert len(b) < 255
        intern_bytes(b)
    toned = [e for e in bases if e["has_tones"]]
    for e in toned:
        for t in e["tones"]:
            intern_bytes(t)
    for e in bases:
        intern_bytes(to_utf8(e["cps"]))
        intern_bytes(e["name_sc"].encode("ascii"))
        intern_bytes(e["name"].encode("utf-8"))
        for al in e["aliases"]:
            intern_bytes(al.encode("ascii"))

    w("const char ccord_emoji_strtab[] = {")
    for i in range(0, len(strtab), 24):
        w("    " + ", ".join(c_strtab_byte(x) for x in strtab[i:i + 24]) + ",")
    w("};")
    w("")

    # alias offset pool (packed, no nullptr sentinels)
    pool = []
    alias_ranges = {}
    for e in bases:
        if e["aliases"]:
            assert len(e["aliases"]) <= 255
            alias_ranges[e["idx"]] = (len(pool), len(e["aliases"]))
            for al in e["aliases"]:
                pool.append(strtab_off[al.encode("ascii")])
    assert len(pool) <= 65535
    w("const uint32_t ccord_emoji_alias_offs[] = {")
    for i in range(0, len(pool), 16):
        w("    " + ", ".join(str(x) for x in pool[i:i + 16]) + ",")
    if not pool:
        w("    0,")
    w("};")
    w("const size_t ccord_emoji_alias_offs_count = %d;" % len(pool))
    w("")
    w("const ccord_emoji_entry ccord_emoji_data[] = {")
    for e in bases:
        al_start, al_count = alias_ranges.get(e["idx"], (0, 0))
        uoff = strtab_off[to_utf8(e["cps"])]
        noff = strtab_off[e["name_sc"].encode("ascii")]
        coff = strtab_off[e["name"].encode("utf-8")]
        w("    {%d, %d, %d, %d, %d, (ccord_emoji_category)%d, %s}," % (
            uoff, noff, coff, al_start, al_count, e["cat"],
            "true" if e["has_tones"] else "false"))
    w("};")
    w("const size_t ccord_emoji_data_count = %d;" % len(bases))
    w("")
    starts = [0] * (len(CATEGORIES) + 1)
    for c in range(len(CATEGORIES) + 1):
        starts[c] = sum(1 for e in bases if e["cat"] < c)
    w("const uint16_t ccord_emoji_cat_start[CCORD_EMOJI_CATEGORY_COUNT + 1] = {%s};" %
      ", ".join(str(s) for s in starts))
    w("")
    w("const ccord_emoji_tone_set ccord_emoji_tone_sets[] = {")
    for e in toned:
        w("    {%d, {%s}}," % (e["idx"], ", ".join(str(strtab_off[t]) for t in e["tones"])))
    w("};")
    w("const size_t ccord_emoji_tone_set_count = %d;" % len(toned))
    w("")
    w("const ccord_emoji_sc_key ccord_emoji_sc_index[] = {")
    for k, idx in keys:
        w("    {%d, %d, %d}," % (strtab_off[k.encode("ascii")], idx, len(k)))
    w("};")
    w("const size_t ccord_emoji_sc_index_count = %d;" % len(keys))
    w("")
    w("const ccord_emoji_utf8_key ccord_emoji_utf8_index[] = {")
    for b, (idx, tone) in sp:
        w("    {%d, %d, %d, %d}," % (strtab_off[b], idx, tone, len(b)))
    w("};")
    w("const size_t ccord_emoji_utf8_index_count = %d;" % len(sp))
    w("")
    data = "\n".join(L)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)
    return len(keys), len(sp)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.environ.get(
        "CCORD_EMOJI_CACHE", os.path.join(tempfile.gettempdir(), "ccord-emoji-cache")))
    ap.add_argument("--out", default=os.path.join(root, "src", "emoji", "emoji_data.c"))
    a = ap.parse_args()
    bases, spellings, claimed = build(a.cache)
    nsc, nsp = emit(bases, spellings, claimed, a.out)
    per = [sum(1 for e in bases if e["cat"] == c) for c in range(len(CATEGORIES))]
    slug = sum(1 for e in bases if e.get("slug"))
    print("emoji: %d  (%s)" % (len(bases), ", ".join(f"{n}={c}" for n, c in zip(CATEGORIES, per))))
    print("with skin tones: %d, shortcode keys: %d, utf8 spellings: %d, slug-only names: %d" % (
        sum(1 for e in bases if e["has_tones"]), nsc, nsp, slug))
    print("wrote", a.out)


if __name__ == "__main__":
    main()
