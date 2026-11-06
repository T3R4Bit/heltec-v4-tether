#!/usr/bin/env python3
"""Regenerate the PWA icons in tools/static/icons (pure Python, no image library).

Design: blocky white "T" on black with three yellow signal bars, inside the maskable safe zone.
"""

import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent / "static" / "icons"
BLACK, WHITE, YELLOW = (17, 17, 17), (242, 242, 242), (255, 212, 0)


def render(size: int):
    px = [[BLACK] * size for _ in range(size)]

    def rect(x0, y0, x1, y1, color):  # fractions of the icon size
        for y in range(int(y0 * size), int(y1 * size)):
            for x in range(int(x0 * size), int(x1 * size)):
                px[y][x] = color

    rect(0.22, 0.24, 0.78, 0.36, WHITE)  # T bar
    rect(0.44, 0.36, 0.56, 0.76, WHITE)  # T stem
    rect(0.62, 0.62, 0.66, 0.76, YELLOW)  # signal bars
    rect(0.68, 0.56, 0.72, 0.76, YELLOW)
    rect(0.74, 0.50, 0.78, 0.76, YELLOW)
    return px


def png(px) -> bytes:
    h, w = len(px), len(px[0])
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for size in (180, 192, 512):
        (OUT / ("icon-%d.png" % size)).write_bytes(png(render(size)))
        print("make_icons: icon-%d.png" % size)


if __name__ == "__main__":
    main()
