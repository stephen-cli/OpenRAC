#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
"""PSL1GHT's librsx made right for GCC 13 (toolchain.sh runs this on PSL1GHT's source, then builds
and installs librsx again). Idempotent.

    psl1ght-lv2.py <PSL1GHT tree>

A librsx function that finds the command buffer's segment full calls libgcm's callback, which
writes a jump at the segment's end and moves the context's pointers to the next segment. librsx
calls it from inline assembly that named three registers as changed and nothing else. GCC 13 took
that at its word: the caller kept the context's write pointer in a register across the call and
wrote its commands at the old place, over the jump and on past the segment's end, while the RSX
followed the jump into the middle of them. The picture stopped at the first full segment (32 KB of
commands) with RPCS3's "FIFO error: possible desync event".

The assembly now says what a call does: memory is written, the registers a call may change are
changed, the two arguments are in r3 and r4 by declaration (they were there by luck). Its frame is
also larger than the red zone, in which the compiler keeps the function's own saved registers.
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def replace_once(path, old, new):
    s = path.read_text(encoding="utf-8")
    if new in s:
        return
    if s.count(old) != 1:
        raise SystemExit(f"{path}: {s.count(old)} matches for {old!r}")
    path.write_text(s.replace(old, new), encoding="utf-8", newline="\n")


replace_once(
    tree / "ppu" / "include" / "rsx" / "rsx_function_macros.h",
    "\t\tregister s32 result asm(\"r3\");\n"
    "\t\t__asm__ __volatile__ (\n"
    "            \"stdu\t1,-128(1)\\n\"\n"
    "            \"mr\t\t31,2\\n\"\n"
    "            \"lwz\t0,0(%3)\\n\"\n"
    "            \"lwz\t2,4(%3)\\n\"\n"
    "            \"mtctr\t0\\n\"\n"
    "            \"bctrl\\n\"\n"
    "            \"mr\t\t2,31\\n\"\n"
    "            \"addi\t1,1,128\\n\"\n"
    "            : \"+r\"(result)\n"
    "            : \"r\"(context), \"r\"(count), \"b\"(context->callback)\n"
    "            : \"r31\", \"r0\", \"lr\"\n"
    "\t\t);\n"
    "\t\treturn result;\n",
    "\t\tregister u64 first asm(\"r3\") = (u64)context;\n"
    "\t\tregister u64 second asm(\"r4\") = count;\n"
    "\t\t__asm__ __volatile__ (\n"
    "            \"stdu\t1,-448(1)\\n\"\n"
    "            \"mr\t\t31,2\\n\"\n"
    "            \"lwz\t0,0(%2)\\n\"\n"
    "            \"lwz\t2,4(%2)\\n\"\n"
    "            \"mtctr\t0\\n\"\n"
    "            \"bctrl\\n\"\n"
    "            \"mr\t\t2,31\\n\"\n"
    "            \"addi\t1,1,448\\n\"\n"
    "            : \"+r\"(first), \"+r\"(second)\n"
    "            : \"b\"(context->callback)\n"
    "            : \"r0\", \"r5\", \"r6\", \"r7\", \"r8\", \"r9\", \"r10\", \"r11\", \"r12\", \"r31\",\n"
    "              \"lr\", \"ctr\", \"xer\", \"cr0\", \"cr1\", \"cr5\", \"cr6\", \"cr7\", \"memory\"\n"
    "\t\t);\n"
    "\t\treturn (s32)first;\n",
)
print("PSL1GHT's librsx prepared for GCC 13")
