#!/usr/bin/env python3
"""Regenerates the stock theme bitmaps in ui/themes/ (original artwork: simple geometric icons).

Every icon is drawn analytically (signed distance + 4x4 supersampling) at 1x and @2x so the
@2x variants are real high-density renders, not upscales. Output is deterministic: running the
script twice produces identical files. The PNG writer cycles the five row filter types and also
emits palette and gray+alpha files so the image decoders see every common PNG layout.

Usage:  python ui/tools/gen_theme_assets.py      (writes into ui/themes/<theme>/...)
"""
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "themes")


def png_chunk(kind, data):
    c = struct.pack(">I", len(data)) + kind + data
    return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def filter_rows(rows, bpp):
    """Applies filter type (y % 5) to each row (deterministic, exercises all five filters)."""
    out = bytearray()
    prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        ft = y % 5
        line = bytearray()
        for i, x in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 0:
                v = x
            elif ft == 1:
                v = x - a
            elif ft == 2:
                v = x - b
            elif ft == 3:
                v = x - ((a + b) >> 1)
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = x - pred
            line.append(v & 0xFF)
        out.append(ft)
        out += line
        prev = row
    return bytes(out)


def write_png(path, w, h, color_type, rows, bpp, palette=None, trns=None):
    raw = filter_rows(rows, bpp)
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
    if palette is not None:
        data += png_chunk(b"PLTE", b"".join(bytes(c[:3]) for c in palette))
        if trns is not None:
            data += png_chunk(b"tRNS", bytes(trns))
    data += png_chunk(b"IDAT", zlib.compress(raw, 9))
    data += png_chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def render(size, shape, color):
    """RGBA rows for `shape(x, y) -> coverage-defining signed distance` (unit square coords)."""
    rows = []
    ss = 4
    for y in range(size):
        row = bytearray()
        for x in range(size):
            cov = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    if shape(u, v) <= 0:
                        cov += 1
            a = round(255 * cov / (ss * ss))
            row += bytes((color[0], color[1], color[2], a))
        rows.append(bytes(row))
    return rows


def circle(cx, cy, r):
    return lambda u, v: math.hypot(u - cx, v - cy) - r


def ring(cx, cy, r, w):
    return lambda u, v: abs(math.hypot(u - cx, v - cy) - r) - w / 2


def rect(x0, y0, x1, y1):
    return lambda u, v: max(x0 - u, u - x1, y0 - v, v - y1)


def triangle_right(x0, y0, x1, y1):
    # isoceles triangle pointing right inside the box
    def f(u, v):
        if u < x0 or u > x1:
            return 1
        t = (u - x0) / (x1 - x0)
        half = (1 - t) * (y1 - y0) / 2
        mid = (y0 + y1) / 2
        return abs(v - mid) - half
    return f


def union(*fs):
    return lambda u, v: min(f(u, v) for f in fs)


def knob_cap(u, v):
    body = math.hypot(u - 0.5, v - 0.5) - 0.40
    # pointer notch at 12 o'clock
    notch = max(abs(u - 0.5) - 0.035, 0.12 - v, v - 0.40)
    return max(body, -notch)


ICONS = {
    "icons/play.png": triangle_right(0.28, 0.2, 0.8, 0.8),
    "icons/stop.png": rect(0.25, 0.25, 0.75, 0.75),
    "icons/record.png": circle(0.5, 0.5, 0.3),
    "icons/loop.png": ring(0.5, 0.5, 0.3, 0.1),
    "icons/pause.png": union(rect(0.27, 0.22, 0.43, 0.78), rect(0.57, 0.22, 0.73, 0.78)),
}

THEME_COLORS = {
    "dark": {"icon": (0xE4, 0xE6, 0xEB), "record": (0xE0, 0x60, 0x4A), "knob": (0xD8, 0xDA, 0xDF)},
    "light": {"icon": (0x2A, 0x2C, 0x33), "record": (0xC8, 0x3A, 0x28), "knob": (0x55, 0x59, 0x63)},
}


def main():
    for theme, cols in THEME_COLORS.items():
        for name, shape in ICONS.items():
            col = cols["record"] if "record" in name else cols["icon"]
            for scale, suffix in ((1, ""), (2, "@2x")):
                size = 16 * scale
                base, ext = os.path.splitext(name)
                write_png(os.path.join(ROOT, theme, base + suffix + ext), size, size, 6, render(size, shape, col), 4)
        for scale, suffix in ((1, ""), (2, "@2x"), (3, "@3x")):
            size = 32 * scale
            write_png(os.path.join(ROOT, theme, "knob/cap" + suffix + ".png"), size, size, 6, render(size, knob_cap, cols["knob"]), 4)

    # base theme: shared bitmaps every theme inherits (palette + gray/alpha layouts on purpose)
    # meter gradient strip, palette PNG with transparency (1x and @2x)
    for scale, suffix in ((1, ""), (2, "@2x")):
        w, h = 4 * scale, 64 * scale
        palette = [(0x3C, 0xC0, 0x6A), (0xE0, 0xC0, 0x3A), (0xE0, 0x60, 0x4A), (0, 0, 0)]
        rows = []
        for y in range(h):
            t = y / h
            idx = 2 if t < 0.12 else (1 if t < 0.3 else 0)
            rows.append(bytes([3 if (y % (4 * scale)) == 0 else idx] * w))
        write_png(os.path.join(ROOT, "base", "meter/segments" + suffix + ".png"), w, h, 3, rows, 1, palette, [255, 255, 255, 0])
    # soft shadow, gray + alpha
    for scale, suffix in ((1, ""), (2, "@2x")):
        size = 24 * scale
        rows = []
        for y in range(size):
            row = bytearray()
            for x in range(size):
                d = math.hypot((x + 0.5) / size - 0.5, (y + 0.5) / size - 0.5)
                a = max(0.0, min(1.0, (0.5 - d) / 0.25))
                row += bytes((0, round(160 * a * a)))
            rows.append(bytes(row))
        write_png(os.path.join(ROOT, "base", "shadow/round" + suffix + ".png"), size, size, 4, rows, 2)


if __name__ == "__main__":
    main()
