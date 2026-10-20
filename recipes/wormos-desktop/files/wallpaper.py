#!/usr/bin/env python3
"""Draw the WormOS wallpaper (a wormhole) as a PNG: wallpaper.py OUT [W H]."""

import math
import struct
import sys
import zlib


def png(path, width, height, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    raw = b"".join(b"\0" + row for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def main():
    out = sys.argv[1]
    width, height = (int(v) for v in sys.argv[2:4]) if len(sys.argv) > 3 else (1920, 1080)
    edge = (0x0d, 0x0b, 0x14)      # background at the corners
    glow = (0x2a, 0x17, 0x47)      # purple around the throat
    ring = (0xa8, 0x55, 0xf7)      # accent: the rings
    cx, cy = width * 0.62, height * 0.5
    scale = min(width, height)
    rows = []
    for y in range(height):
        row = bytearray()
        dy = (y - cy) / scale
        for x in range(width):
            dx = (x - cx) / scale
            # Squash horizontally a little: a tunnel seen from the side.
            r = math.hypot(dx * 0.8, dy)
            base = mix(glow, edge, min(1.0, r / 0.75) ** 0.7)
            # Rings get closer together towards the centre, like a funnel.
            phase = math.log(max(r, 0.02)) * 9.0
            band = (0.5 + 0.5 * math.cos(phase)) ** 24
            fade = max(0.0, 1.0 - r / 0.7) * min(1.0, r / 0.05)
            color = mix(base, ring, band * fade * 0.55)
            dark = max(0.0, 1.0 - r / 0.035)
            color = mix(color, (0, 0, 0), dark)
            row += bytes(int(c) for c in color)
        rows.append(bytes(row))
    png(out, width, height, rows)


if __name__ == "__main__":
    main()
