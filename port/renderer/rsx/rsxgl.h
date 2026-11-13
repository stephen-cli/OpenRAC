// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The renderer's GL (renderer/gl.h) carried out with the PlayStation 3's RSX, through PSL1GHT's
// librsx: what renderer/vulkan/vkgl.h is to Vulkan. The window opens it for GlApi::Rsx
// (platform/window.h) and hands its get_proc to gl::load, so the renderer calls it as any GL.
// README.md says how much of GL is carried out yet; the rest is answered and does nothing.
//
// One thread calls it, the one that draws.

#pragma once

#include <string>

namespace openrac::rsxgl {

struct Config {
    bool vsync = true;  // the flip waits for the vertical blank
};

// Starts the RSX: the console's video mode as it is set, the command buffer, two colour buffers
// and a depth buffer in the RSX's memory. False, with `error`, when the display cannot be had.
bool open(const Config& config, std::string& error);

// Lets the RSX finish. The process ends with its memory; nothing is given back before.
void close();

// True between open and close.
bool active();

// The picture's size in pixels: the video mode's.
int width();
int height();

// What the session's log calls the graphics: the chip and the video mode.
std::string device_name();

// The GL function of that name, in SDL_GL_GetProcAddress's shape; null for one the layer does not
// answer (the compute and program-binary functions, which gl::load takes as absent).
using Proc = void (*)();
Proc get_proc(const char* name);

// Shows the picture drawn since the last call and makes the other buffer the one drawn into.
void present();

}  // namespace openrac::rsxgl
