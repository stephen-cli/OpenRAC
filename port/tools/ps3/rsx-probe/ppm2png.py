#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
"""A binary PPM (P6, 8 bits) written as a PNG, with nothing but the standard library."""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
assert parts[0] == b"P6" and parts[2] == b"255"
width, height = map(int, parts[1].split())
pixels = parts[3]
rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
print(width, height, len(png))
