# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
#
# Writes OUTPUT, the RSX program INPUT without the parts named in DROP (names joined by "+").
# A line of a part ends in "#@" and the part's name; the marks are taken off the lines kept.
# Run by cmake/Window.cmake at build time, once for every set of a fragment program's parts:
#   cmake -DINPUT=mesh.fp -DOUTPUT=mesh_09.fp -DDROP=+cutout+leftout+added -P variant.cmake
file(READ "${INPUT}" text)
string(REPLACE "+" ";" parts "${DROP}")
foreach(part IN LISTS parts)
  if(NOT part STREQUAL "")
    string(REGEX REPLACE "[^\n]*#@${part}\n" "" text "${text}")
  endif()
endforeach()
string(REGEX REPLACE " *#@[a-z]+\n" "\n" text "${text}")
file(WRITE "${OUTPUT}" "${text}")
