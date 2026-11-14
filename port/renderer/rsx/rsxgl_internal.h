// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// What the layer's files share: the display, GL's objects and state as the layer keeps them, and
// the functions of one file the others call. rsxgl.cpp has the display and the table of GL
// functions, rsxgl_objects.cpp the objects, rsxgl_draw.cpp what reaches the RSX.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer/gl.h"
#include "renderer/rsx/rsxgl.h"

#include <ppu-types.h>
#include <rsx/rsx.h>

namespace openrac::rsxgl {

inline constexpr u32 kBuffers = 2;        // the screen's colour buffers
inline constexpr u32 kAttributes = 16;    // the RSX's vertex attributes
inline constexpr u32 kTextureUnits = 2;   // the units the layer's programs sample
inline constexpr u32 kUniformSlots = 16;  // the most uniforms one of the layer's programs has

// A piece of the RSX's memory.
struct Memory {
    void* address = nullptr;
    u32 offset = 0;
};

// A texture, or a renderbuffer: one level of A8R8G8B8 or of 24-bit depth with stencil, its rows
// in GL's order (the first row is t = 0, and y = 0 of a framebuffer it is attached to).
struct Image {
    Memory memory;
    u32 width = 0;
    u32 height = 0;
    u32 pitch = 0;
    bool depth = false;
    gl::GLenum min_filter = gl::GL_LINEAR;  // one level: GL's own default asks for a mip chain
    gl::GLenum mag_filter = gl::GL_LINEAR;
    gl::GLenum wrap_s = gl::GL_REPEAT;
    gl::GLenum wrap_t = gl::GL_REPEAT;
    u32 used = 0;  // State::sync when a draw last read or wrote it
};

struct Sampler {
    gl::GLenum min_filter = gl::GL_LINEAR;  // one level: GL's own default asks for a mip chain
    gl::GLenum mag_filter = gl::GL_LINEAR;
    gl::GLenum wrap_s = gl::GL_REPEAT;
    gl::GLenum wrap_t = gl::GL_REPEAT;
};

struct Buffer {
    Memory memory;
    u32 size = 0;
    u32 drawn = 0;  // State::frame when a draw last read it
};

struct Attribute {
    bool enabled = false;
    gl::GLint size = 4;
    gl::GLenum type = gl::GL_FLOAT;
    bool normalized = false;
    gl::GLsizei stride = 0;
    u32 pointer = 0;
    gl::GLuint buffer = 0;
};

struct VertexArray {
    std::array<Attribute, kAttributes> attributes{};
};

// What a framebuffer's attachment is: a texture's name or a renderbuffer's (both are Images).
struct Framebuffer {
    gl::GLuint colour = 0;
    gl::GLuint depth = 0;
};

struct Shader {
    gl::GLenum type = 0;
    std::string source;
};

// The programs the layer has for the RSX (programs/), found by a line in the GLSL of both stages.
enum class ProgramKind : std::uint8_t {
    None,    // no RSX program yet: its draws are left out
    Direct,  // renderer/shaders/direct.vert and direct.frag
};

// The direct program's uniforms: a uniform's location is its number here (rsxgl_objects.cpp has
// their names in this order).
enum class DirectUniform : std::uint8_t {
    PixelOffset,
    Textured,
    Tcc,
    Tfx,
    TexAlphaScale,
    FogEnable,
    FogColour,
    AlphaTest,
    AlphaRef,
    AlphaKeepFailing,
    SourceScale,
    SourceFix,
};

struct Program {
    gl::GLuint vertex = 0;
    gl::GLuint fragment = 0;
    ProgramKind kind = ProgramKind::None;
    std::array<std::array<gl::GLfloat, 4>, kUniformSlots> uniforms{};
};

struct State {
    // The display.
    gcmContextData* context = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 colour_pitch = 0;
    Memory colour[kBuffers];
    u32 depth_pitch = 0;
    Memory depth;
    u32 drawn = 0;  // the colour buffer drawn into
    u32 label = 1;
    u32 sync = 1;   // counts wait_idle: what was used at an earlier count the RSX has done with
    u32 frame = 1;  // counts present
    bool open = false;

    // GL's objects. A texture and a renderbuffer share `images`: their names never collide.
    gl::GLuint next_name = 1;
    std::unordered_map<gl::GLuint, Image> images;
    std::unordered_map<gl::GLuint, Sampler> samplers;
    std::unordered_map<gl::GLuint, Buffer> buffers;
    std::unordered_map<gl::GLuint, VertexArray> vertex_arrays;
    std::unordered_map<gl::GLuint, Framebuffer> framebuffers;
    std::unordered_map<gl::GLuint, Shader> shaders;
    std::unordered_map<gl::GLuint, Program> programs;
    std::vector<void*> retired;  // memory GL gave up, given back when the frame has been shown

    // GL's bindings.
    gl::GLuint draw_framebuffer = 0;  // 0 is the screen
    gl::GLuint read_framebuffer = 0;
    gl::GLuint renderbuffer = 0;
    gl::GLuint array_buffer = 0;
    gl::GLuint vertex_array = 0;
    gl::GLuint program = 0;
    u32 unit = 0;  // glActiveTexture
    gl::GLuint texture[kTextureUnits] = {};
    gl::GLuint sampler[kTextureUnits] = {};

    // GL's state.
    gl::GLint viewport[4] = {};
    gl::GLint scissor[4] = {};
    bool scissor_test = false;
    bool blend = false;
    gl::GLenum blend_equation = gl::GL_FUNC_ADD;
    gl::GLenum blend_factor[4] =
        {gl::GL_ONE, gl::GL_ZERO, gl::GL_ONE, gl::GL_ZERO};  // source, destination; alpha's
    u32 blend_colour = 0;                                    // A8R8G8B8
    bool depth_test = false;
    gl::GLenum depth_function = gl::GL_LESS;
    bool depth_write = true;
    bool depth_clamp = false;
    bool colour_write[4] = {true, true, true, true};
    bool cull = false;
    gl::GLenum cull_face = gl::GL_BACK;
    gl::GLenum front_face = gl::GL_CCW;
    u32 clear_colour = 0;  // A8R8G8B8
    gl::GLfloat clear_depth = 1.0f;

    // Counted for the log (present): glDrawArrays carried out, those left out for want of an RSX
    // program, and copies between framebuffers.
    u32 draws = 0;
    u32 draws_left_out = 0;
    u32 copies = 0;

    // What the RSX was last told, so that a draw tells it only what changed.
    bool target_stale = true;  // the surface, and with it the viewport and scissor
    bool state_stale = true;
    ProgramKind loaded = ProgramKind::None;
    u32 attributes_bound = 0;  // a bit for each attribute the RSX has a vertex array for
};

extern State g;

// rsxgl.cpp
// Waits until the RSX has run every command sent so far.
void wait_idle();

// rsxgl_objects.cpp
Memory allocate(u32 bytes);
void retire(Memory& memory);
// The image of that name with memory behind it, or null.
Image* image(gl::GLuint name);
// Waits for the RSX when it may still read what is about to be written.
void before_write(u32 used);

// rsxgl_draw.cpp
// Where a framebuffer's draws go. False when it has nothing to draw into.
struct Target {
    const Image* colour = nullptr;  // null on the screen and on a depth-only framebuffer
    const Image* depth = nullptr;   // null on the screen and without a depth attachment
    u32 width = 0;
    u32 height = 0;
    bool screen = false;  // its rows run downwards: y is turned over
};

bool target_of(gl::GLuint framebuffer, Target& target);
// The programs' microcode into the RSX's memory, the texture every unit starts with.
bool prepare_programs(std::string& error);

// The GL functions of each file, for the table.
struct Entry {
    const char* name;
    Proc function;
};

template <typename Function>
constexpr Proc as_proc(Function function) {
    return reinterpret_cast<Proc>(function);
}

#define OPENRAC_RSXGL_ENTRY(name, function)                                                        \
    Entry {                                                                                        \
        #name, as_proc(static_cast<gl::PFN_##name>(function))                                      \
    }

extern const Entry kObjectFunctions[];
extern const std::size_t kObjectFunctionCount;
extern const Entry kDrawFunctions[];
extern const std::size_t kDrawFunctionCount;

}  // namespace openrac::rsxgl
