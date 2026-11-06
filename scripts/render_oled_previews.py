#!/usr/bin/env python3
"""Render the TETHER OLED layouts to PNG with the firmware's own drawing code (SYNTHETIC link scenarios).

Compiles tests/host/oled_preview.cpp against module/Tether, runs it, and writes:
  docs/oled/<layout>-<scenario>.png   one frame each, 4x scale, OLED look
  docs/oled/sketches.png              contact sheet: layouts across, scenarios down

Usage: python scripts/render_oled_previews.py
"""

import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from gen_tether_font import GLYPHS  # noqa: E402
from run_host_tests import find_compiler  # noqa: E402

OUT = REPO / "docs" / "oled"
BG, OFF, ON, INK = (10, 10, 12), (22, 24, 30), (225, 240, 255), (240, 240, 240)
LAYOUTS = [("1", "strip", "1 STRIP"), ("2", "split", "2 SPLIT"), ("3", "big", "3 BIG")]
SCENARIOS = [("good", "GOOD LINK"), ("fading", "FADING"), ("lost", "LOST"), ("nopeer", "NO PEER"), ("waiting", "WAITING")]


def read_pbm(path: Path):
    tokens = path.read_text().split()
    assert tokens[0] == "P1"
    w, h = int(tokens[1]), int(tokens[2])
    bits = "".join(tokens[3:])
    return [[bits[y * w + x] == "1" for x in range(w)] for y in range(h)]


def png_bytes(px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def blank(w, h, color=BG):
    return [[color] * w for _ in range(h)]


def blit_frame(img, frame, ox, oy, scale):
    """Draw an OLED frame with visible pixel grid: each pixel a (scale-1) square."""
    for y, row in enumerate(frame):
        for x, on in enumerate(row):
            color = ON if on else OFF
            for dy in range(scale - 1):
                line = img[oy + y * scale + dy]
                for dx in range(scale - 1):
                    line[ox + x * scale + dx] = color


def draw_text(img, x, y, text, scale=2, color=INK):
    for ch in text.upper():
        rows = GLYPHS.get(ch, GLYPHS["?"])
        for ry, row in enumerate(rows):
            for rx, c in enumerate(row):
                if c == "#":
                    for dy in range(scale):
                        for dx in range(scale):
                            img[y + ry * scale + dy][x + rx * scale + dx] = color
        x += (len(rows[0]) + 1) * scale


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "oled_preview.exe"
        cmd = find_compiler() + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-I", str(REPO / "module" / "Tether"),
                                 str(REPO / "tests" / "host" / "oled_preview.cpp"), "-o", str(exe)]
        if subprocess.run(cmd).returncode != 0:
            sys.exit("render_oled_previews: compile failed")
        if subprocess.run([str(exe), tmp]).returncode != 0:
            sys.exit("render_oled_previews: renderer failed")
        frames = {(lid, sc): read_pbm(Path(tmp) / ("%s-%s-%s.pbm" % (lid, lname, sc)))
                  for lid, lname, _ in LAYOUTS for sc, _ in SCENARIOS}

    for (lid, sc), frame in frames.items():
        lname = [n for i, n, _ in LAYOUTS if i == lid][0]
        img = blank(128 * 4 + 16, 64 * 4 + 16)
        blit_frame(img, frame, 8, 8, 4)
        (OUT / ("%s-%s.png" % (lname, sc))).write_bytes(png_bytes(img))

    scale, pad, label_w, head_h = 3, 24, 150, 40
    cell_w, cell_h = 128 * scale, 64 * scale
    w = label_w + len(LAYOUTS) * (cell_w + pad) + pad
    h = head_h + len(SCENARIOS) * (cell_h + pad) + pad
    sheet = blank(w, h)
    for i, (_, _, title) in enumerate(LAYOUTS):
        draw_text(sheet, label_w + pad + i * (cell_w + pad), 14, title, 2)
    for j, (sc, title) in enumerate(SCENARIOS):
        y = head_h + pad + j * (cell_h + pad)
        draw_text(sheet, 12, y + cell_h // 2 - 7, title, 2)
        for i, (lid, _, _) in enumerate(LAYOUTS):
            blit_frame(sheet, frames[(lid, sc)], label_w + pad + i * (cell_w + pad), y, scale)
    (OUT / "sketches.png").write_bytes(png_bytes(sheet))
    print("render_oled_previews: wrote %d frames and sketches.png to %s" % (len(frames), OUT))


if __name__ == "__main__":
    main()
