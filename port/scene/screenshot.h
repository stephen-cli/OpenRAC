// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// Writing a frame to a PNG, for the viewer's headless checks.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openrac::scene {

// `rgba` is width x height RGBA bytes, top row first.
bool write_png(
    const std::string& path, int width, int height, const std::vector<std::uint8_t>& rgba
);

// As write_png, on a thread of its own: encoding a window's PNG stops a frame for about a second.
// Logs `written` once the file is written (a warning if it cannot be). One shot may wait behind
// the one being written; with one waiting already the call waits for the writer, so that the
// frames held are never more than two. One still waiting at exit is written then.
void write_png_later(
    std::string path, int width, int height, std::vector<std::uint8_t> rgba, std::string written
);

}  // namespace openrac::scene
