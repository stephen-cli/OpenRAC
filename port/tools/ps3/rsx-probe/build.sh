#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# Builds the RSX probe in the toolchain's container (README.md): the two programs assembled by
# PSL1GHT's cgcomp (-a: from assembly, which needs no Cg toolkit) and embedded as arrays, then the
# ELF, in OUT (default port/build/rsx-probe).
#   podman run --rm -v "$PWD:/src" openrac-ps3 sh /src/port/tools/ps3/rsx-probe/build.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../../build/rsx-probe}"
mkdir -p "$OUT"
cd "$OUT"
cgcomp -v -a "$HERE/triangle.vp" triangle.vpo
cgcomp -f -a "$HERE/triangle.fp" triangle.fpo
for p in vpo fpo; do
    {
        echo "alignas(16) static const unsigned char triangle_$p[] = {"
        od -An -v -tx1 "triangle.$p" | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'
        echo "};"
    } > "triangle_$p.h"
done
ppu-g++ -std=c++20 -O2 -mcpu=cell -I. -I"$PS3DEV/ppu/include" "$HERE/main.cpp" -o rsx-probe.elf \
    -L"$PS3DEV/ppu/lib" -lrsx -lgcm_sys -lsysutil -lio -lrt -llv2 -lm
"$PS3DEV/bin/sprxlinker" rsx-probe.elf
ls -l rsx-probe.elf
