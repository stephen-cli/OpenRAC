!!ARBfp1.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# viewer/shaders/mesh.frag for the RSX (README.md), the game's own picture of it: the texel times
# the vertex colour and the material's, the cutout, the viewer's sun, the chip's fog. The GLSL
# branches on its uniforms; here each branch is a sum whose weights the layer works out
# (rsxgl_draw.cpp, mesh_constants) and mesh.vp hands over as texture coordinates.
#
#   texcoord[0]  u, v
#   texcoord[1]  the vertex colour times the instance's tint
#   texcoord[2]  fog w, w, the light; w: below 0 where the pixel is left out (moby.vp: a glow
#                packet's in the solid pass; all three of a packet's triangle's vertices agree)
#   texcoord[3]  material_colour
#   texcoord[4]  the fog colour; w: the fog's weight, f = w fog + texcoord[6].x
#   texcoord[5]  the cutout: the pixel is left out when x a + y < 0 or z a + w < 0
#   texcoord[6]  the rest of f; alpha written = y a + z
#
# One interpolated input an instruction, each moved to a register first (direct.fp).
MOV R1, fragment.texcoord[0];
TEX R2, R1, texture[0], 2D;
MOV R3, fragment.texcoord[1];
MUL R2, R2, R3;
MOV R4, fragment.texcoord[3];
MUL R2, R2, R4;
MOV R5, fragment.texcoord[5];
MUL R6.x, R2.w, R5.x;
ADDC R6.x, R6.x, R5.y;
KIL (LT.x);
MUL R6.x, R2.w, R5.z;
ADDC R6.x, R6.x, R5.w;
KIL (LT.x);
MOV R7, fragment.texcoord[2];
ADDC R6.x, R7.w, R7.w;
KIL (LT.x);
MUL_SAT R2.xyz, R2, R7.z;
RCP R7.w, R7.y;
MUL R7.x, R7.x, R7.w;
MOV R8, fragment.texcoord[4];
MOV R9, fragment.texcoord[6];
MAD R7.x, R7.x, R8.w, R9.x;
ADD R10.xyz, R2, -R8;
MAD R2.xyz, R10, R7.x, R8;
MAD_SAT R2.w, R2.w, R9.y, R9.z;
MOV result.color, R2;
END
