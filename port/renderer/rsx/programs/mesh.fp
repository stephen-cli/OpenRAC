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
#   texcoord[7]  an added moby's colour is weighed by its alpha here (the GS's Cs As, As to
#                about 2): the colour times min(a, z) x + y
#
# The RSX fills a pixel the faster the shorter its program, so a part a draw can do without
# is left out of its program: a line ending in "#@" and a name is of that part, and the
# program is assembled once for every set of parts (programs/variant.cmake; rsxgl_draw.cpp,
# mesh_parts, chooses a draw's). Without a part the pixel is what the part's numbers of no
# effect make it:
#   material  texcoord[3] is not white
#   cutout    texcoord[5] leaves pixels out
#   leftout   texcoord[2].w can be below 0 (moby.vp's draws)
#   fog       texcoord[4].w is not 0
#   added     texcoord[7] weighs the colour by its alpha
#
# One interpolated input an instruction, each moved to a register first (direct.fp).
MOV R1, fragment.texcoord[0];
TEX R2, R1, texture[0], 2D;
MOV R3, fragment.texcoord[1];
MUL R2, R2, R3;
MOV R4, fragment.texcoord[3]; #@material
MUL R2, R2, R4; #@material
MOV R5, fragment.texcoord[5]; #@cutout
MUL R6.x, R2.w, R5.x; #@cutout
ADDC R6.x, R6.x, R5.y; #@cutout
KIL (LT.x); #@cutout
MUL R6.x, R2.w, R5.z; #@cutout
ADDC R6.x, R6.x, R5.w; #@cutout
KIL (LT.x); #@cutout
MOV R7, fragment.texcoord[2];
ADDC R6.x, R7.w, R7.w; #@leftout
KIL (LT.x); #@leftout
MUL_SAT R2.xyz, R2, R7.z;
RCP R7.w, R7.y; #@fog
MUL R7.x, R7.x, R7.w; #@fog
MOV R8, fragment.texcoord[4]; #@fog
MOV R9, fragment.texcoord[6];
MAD R7.x, R7.x, R8.w, R9.x; #@fog
ADD R10.xyz, R2, -R8; #@fog
MAD R2.xyz, R10, R7.x, R8; #@fog
MOV R11, fragment.texcoord[7]; #@added
MIN R12.x, R2.w, R11.z; #@added
MAD R12.x, R12.x, R11.x, R11.y; #@added
MUL_SAT R2.xyz, R2, R12.x; #@added
MAD_SAT R2.w, R2.w, R9.y, R9.z;
MOV result.color, R2;
END
