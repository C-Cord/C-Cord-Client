#!/usr/bin/env python3
"""Regenerates src/platform/app_icon_data.c from assets/icons/*.png as lossless QOI (stdlib only, deterministic)."""
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_SIZES = [16, 32, 48, 64, 128, 256]
TRAY_SIZES = [22, 24, 32, 48]


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, hdr = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or ctype != 6 or interlace != 0:
        raise SystemExit(f"{path}: need 8-bit non-interlaced RGBA")
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    return w, h, bytes(out)


def qoi_encode_rgba(w, h, rgba):
    """Encodes straight-alpha RGBA bytes into standard QOI v1.0 format."""
    out = bytearray(struct.pack(">4sIIBB", b"qoif", w, h, 4, 0))
    index = [(0, 0, 0, 0)] * 64
    pr, pg, pb, pa = 0, 0, 0, 255
    run = 0
    n = w * h
    for i in range(n):
        off = i * 4
        r, g, b, a = rgba[off], rgba[off + 1], rgba[off + 2], rgba[off + 3]
        if r == pr and g == pg and b == pb and a == pa:
            run += 1
            if run == 62 or i == n - 1:
                out.append(0xC0 | (run - 1))
                run = 0
        else:
            if run > 0:
                out.append(0xC0 | (run - 1))
                run = 0
            idx = (r * 3 + g * 5 + b * 7 + a * 11) & 63
            if index[idx] == (r, g, b, a):
                out.append(idx)
            else:
                index[idx] = (r, g, b, a)
                if a == pa:
                    vr = ((r - pr + 128) & 255) - 128
                    vg = ((g - pg + 128) & 255) - 128
                    vb = ((b - pb + 128) & 255) - 128
                    vg_r = ((vr - vg + 128) & 255) - 128
                    vg_b = ((vb - vg + 128) & 255) - 128
                    if -2 <= vr <= 1 and -2 <= vg <= 1 and -2 <= vb <= 1:
                        out.append(0x40 | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2))
                    elif -32 <= vg <= 31 and -8 <= vg_r <= 7 and -8 <= vg_b <= 7:
                        out.append(0x80 | (vg + 32))
                        out.append(((vg_r + 8) << 4) | (vg_b + 8))
                    else:
                        out.extend((0xFE, r, g, b))
                else:
                    out.extend((0xFF, r, g, b, a))
            pr, pg, pb, pa = r, g, b, a
    out.extend(b"\x00\x00\x00\x00\x00\x00\x00\x01")
    return bytes(out)


def qoi_decode_rgba(data):
    """Reference QOI v1.0 decoder used to verify lossless round-trip on generation."""
    magic, w, h, ch, _ = struct.unpack(">4sIIBB", data[:14])
    assert magic == b"qoif" and ch == 4 and data[-8:] == b"\x00\x00\x00\x00\x00\x00\x00\x01"
    index = [(0, 0, 0, 0)] * 64
    r, g, b, a = 0, 0, 0, 255
    p, end = 14, len(data) - 8
    run = 0
    out = bytearray(w * h * 4)
    for dst in range(0, len(out), 4):
        if run > 0:
            run -= 1
        else:
            b1 = data[p]
            p += 1
            update = True
            if b1 == 0xFE:
                r, g, b = data[p], data[p + 1], data[p + 2]
                p += 3
            elif b1 == 0xFF:
                r, g, b, a = data[p], data[p + 1], data[p + 2], data[p + 3]
                p += 4
            else:
                tag = b1 & 0xC0
                if tag == 0x00:
                    r, g, b, a = index[b1]
                    update = False
                elif tag == 0x40:
                    r = (r + ((b1 >> 4) & 3) - 2) & 255
                    g = (g + ((b1 >> 2) & 3) - 2) & 255
                    b = (b + (b1 & 3) - 2) & 255
                elif tag == 0x80:
                    b2 = data[p]
                    p += 1
                    vg = (b1 & 0x3F) - 32
                    r = (r + vg - 8 + ((b2 >> 4) & 0x0F)) & 255
                    g = (g + vg) & 255
                    b = (b + vg - 8 + (b2 & 0x0F)) & 255
                else:
                    run = b1 & 0x3F
                    update = False
            if update:
                index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r, g, b, a)
        out[dst] = r
        out[dst + 1] = g
        out[dst + 2] = b
        out[dst + 3] = a
    assert run == 0 and p == end
    return bytes(out)


def emit(name, blob, lines):
    lines.append(f"static const uint8_t {name}[{len(blob)}] = {{")
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(str(b) for b in blob[i:i + 24]) + ",")
    lines.append("};")


def main():
    lines = [
        "// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception",
        "// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt",
        "// Generated by scripts/embed_icons.py from assets/icons/*.png (lossless QOI). Do not edit.",
        '#include "app_icon_data.h"',
        "",
    ]
    for s in APP_SIZES:
        w, h, pix = read_png(ROOT / f"assets/icons/ccord-{s}.png")
        assert w == s and h == s
        qoi = qoi_encode_rgba(w, h, pix)
        assert qoi_decode_rgba(qoi) == pix, f"QOI round-trip mismatch for ccord-{s}.png"
        emit(f"k_app_{s}", qoi, lines)
    for s in TRAY_SIZES:
        w, h, pix = read_png(ROOT / f"assets/icons/ccord-tray-{s}.png")
        assert w == s and h == s
        qoi = qoi_encode_rgba(w, h, pix)
        assert qoi_decode_rgba(qoi) == pix, f"QOI round-trip mismatch for ccord-tray-{s}.png"
        emit(f"k_tray_{s}", qoi, lines)
    lines.append("")
    lines.append("const ccord_icon_image ccord_app_icon_images[] = {")
    for s in APP_SIZES:
        lines.append(f"  {{{s}, k_app_{s}, sizeof k_app_{s}}},")
    lines.append("};")
    lines.append(f"const size_t ccord_app_icon_image_count = {len(APP_SIZES)};")
    lines.append("const ccord_icon_image ccord_tray_icon_images[] = {")
    for s in TRAY_SIZES:
        lines.append(f"  {{{s}, k_tray_{s}, sizeof k_tray_{s}}},")
    lines.append("};")
    lines.append(f"const size_t ccord_tray_icon_image_count = {len(TRAY_SIZES)};")
    out = ROOT / "src/platform/app_icon_data.c"
    out.write_text("\n".join(lines) + "\n")
    print(f"wrote {out.relative_to(ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
