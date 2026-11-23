#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
"""cgltf made to read glTF on a big-endian host (the port's CMake runs this on the cgltf it fetched
when the target is big-endian). Idempotent.

    cgltf_little_endian.py <cgltf tree>

A glTF binary's numbers are little-endian: the GLB header's and its chunks' words, and every
component of a buffer. cgltf 1.15 reads them in the host's order, the words with memcpy and the
components through a cast pointer, so on a big-endian host it refuses every .glb at its first word.
Here both read through functions that put a number together from its bytes, which is right on any
host:

  - the seven words of the GLB header and chunk headers (cgltf_parse);
  - the components behind cgltf_accessor_read_float, _read_uint, _read_index and the sparse
    indices (cgltf_component_read_*).

Not changed: the paths that copy a whole float accessor with memcpy
(cgltf_accessor_unpack_floats, cgltf_accessor_unpack_indices). The port does not call them.
"""
import re
import sys
from pathlib import Path

HELPERS = """\
/* OpenRAC: a glTF's numbers are little-endian whatever the host (tools/cgltf_little_endian.py). */
static uint8_t cgltf_le_uint8_t(const void* in) { return *(const uint8_t*)in; }
static int8_t cgltf_le_int8_t(const void* in) { return (int8_t)cgltf_le_uint8_t(in); }
static uint16_t cgltf_le_uint16_t(const void* in)
{
	const uint8_t* b = (const uint8_t*)in;
	return (uint16_t)(b[0] | b[1] << 8);
}
static int16_t cgltf_le_int16_t(const void* in) { return (int16_t)cgltf_le_uint16_t(in); }
static uint32_t cgltf_le_uint32_t(const void* in)
{
	const uint8_t* b = (const uint8_t*)in;
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static float cgltf_le_float(const void* in)
{
	union { uint32_t u; float f; } v;
	v.u = cgltf_le_uint32_t(in);
	return v.f;
}

"""

ANCHOR = "#define GlbHeaderSize 12\n"
WORDS = 7
COMPONENTS = 18

path = Path(sys.argv[1]) / "cgltf.h"
s = path.read_text(encoding="utf-8")
if "cgltf_le_uint32_t" in s:
    sys.exit(0)
if s.count(ANCHOR) != 1:
    raise SystemExit(f"{path}: {s.count(ANCHOR)} matches for {ANCHOR!r}")
s = s.replace(ANCHOR, HELPERS + ANCHOR)
s, words = re.subn(r"memcpy\(&(\w+), ([^,;]+), 4\);", r"\1 = cgltf_le_uint32_t(\2);", s)
s, components = re.subn(
    r"\*\(\(const (u?int(?:8|16|32)_t|float)\*\) in\)", r"cgltf_le_\1(in)", s
)
if (words, components) != (WORDS, COMPONENTS):
    raise SystemExit(
        f"{path}: {words} words and {components} components changed, "
        f"{WORDS} and {COMPONENTS} expected: not the cgltf this was written for"
    )
path.write_text(s, encoding="utf-8", newline="\n")
