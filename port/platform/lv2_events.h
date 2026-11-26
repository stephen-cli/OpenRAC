// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// What the PlayStation 3's system has for the program between frames, which SDL (built there
// without a platform of its own, cmake/Dependencies.cmake) does not read: the pads and the XMB's
// request to quit.
//
// A pad on one of the console's ports is an SDL virtual gamepad for as long as it is connected,
// so the rest of the port reads it as any other gamepad (input.h): the bindings, the players'
// ports, the menus and rumble are those of every platform. The request to quit ("Quit Game" in
// the XMB, the power button) becomes SDL's quit event.
#pragma once

namespace openrac::platform {

// Reads the pads and the system's requests into SDL. Called where SDL's own events are about to be
// read (pump.cpp), on the thread that reads them.
void read_lv2_events();

}  // namespace openrac::platform
