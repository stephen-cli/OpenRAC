// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// What the layer's files share: the display, GL's objects and state as the layer keeps them, and
// the functions of one file the others call. rsxgl.cpp has the display and the table of GL
// functions, rsxgl_objects.cpp the objects, rsxgl_draw.cpp what reaches the RSX.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "renderer/gl/gl.h"
#include "renderer/rsx/rsxgl.h"

#include <ppu-types.h>
#include <rsx/rsx.h>

namespace openrac::rsxgl {

inline constexpr u32 kBuffers = 2;      // the screen's colour buffers
inline constexpr u32 kAttributes = 16;  // the RSX's vertex attributes
// The units the layer keeps: the one its programs sample, and the palette's and the light
// texture's, which it reads itself (mesh::kPaletteUnit, mesh::kLightUnit).
inline constexpr u32 kTextureUnits = 3;

// A piece of the RSX's memory.
struct Memory {
    void* address = nullptr;
    u32 offset = 0;
};

// A texture, or a renderbuffer: A8R8G8B8 or 24-bit depth with stencil, its rows in GL's order
// (the first row is t = 0, and y = 0 of a framebuffer it is attached to). One level, or after
// glGenerateMipmap a chain: each level half the one above down to one texel, one after the
// other in the same memory, every one with the first's pitch (the RSX's layout of a texture
// that is not swizzled).
struct Image {
    Memory memory;
    u32 width = 0;
    u32 height = 0;
    u32 pitch = 0;
    u32 levels = 1;
    bool depth = false;
    bool surface = false;  // its memory is laid out for a tiled region (allocate_surface)
    gl::GLenum min_filter = gl::GL_LINEAR;  // GL's own default asks for a mip chain
    gl::GLenum mag_filter = gl::GL_LINEAR;
    gl::GLenum wrap_s = gl::GL_REPEAT;
    gl::GLenum wrap_t = gl::GL_REPEAT;
    u32 used = 0;  // State::sync when a draw last read or wrote it
    // An RGBA32F texture's numbers, four a texel, kept by the layer and not in the RSX's memory:
    // the mobys' joint matrices, which a draw hands the RSX as constants.
    std::vector<float> floats;
    u32 written = 0;  // counts the writes of its texels by glTexImage2D and glTexSubImage2D
    // Its first level's texels (A8R8G8B8, row after row) in the program's memory too, once a draw
    // has read it for an instance's colours (the light texture): the RSX's memory is slow for
    // the processor to read, and that read is made for every instance. As `written` was at
    // `kept_at`.
    mutable std::vector<u32> kept;
    mutable u32 kept_at = 0;
};

struct Sampler {
    gl::GLenum min_filter = gl::GL_LINEAR;  // GL's own default asks for a mip chain
    gl::GLenum mag_filter = gl::GL_LINEAR;
    gl::GLenum wrap_s = gl::GL_REPEAT;
    gl::GLenum wrap_t = gl::GL_REPEAT;
};

struct Buffer {
    Memory memory;
    u32 size = 0;
    u32 drawn = 0;    // State::frame when a draw last read it
    u32 written = 0;  // counts its writes: what the layer worked out of its bytes holds until one
    // Its bytes in the program's memory too, once a draw has read it for values an instance,
    // and written with it from then on: the RSX's memory is slow for the processor to read.
    std::vector<unsigned char> kept;
};

struct Attribute {
    bool enabled = false;
    gl::GLint size = 4;
    gl::GLenum type = gl::GL_FLOAT;
    bool normalized = false;
    gl::GLsizei stride = 0;
    u32 pointer = 0;
    gl::GLuint buffer = 0;
    u32 divisor = 0;  // 1: one value an instance, which the layer hands the program as a constant
};

struct VertexArray {
    std::array<Attribute, kAttributes> attributes{};
    gl::GLuint elements = 0;  // GL_ELEMENT_ARRAY_BUFFER, which GL keeps with the vertex array
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
    Sky,     // viewer/shaders/sky.vert and sky.frag
    Mesh,    // viewer/shaders/mesh.vert and mesh.frag
    Moby,    // the same GL program where its `skinning` is set: programs/moby.vp with mesh.fp
    Effect,  // renderer/shaders/effect.vert with direct.frag: programs/effect.vp with direct.fp
};

// A uniform of one of the layer's programs: its name in the GLSL, and where its numbers are kept
// in Program::uniforms, four to a slot (a matrix has four slots, its columns; an array one slot an
// element). A uniform's location is its first slot.
struct Uniform {
    std::string_view name;
    u32 slot;
    u32 slots;
};

// The effect program's uniforms' slots: the direct program's (DirectUniform), then its own.
namespace effect {
inline constexpr u32 kViewProjection = 12;
inline constexpr u32 kView = 16;
inline constexpr u32 kFogLine = 20;
inline constexpr u32 kSlots = 21;
}  // namespace effect

// The sky program's uniforms' slots (rsxgl_objects.cpp has their names).
namespace sky {
inline constexpr u32 kViewProjection = 0;
inline constexpr u32 kShell = 4;
inline constexpr u32 kTint = 8;
inline constexpr u32 kSlots = 10;
}  // namespace sky

// The mesh program's.
namespace mesh {
inline constexpr u32 kViewProjection = 0;
inline constexpr u32 kView = 4;
inline constexpr u32 kFogParams = 8;
inline constexpr u32 kFogColour = 9;
inline constexpr u32 kMaterialColour = 10;
inline constexpr u32 kCutout = 11;
inline constexpr u32 kLighting = 12;
inline constexpr u32 kGlowPass = 13;
inline constexpr u32 kTranslucentPass = 14;
inline constexpr u32 kEffectPass = 15;
inline constexpr u32 kMetalPass = 16;
inline constexpr u32 kSkinning = 17;
inline constexpr u32 kLightSets = 18;        // 64 slots
inline constexpr u32 kMobyPointLights = 82;  // 64 slots
inline constexpr u32 kGlowSplit = 146;
inline constexpr u32 kEffectReference = 147;
inline constexpr u32 kEffectAdditive = 148;
inline constexpr u32 kClipZeroToOne = 149;
inline constexpr u32 kSlots = 150;
// The attributes of an instance (scene/level_instances.cpp): the model's four columns, the tint,
// a moby's first joint matrix in the palette texture, a lit tie's first colour in the light
// texture (a moby's shine distance, as -1 - distance), a moby's lights and its ambient colour.
inline constexpr u32 kModel = 4;
inline constexpr u32 kTint = 8;
inline constexpr u32 kPalette = 9;
inline constexpr u32 kLights = 13;
inline constexpr u32 kMobyLight = 14;
inline constexpr u32 kMobyAmbient = 15;
// A moby's vertex: its four joints (bytes), their weights, its light slot.
inline constexpr u32 kJoints = 10;
inline constexpr u32 kWeights = 11;
inline constexpr u32 kLightSlot = 12;
// The unit of the palette texture (scene/level_renderer_internal.h's kPaletteUnit).
inline constexpr u32 kPaletteUnit = 1;
// The unit of the light texture, the lit ties' and shrubs' colours (kLightUnit there).
inline constexpr u32 kLightUnit = 2;
}  // namespace mesh

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
    std::vector<std::array<gl::GLfloat, 4>> uniforms;  // as many slots as its kind has
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
    u32 left_out_logged = 0;  // a bit for each reason a draw was left out that the log has had
    u32 copies = 0;
    // And the layer's time since that line, in ticks of the processor's time base: a draw's
    // state, an instance's constants, the draw commands themselves (which wait when the
    // command buffer is full), and the waits for the RSX to catch up and for the flip.
    u64 ticks_state = 0;
    u64 ticks_constants = 0;
    u64 ticks_commands = 0;
    u64 ticks_waits = 0;

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

// What the RSX draws into is a tiled region of its memory: rows of one of the RSX's own
// pitches, from a multiple of 64 KB and 64 KB whole, and bound as a region while it is drawn
// into. The RSX fills a pixel of such a region about three times as fast as one of memory
// it was told nothing of (README.md, "Tiled surfaces"); the bytes are where they would be
// without the region, so a surface is read and sampled as any other image.
// The pitch of a surface so wide in pixels; 0 where the RSX has none for a row so short.
u32 surface_pitch(u32 width);
Memory allocate_surface(u32 pitch, u32 rows);
// Binds the region of the surface at `offset` (it stays bound while there is room: the RSX
// has 15 regions, and the one longest out of use gives way).
void bind_surface(u32 offset, u32 pitch, u32 rows);
// The region of memory that is about to be freed.
void unbind_surface(u32 offset);
void retire(Memory& memory);
// The RSX's memory from the program's side, the layer's only touches of it. On a console a load
// of it wider than its address's alignment stops the program (README.md, "On a console"), and a
// compiler makes such loads of a memcpy and of neighbouring fields: these move a word at a time
// where the RSX's side is a word's place, a byte at a time before and after.
void read_memory(void* to, const void* rsx, std::size_t bytes);
void write_memory(void* rsx, const void* from, std::size_t bytes);
void fill_memory(void* rsx, unsigned char value, std::size_t bytes);

// A word of it, at a word's place.
inline u32 read_word(const void* rsx) {
    return *static_cast<const volatile u32*>(rsx);
}

inline void write_word(void* rsx, u32 word) {
    *static_cast<volatile u32*>(rsx) = word;
}

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
