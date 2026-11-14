!!ARBfp1.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# renderer/shaders/direct.frag for the RSX (README.md): the chip's texture function, fog, alpha
# test and the blend's source weight. The GLSL branches on its uniforms; here every branch is one
# sum whose weights the layer works out from the same uniforms (rsxgl_draw.cpp, direct_constants)
# and direct.vp hands over as texture coordinates. Colours are 0 to 1 for the chip's 0 to 255.
#
#   color        the vertex colour c
#   texcoord[0]  s, t, q, fog
#   texcoord[1]  rgb = x c.rgb t.rgb + y t.rgb + z c.a + w c.rgb
#   texcoord[2]  a = x c.a t.a + y t.a + z c.a;  w: the fog's weight, f = w fog + texcoord[3].w
#   texcoord[3]  the fog colour, and the rest of f
#   texcoord[4]  the alpha test: d = round(255 a) - w, killed when x d + y |d| + z < 0
#   texcoord[5]  the source weight: rgb = rgb (x + y a) + (z a + w)
#   texcoord[6]  255, 0.5, what a is multiplied by on its way out, 0
#
# The chip reads one interpolated input an instruction (the assembler takes more and keeps the
# last), so each is moved to a register first: R7 the colour, R8 to R13 texcoord[1] to [6].
# R0 is the colour written, and only the last instruction writes it. The assembler leaves out
# what the chip has no instruction for and says nothing (SUB: an ADD of the negated operand).
RCP R1.w, fragment.texcoord[0].z;
MUL R1.xy, fragment.texcoord[0], R1.w;
MOV R1.z, fragment.texcoord[0].w;
TEX R2, R1, texture[0], 2D;
MOV R7, fragment.color;
MUL R3, R7, R2;
MOV R8, fragment.texcoord[1];
MUL R4.xyz, R3, R8.x;
MAD R4.xyz, R2, R8.y, R4;
MAD R4.xyz, R7.w, R8.z, R4;
MAD_SAT R4.xyz, R7, R8.w, R4;
MOV R9, fragment.texcoord[2];
MUL R4.w, R3.w, R9.x;
MAD R4.w, R2.w, R9.y, R4.w;
MAD_SAT R4.w, R7.w, R9.z, R4.w;
MOV R10, fragment.texcoord[3];
MAD R5.x, R1.z, R9.w, R10.w;
ADD R6.xyz, R4, -R10;
MAD R4.xyz, R6, R5.x, R10;
MOV R13, fragment.texcoord[6];
MAD R5.y, R4.w, R13.x, R13.y;
FLR R5.y, R5.y;
MOV R11, fragment.texcoord[4];
ADD R5.y, R5.y, -R11.w;
MAX R5.w, R5.y, -R5.y;
MUL R5.z, R5.y, R11.x;
MAD R5.z, R5.w, R11.y, R5.z;
ADDC R5.z, R5.z, R11.z;
KIL (LT.z);
MOV R12, fragment.texcoord[5];
MAD R6.x, R4.w, R12.y, R12.x;
MAD R6.y, R4.w, R12.z, R12.w;
MAD_SAT R4.xyz, R4, R6.x, R6.y;
MUL R4.w, R4.w, R13.z;
MOV result.color, R4;
END
