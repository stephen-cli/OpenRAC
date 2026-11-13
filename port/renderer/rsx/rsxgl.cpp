// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// rsxgl.h: the display, the table of GL functions, and the ones carried out so far (README.md).

#include "renderer/rsx/rsxgl.h"

#include <cstddef>
#include <cstring>
#include <format>
#include <malloc.h>
#include <string_view>
#include <unistd.h>

#include "renderer/gl.h"

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

namespace openrac::rsxgl {
namespace {

using namespace openrac::gl;

constexpr u32 kCommandBufferSize = 0x80000;  // 512 KB of commands between flushes
constexpr u32 kHostSize = 32 * 1024 * 1024;  // main memory the RSX may read (rsxInit's)
constexpr u32 kHostAlignment = 1024 * 1024;
constexpr u32 kLabel = 255;  // the label the RSX writes when it has caught up
constexpr u32 kBuffers = 2;

struct State {
    gcmContextData* context = nullptr;
    videoResolution resolution{};
    u32 colour_pitch = 0;
    u32 colour_offset[kBuffers] = {};
    u32* colour[kBuffers] = {};
    u32 depth_pitch = 0;
    u32 depth_offset = 0;
    u32 drawn = 0;  // the colour buffer drawn into
    u32 shown = 0;  // the one presented last
    u32 label = 1;
    bool vsync = true;
    bool open = false;

    // GL's state, as far as the layer keeps it.
    GLuint framebuffer = 0;  // the draw framebuffer bound: 0 is the screen
    u32 clear_colour = 0;    // A8R8G8B8
    GLuint next_name = 1;    // names handed out by glGen* and glCreate*
};

State g;

// Waits until the RSX has run every command sent so far.
void wait_idle() {
    rsxSetWriteBackendLabel(g.context, kLabel, g.label);
    rsxFlushBuffer(g.context);
    while (*static_cast<vu32*>(gcmGetLabelAddress(kLabel)) != g.label) {
        usleep(100);
    }
    ++g.label;
}

void set_target(u32 index) {
    gcmSurface surface;
    std::memset(&surface, 0, sizeof surface);
    surface.colorFormat = GCM_SURFACE_X8R8G8B8;
    surface.colorTarget = GCM_SURFACE_TARGET_0;
    for (int i = 0; i < 4; ++i) {
        surface.colorLocation[i] = GCM_LOCATION_RSX;
        surface.colorPitch[i] = 64;
    }
    surface.colorOffset[0] = g.colour_offset[index];
    surface.colorPitch[0] = g.colour_pitch;
    surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    surface.depthLocation = GCM_LOCATION_RSX;
    surface.depthOffset = g.depth_offset;
    surface.depthPitch = g.depth_pitch;
    surface.type = GCM_SURFACE_TYPE_LINEAR;
    surface.antiAlias = GCM_SURFACE_CENTER_1;
    surface.width = g.resolution.width;
    surface.height = g.resolution.height;
    rsxSetSurface(g.context, &surface);
    rsxSetColorMask(
        g.context, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A
    );
    rsxSetScissor(g.context, 0, 0, g.resolution.width, g.resolution.height);
}

// ---- The GL functions carried out ----

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
            data[0] = 0;
            data[1] = 0;
            data[2] = width();
            data[3] = height();
            break;
        default:
            *data = 0;
            break;  // GL_NUM_EXTENSIONS among them: none
    }
}

void get_float(GLenum, GLfloat* data) {
    *data = 0.0f;
}

// glGenTextures and its like: `count` names nothing else has.
void generate(GLsizei count, GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        names[i] = g.next_name++;
    }
}

GLuint create_shader(GLenum) {
    return g.next_name++;
}

GLuint create_program() {
    return g.next_name++;
}

// A shader compiled and a program linked: nothing is compiled here (the RSX's programs are made
// ahead of time), so every one reports success and an empty log.
void get_status(GLuint, GLenum name, GLint* value) {
    *value = name == GL_COMPILE_STATUS || name == GL_LINK_STATUS ? GL_TRUE : 0;
}

GLint uniform_location(GLuint, const GLchar*) {
    return -1;  // no uniform is there yet: GL ignores a value set at -1
}

GLenum framebuffer_status(GLenum) {
    return GL_FRAMEBUFFER_COMPLETE;
}

void bind_framebuffer(GLenum target, GLuint framebuffer) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        g.framebuffer = framebuffer;
    }
}

void clear_colour(GLfloat r, GLfloat green, GLfloat b, GLfloat a) {
    const auto channel = [](GLfloat value) {
        const GLfloat clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
        return static_cast<u32>(clamped * 255.0f + 0.5f);
    };
    g.clear_colour = channel(a) << 24 | channel(r) << 16 | channel(green) << 8 | channel(b);
}

// The screen's clear. An off-screen framebuffer has no memory behind it yet and is left alone.
void clear(GLbitfield mask) {
    if (g.framebuffer != 0) {
        return;
    }
    u32 what = 0;
    if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
        what |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
        rsxSetClearColor(g.context, g.clear_colour);
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) != 0) {
        what |= GCM_CLEAR_Z;
    }
    if ((mask & GL_STENCIL_BUFFER_BIT) != 0) {
        what |= GCM_CLEAR_S;
    }
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0) {
        rsxSetClearDepthStencil(g.context, 0);  // reversed depth: far is 0 (renderer/README.md)
    }
    if (what != 0) {
        rsxClearSurface(g.context, what);
    }
}

// The screen's pixels, bottom row first as GL has them, as bytes or as floats (the port's
// screenshots, OPENRAC_SHOT, ask for either). The colour buffer is the program's own memory, read
// once the RSX has drawn everything sent; under RPCS3 that needs "Write Color Buffers".
void read_pixels(
    GLint x, GLint y, GLsizei wide, GLsizei high, GLenum format, GLenum type, void* pixels
) {
    if (g.framebuffer != 0 || format != GL_RGBA || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)) {
        return;
    }
    wait_idle();
    const u32* picture = g.colour[g.drawn];
    auto* bytes = static_cast<unsigned char*>(pixels);
    auto* floats = static_cast<GLfloat*>(pixels);
    for (GLsizei row = 0; row < high; ++row) {
        const int from_top = height() - 1 - (y + row);
        for (GLsizei column = 0; column < wide; ++column) {
            const int across = x + column;
            u32 pixel = 0;  // X8R8G8B8, read as the console's own word
            if (from_top >= 0 && from_top < height() && across >= 0 && across < width()) {
                pixel = picture
                    [static_cast<u32>(from_top) * static_cast<u32>(width())
                     + static_cast<u32>(across)];
            }
            const unsigned char rgba[4] = {
                static_cast<unsigned char>(pixel >> 16),
                static_cast<unsigned char>(pixel >> 8),
                static_cast<unsigned char>(pixel),
                0xFF,
            };
            const std::size_t at = (static_cast<std::size_t>(row) * static_cast<std::size_t>(wide)
                                    + static_cast<std::size_t>(column))
                                   * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                if (type == GL_FLOAT) {
                    floats[at + channel] = static_cast<GLfloat>(rgba[channel]) / 255.0f;
                } else {
                    bytes[at + channel] = rgba[channel];
                }
            }
        }
    }
}

// ---- The table ----

// What a GL function not carried out yet does: nothing, and answers zero (no error, no name, a
// null string).
template <typename Result, typename... Arguments>
Result OPENRAC_GLAPI nothing(Arguments...) {
    return Result();
}

struct Entry {
    std::string_view name;
    Proc function;
};

template <typename Function>
constexpr Proc as_proc(Function function) {
    return reinterpret_cast<Proc>(function);
}

// The functions carried out, looked up before the table of every function below.
const Entry kCarriedOut[] = {
    {"glGetString", as_proc(static_cast<PFN_glGetString>(get_string))},
    {"glGetIntegerv", as_proc(static_cast<PFN_glGetIntegerv>(get_integer))},
    {"glGetFloatv", as_proc(static_cast<PFN_glGetFloatv>(get_float))},
    {"glGenTextures", as_proc(static_cast<PFN_glGenTextures>(generate))},
    {"glGenSamplers", as_proc(static_cast<PFN_glGenSamplers>(generate))},
    {"glGenBuffers", as_proc(static_cast<PFN_glGenBuffers>(generate))},
    {"glGenVertexArrays", as_proc(static_cast<PFN_glGenVertexArrays>(generate))},
    {"glGenFramebuffers", as_proc(static_cast<PFN_glGenFramebuffers>(generate))},
    {"glGenRenderbuffers", as_proc(static_cast<PFN_glGenRenderbuffers>(generate))},
    {"glCreateShader", as_proc(static_cast<PFN_glCreateShader>(create_shader))},
    {"glCreateProgram", as_proc(static_cast<PFN_glCreateProgram>(create_program))},
    {"glGetShaderiv", as_proc(static_cast<PFN_glGetShaderiv>(get_status))},
    {"glGetProgramiv", as_proc(static_cast<PFN_glGetProgramiv>(get_status))},
    {"glGetUniformLocation", as_proc(static_cast<PFN_glGetUniformLocation>(uniform_location))},
    {"glCheckFramebufferStatus",
     as_proc(static_cast<PFN_glCheckFramebufferStatus>(framebuffer_status))},
    {"glBindFramebuffer", as_proc(static_cast<PFN_glBindFramebuffer>(bind_framebuffer))},
    {"glClearColor", as_proc(static_cast<PFN_glClearColor>(clear_colour))},
    {"glClear", as_proc(static_cast<PFN_glClear>(clear))},
    {"glReadPixels", as_proc(static_cast<PFN_glReadPixels>(read_pixels))},
};

// Every function gl::load asks for by name and stops without (renderer/gl.h's two lists).
#define OPENRAC_RSXGL_NOTHING(ret, name, params) {#name, as_proc(static_cast<PFN_##name>(nothing))},
const Entry kEveryFunction[] = {OPENRAC_GL_FUNCTIONS(OPENRAC_RSXGL_NOTHING)
                                    OPENRAC_GL_DESKTOP_FUNCTIONS(OPENRAC_RSXGL_NOTHING)};
#undef OPENRAC_RSXGL_NOTHING

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
    if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0
        || videoGetResolution(state.displayMode.resolution, &g.resolution) != 0) {
        error = "the console's video mode could not be read";
        return false;
    }
    videoConfiguration video;
    std::memset(&video, 0, sizeof video);
    video.resolution = state.displayMode.resolution;
    video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = VIDEO_ASPECT_AUTO;
    video.pitch = g.resolution.width * 4;
    if (videoConfigure(VIDEO_PRIMARY, &video, nullptr, 0) != 0) {
        error = "the console's video mode could not be set";
        return false;
    }
    wait_idle();
    g.vsync = config.vsync;
    gcmSetFlipMode(g.vsync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);

    g.colour_pitch = g.resolution.width * 4;
    g.depth_pitch = g.resolution.width * 4;
    for (u32 i = 0; i < kBuffers; ++i) {
        g.colour[i] = static_cast<u32*>(rsxMemalign(64, g.resolution.height * g.colour_pitch));
        if (g.colour[i] == nullptr) {
            error = "no RSX memory for the colour buffers";
            return false;
        }
        rsxAddressToOffset(g.colour[i], &g.colour_offset[i]);
        gcmSetDisplayBuffer(
            i, g.colour_offset[i], g.colour_pitch, g.resolution.width, g.resolution.height
        );
    }
    void* depth = rsxMemalign(64, g.resolution.height * g.depth_pitch);
    if (depth == nullptr) {
        error = "no RSX memory for the depth buffer";
        return false;
    }
    rsxAddressToOffset(depth, &g.depth_offset);
    g.drawn = 0;
    g.shown = 0;
    set_target(g.drawn);
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
    return g.resolution.width;
}

int height() {
    return g.resolution.height;
}

std::string device_name() {
    return std::format("RSX, {}x{}", width(), height());
}

Proc get_proc(const char* name) {
    const std::string_view wanted(name);
    for (const Entry& entry : kCarriedOut) {
        if (entry.name == wanted) {
            return entry.function;
        }
    }
    for (const Entry& entry : kEveryFunction) {
        if (entry.name == wanted) {
            return entry.function;
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
    gcmSetFlip(g.context, g.drawn);
    rsxFlushBuffer(g.context);
    gcmSetWaitFlip(g.context);
    while (gcmGetFlipStatus() != 0) {
        usleep(200);
    }
    g.shown = g.drawn;
    g.drawn ^= 1;
    set_target(g.drawn);
}

}  // namespace openrac::rsxgl
