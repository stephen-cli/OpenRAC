!!ARBfp1.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# viewer/shaders/sky.frag for the RSX (README.md): a shell's texel times its vertex colour, the
# colour times the tint; the alpha blends it.
#
#   texcoord[0]  u, v
#   texcoord[1]  sky_tint.rgb
#   texcoord[2]  the vertex colour
#
# One interpolated input an instruction, each moved to a register first (direct.fp).
MOV R1, fragment.texcoord[0];
TEX R2, R1, texture[0], 2D;
MOV R3, fragment.texcoord[2];
MUL R2, R2, R3;
MOV R4, fragment.texcoord[1];
MUL R2.xyz, R2, R4;
MOV result.color, R2;
END
