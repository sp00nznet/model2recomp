#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the text a game has put on screen out of its tilemap dump.

A recompiled game that draws nothing is not necessarily silent: most of these
boot through a self-test that prints what it is doing, and the tilemap name
table holds those characters as tile indices long before anything about the
colour path is working. Virtua Striker sits on "SOUND initialize..." - which is
a sound-board stub, not a renderer fault, and nothing else in the sweep said so.

    MODEL2_RAMDUMP=out ./corpus_<set> <roms> <title> <variant>
    python tools/screen_text.py out.tile

The System 24 tilemap is 64 columns of 16-bit entries per row; the low 12 bits
are the tile index, which in every Model 2 self-test font so far is ASCII.
"""
import struct
import sys


def screen_text(path, min_run=3):
    with open(path, "rb") as f:
        raw = f.read()
    words = []
    for dw in struct.unpack("<%dI" % (len(raw) // 4), raw):
        words.append(dw & 0xFFFF)
        words.append((dw >> 16) & 0xFFFF)

    lines = []
    for page_start in range(0, min(len(words), 0x4000), 0x1000):
        for row in range(64):
            base = page_start + row * 64
            chars = []
            for col in range(64):
                if base + col >= len(words):
                    break
                tile = words[base + col] & 0xFFF
                chars.append(chr(tile) if 32 <= tile < 127 else " ")
            line = "".join(chars).rstrip()
            # A row of nothing but spaces is a cleared row, not a message.
            if len(line.strip()) >= min_run:
                lines.append((page_start // 0x1000, row, line.strip()))
    return lines


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        found = screen_text(path)
        print("== %s ==" % path)
        for page, row, line in found:
            print("  p%d r%-2d %s" % (page, row, line))
        if not found:
            print("  (nothing legible on screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
