// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// rsxgl.h: the display, GL's questions about the context, and the table of GL functions
// (README.md says which are carried out; rsxgl_objects.cpp and rsxgl_draw.cpp carry them out).

#include "renderer/rsx/rsxgl.h"

#include <cstddef>
#include <cstring>
#include <format>
#include <malloc.h>
#include <string_view>
#include <unistd.h>

#include "common/log.h"
#include "renderer/rsx/rsxgl_internal.h"

#include <sys/systime.h>
#include <sysutil/video.h>

namespace openrac::rsxgl {

using namespace openrac::gl;

State g;

namespace {

// A wait's sleep. Five seconds of them are said once: a queue the RSX has stopped reading is
// otherwise a program that hangs in silence.
void wait_a_little(const char* what, u32& waited) {
    constexpr u32 kSleep = 100;  // microseconds
    usleep(kSleep);
    if (++waited == 5'000'000 / kSleep) {
        const gcmControlRegister* queue = gcmGetControlRegister();
        log::error(
            "rsxgl: {} for five seconds (the RSX reads its commands at {:#x}, they are written to "
            "{:#x})",
            what,
            static_cast<u32>(queue->get),
            static_cast<u32>(queue->put)
        );
    }
}

}  // namespace

// The label is written at the end of the command buffer: when it reads back, the RSX has run
// everything before it, and memory GL gave up since the last wait is no longer read.
void wait_idle() {
    constexpr u32 kLabel = 255;
    const u64 began = __builtin_ppc_get_timebase();
    rsxSetWriteBackendLabel(g.context, kLabel, g.label);
    rsxFlushBuffer(g.context);
    u32 waited = 0;
    while (*static_cast<vu32*>(gcmGetLabelAddress(kLabel)) != g.label) {
        wait_a_little("waiting for the RSX to catch up", waited);
    }
    ++g.label;
    ++g.sync;
    g.ticks_waits += __builtin_ppc_get_timebase() - began;
}

namespace {

constexpr u32 kCommandBufferSize = 0x80000;  // 512 KB of commands between flushes
// Main memory the RSX may read (rsxInit's, megabytes whole): the command buffer alone, as buffers
// and textures are in the RSX's own memory. It comes out of the 213 MB a program has.
constexpr u32 kHostSize = 1024 * 1024;
constexpr u32 kHostAlignment = 1024 * 1024;

const GLubyte* get_string(GLenum name) {
    const char* text = "";
    switch (name) {
        case GL_VERSION:
            text = "4.1 OpenRAC on RSX";
            break;
        case GL_RENDERER:
            text = "RSX (PSL1GHT librsx)";
            break;
        case GL_VENDOR:
            text = "OpenRAC";
            break;
        case GL_SHADING_LANGUAGE_VERSION:
            text = "4.10";
            break;
        default:
            break;
    }
    return reinterpret_cast<const GLubyte*>(text);
}

void get_integer(GLenum name, GLint* data) {
    switch (name) {
        case GL_MAX_TEXTURE_SIZE:
            *data = 4096;
            break;  // the RSX's largest texture
        case GL_MAX_SAMPLES:
            *data = 1;
            break;
        case GL_VIEWPORT:
            std::memcpy(data, g.viewport, sizeof g.viewport);
            break;
        default:
            *data = 0;
            break;  // GL_NUM_EXTENSIONS among them: none
    }
}

void get_float(GLenum, GLfloat* data) {
    *data = 0.0f;
}

const Entry kContextFunctions[] = {
    OPENRAC_RSXGL_ENTRY(glGetString, get_string),
    OPENRAC_RSXGL_ENTRY(glGetIntegerv, get_integer),
    OPENRAC_RSXGL_ENTRY(glGetFloatv, get_float),
};

// What a GL function not carried out yet does: nothing, and answers zero (no error, no name, a
// null string).
template <typename Result, typename... Arguments>
Result OPENRAC_GLAPI nothing(Arguments...) {
    return Result();
}

// Every function gl::load asks for by name and stops without (renderer/gl/gl.h's two lists).
#define OPENRAC_RSXGL_NOTHING(ret, name, params) OPENRAC_RSXGL_ENTRY(name, nothing),
const Entry kEveryFunction[] = {OPENRAC_GL_FUNCTIONS(OPENRAC_RSXGL_NOTHING)
                                    OPENRAC_GL_DESKTOP_FUNCTIONS(OPENRAC_RSXGL_NOTHING)};
#undef OPENRAC_RSXGL_NOTHING

Proc find(const Entry* entries, std::size_t count, std::string_view wanted) {
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].name == wanted) {
            return entries[i].function;
        }
    }
    return nullptr;
}

// The RSX's state that the renderer's GL has no say in, said once: the registers hold what the
// program before this one left in them.
void set_fixed_state() {
    rsxSetAlphaTestEnable(g.context, GCM_FALSE);
    rsxSetAlphaFunc(g.context, GCM_ALWAYS, 0);
    rsxSetStencilTestEnable(g.context, GCM_FALSE);
    rsxSetTwoSidedStencilTestEnable(g.context, GCM_FALSE);
    rsxSetLogicOpEnable(g.context, GCM_FALSE);
    rsxSetAntialiasingControl(g.context, GCM_FALSE, GCM_FALSE, GCM_FALSE, 0xFFFF);
    rsxSetTwoSideLightEnable(g.context, GCM_FALSE);
    rsxSetPolygonOffsetFillEnable(g.context, GCM_FALSE);
    rsxSetRestartIndexEnable(g.context, GCM_FALSE);
    rsxSetShadeModel(g.context, GCM_SHADE_MODEL_SMOOTH);
}

}  // namespace

bool open(const Config& config, std::string& error) {
    void* host = memalign(kHostAlignment, kHostSize);
    if (host == nullptr) {
        error = "no memory for the RSX's command buffer";
        return false;
    }
    rsxInit(&g.context, kCommandBufferSize, kHostSize, host);
    if (g.context == nullptr) {
        error = "the RSX could not be started";
        return false;
    }
    videoState state;
    videoResolution resolution;
    if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0
        || videoGetResolution(state.displayMode.resolution, &resolution) != 0) {
        error = "the console's video mode could not be read";
        return false;
    }
    g.width = resolution.width;
    g.height = resolution.height;
    videoConfiguration video;
    std::memset(&video, 0, sizeof video);
    video.resolution = state.displayMode.resolution;
    video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = VIDEO_ASPECT_AUTO;
    // The screen's buffers are tiled regions too (rsxgl_internal.h, allocate_surface).
    const u32 tiled = surface_pitch(g.width);
    g.colour_pitch = g.depth_pitch = tiled != 0 ? tiled : g.width * 4;
    video.pitch = g.colour_pitch;
    if (videoConfigure(VIDEO_PRIMARY, &video, nullptr, 0) != 0) {
        error = "the console's video mode could not be set";
        return false;
    }
    wait_idle();
    gcmSetFlipMode(config.vsync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);

    for (u32 i = 0; i < kBuffers; ++i) {
        g.colour[i] = allocate_surface(g.colour_pitch, g.height);
        if (g.colour[i].address == nullptr) {
            error = "no RSX memory for the colour buffers";
            return false;
        }
        gcmSetDisplayBuffer(i, g.colour[i].offset, g.colour_pitch, g.width, g.height);
    }
    g.depth = allocate_surface(g.depth_pitch, g.height);
    if (g.depth.address == nullptr) {
        error = "no RSX memory for the depth buffer";
        return false;
    }
    if (!prepare_programs(error)) {
        return false;
    }
    g.drawn = 0;
    g.viewport[2] = g.scissor[2] = static_cast<GLint>(g.width);
    g.viewport[3] = g.scissor[3] = static_cast<GLint>(g.height);
    g.target_stale = true;
    set_fixed_state();
    gcmResetFlipStatus();
    g.open = true;
    return true;
}

void close() {
    if (!g.open) {
        return;
    }
    wait_idle();
    g.open = false;
}

bool active() {
    return g.open;
}

int width() {
    return static_cast<int>(g.width);
}

int height() {
    return static_cast<int>(g.height);
}

std::string device_name() {
    return std::format("RSX, {}x{}", g.width, g.height);
}

Proc get_proc(const char* name) {
    const std::string_view wanted(name);
    for (const auto& [entries, count] : {
             std::pair{static_cast<const Entry*>(kObjectFunctions), kObjectFunctionCount},
             std::pair{static_cast<const Entry*>(kDrawFunctions), kDrawFunctionCount},
             std::pair{static_cast<const Entry*>(kContextFunctions), std::size(kContextFunctions)},
             std::pair{static_cast<const Entry*>(kEveryFunction), std::size(kEveryFunction)},
         }) {
        if (const Proc found = find(entries, count, wanted)) {
            return found;
        }
    }
    return nullptr;
}

void present() {
    if (!g.open) {
        return;
    }
    wait_idle();
    gcmResetFlipStatus();
    gcmSetFlip(g.context, static_cast<u8>(g.drawn));
    rsxFlushBuffer(g.context);
    gcmSetWaitFlip(g.context);
    u32 waited = 0;
    const u64 flip_began = __builtin_ppc_get_timebase();
    while (gcmGetFlipStatus() != 0) {
        wait_a_little("waiting for the flip", waited);
    }
    g.ticks_waits += __builtin_ppc_get_timebase() - flip_began;
    g.drawn ^= 1;
    g.target_stale = true;
    // What the layer did, every two seconds of frames: a black picture with draws left out is a
    // program the layer has no RSX program for (README.md).
    constexpr u32 kFramesALine = 120;
    static const double ticks_a_ms = static_cast<double>(sysGetTimebaseFrequency()) / 1000.0;
    const auto ms_a_frame = [](u64 ticks) {
        return static_cast<double>(ticks) / ticks_a_ms / kFramesALine;
    };
    if (g.frame % kFramesALine == 0) {
        log::verbose(
            "rsxgl: {} frames: {} draws, {} left out, {} copies; ms a frame: {:.1f} a draw's "
            "state, {:.1f} the instances' constants, {:.1f} the draw commands, {:.1f} waiting",
            kFramesALine,
            g.draws,
            g.draws_left_out,
            g.copies,
            ms_a_frame(g.ticks_state),
            ms_a_frame(g.ticks_constants),
            ms_a_frame(g.ticks_commands),
            ms_a_frame(g.ticks_waits)
        );
        g.draws = g.draws_left_out = g.copies = 0;
        g.ticks_state = g.ticks_constants = g.ticks_commands = g.ticks_waits = 0;
    }
    // The frame is shown: nothing queued reads what GL gave up during it.
    ++g.frame;
    for (void* memory : g.retired) {
        u32 offset = 0;
        rsxAddressToOffset(memory, &offset);
        unbind_surface(offset);
        rsxFree(memory);
    }
    g.retired.clear();
}

}  // namespace openrac::rsxgl
