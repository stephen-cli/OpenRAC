!!ARBfp1.0
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# A copy between framebuffers (rsxgl_draw.cpp, draw_copy): the texel as it is, alpha with it.
# What direct.fp writes with a copy's numbers, in three instructions: the RSX fills a pixel
# the faster the shorter its program (README.md, "Fragment programs by their parts").
MOV R1, fragment.texcoord[0];
TEX R2, R1, texture[0], 2D;
MOV result.color, R2;
END
