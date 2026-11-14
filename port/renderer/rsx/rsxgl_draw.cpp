// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// What reaches the RSX (rsxgl_internal.h): the surface drawn into, GL's state, the layer's
// programs and their numbers, draws, clears, and the copies between framebuffers.
//
// Which way up. A texture's and an off-screen framebuffer's rows are in GL's order in memory, the
// first row y = 0, so what is drawn into a texture is sampled as GL samples it. The screen's rows
// run downwards. The RSX's own window coordinates count rows of memory, so the viewport turns y
// over on the screen and nowhere else, and with it the winding that faces front.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log.h"
#include "renderer/rsx/rsxgl_internal.h"
#include "rsx_programs.h"

namespace openrac::rsxgl {

using namespace openrac::gl;

namespace {

// ---- The programs ----

struct Loaded {
    const rsxVertexProgram* vertex = nullptr;
    const void* vertex_code = nullptr;
    const rsxFragmentProgram* fragment = nullptr;
    Memory fragment_code;  // in the RSX's memory, where the RSX reads a fragment program from
};

Loaded g_direct;
Image g_white;  // one white texel: what a unit samples when the draw binds nothing

// The layer's own vertices for a copy, in the direct program's layout.
struct CopyVertex {
    float x, y, z;
    float s, t, q;
    unsigned char rgba[4];
    float fog;
};

constexpr u32 kCopyVertices = 6;

bool load(Loaded& into, const unsigned char* vertex, const unsigned char* fragment) {
    into.vertex = reinterpret_cast<const rsxVertexProgram*>(vertex);
    into.fragment = reinterpret_cast<const rsxFragmentProgram*>(fragment);
    void* code = nullptr;
    u32 size = 0;
    rsxVertexProgramGetUCode(into.vertex, &code, &size);
    into.vertex_code = code;
    rsxFragmentProgramGetUCode(into.fragment, &code, &size);
    into.fragment_code = allocate(size);
    if (into.fragment_code.address == nullptr) {
        return false;
    }
    std::memcpy(into.fragment_code.address, code, size);
    return true;
}

// ---- The surface ----

u32 turned(const Target& target, GLint y, GLint high) {
    const GLint top = target.screen ? static_cast<GLint>(target.height) - y - high : y;
    return static_cast<u32>(std::clamp(top, 0, static_cast<GLint>(target.height)));
}

void apply_viewport(const Target& target) {
    const auto x = static_cast<float>(g.viewport[0]);
    const auto y = static_cast<float>(g.viewport[1]);
    const auto wide = static_cast<float>(g.viewport[2]);
    const auto high = static_cast<float>(g.viewport[3]);
    // GL's depth: -1 to 1 in device coordinates is 0 to 1 in the buffer.
    const float scale[4] = {wide * 0.5f, target.screen ? -high * 0.5f : high * 0.5f, 0.5f, 0.0f};
    const float offset[4] = {
        x + wide * 0.5f,
        target.screen ? static_cast<float>(target.height) - y - high * 0.5f : y + high * 0.5f,
        0.5f,
        0.0f,
    };
    rsxSetViewport(
        g.context,
        static_cast<u16>(std::clamp(g.viewport[0], 0, 4095)),
        static_cast<u16>(turned(target, g.viewport[1], g.viewport[3])),
        static_cast<u16>(std::clamp(g.viewport[2], 0, 4096)),
        static_cast<u16>(std::clamp(g.viewport[3], 0, 4096)),
        0.0f,
        1.0f,
        scale,
        offset
    );
}

void apply_scissor(const Target& target) {
    if (!g.scissor_test) {
        rsxSetScissor(
            g.context, 0, 0, static_cast<u16>(target.width), static_cast<u16>(target.height)
        );
        return;
    }
    const GLint x0 = std::clamp(g.scissor[0], 0, static_cast<GLint>(target.width));
    const GLint x1 = std::clamp(g.scissor[0] + g.scissor[2], x0, static_cast<GLint>(target.width));
    const GLint y0 = std::clamp(g.scissor[1], 0, static_cast<GLint>(target.height));
    const GLint y1 = std::clamp(g.scissor[1] + g.scissor[3], y0, static_cast<GLint>(target.height));
    rsxSetScissor(
        g.context,
        static_cast<u16>(x0),
        static_cast<u16>(turned(target, y0, y1 - y0)),
        static_cast<u16>(x1 - x0),
        static_cast<u16>(y1 - y0)
    );
}

// Makes `target` the RSX's surface; on the screen, the colour buffer of that number.
void set_surface(const Target& target, u32 screen_buffer) {
    gcmSurface surface;
    std::memset(&surface, 0, sizeof surface);
    surface.type = GCM_SURFACE_TYPE_LINEAR;
    surface.antiAlias = GCM_SURFACE_CENTER_1;
    surface.width = static_cast<u16>(target.width);
    surface.height = static_cast<u16>(target.height);
    for (int i = 0; i < 4; ++i) {
        surface.colorLocation[i] = GCM_LOCATION_RSX;
        surface.colorPitch[i] = 64;
    }
    // The screen's too, though its alpha is never shown: onto an X8R8G8B8 surface RPCS3 blended
    // with a source alpha of one (tools/ps3/rsxgl-test; the console itself was not asked).
    surface.colorFormat = GCM_SURFACE_A8R8G8B8;
    surface.colorTarget = GCM_SURFACE_TARGET_0;
    if (target.screen) {
        surface.colorOffset[0] = g.colour[screen_buffer].offset;
        surface.colorPitch[0] = g.colour_pitch;
    } else if (target.colour != nullptr) {
        surface.colorOffset[0] = target.colour->memory.offset;
        surface.colorPitch[0] = target.colour->pitch;
    } else {
        surface.colorTarget = GCM_SURFACE_TARGET_NONE;
    }
    // A surface has a depth buffer whether GL attached one or not: without one the screen's
    // stands in, and the draws neither test nor write depth (apply_state).
    surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    surface.depthLocation = GCM_LOCATION_RSX;
    surface.depthOffset = target.depth != nullptr ? target.depth->memory.offset : g.depth.offset;
    surface.depthPitch = target.depth != nullptr ? target.depth->pitch : g.depth_pitch;
    rsxSetSurface(g.context, &surface);
}

// The surface the RSX draws into. False when the framebuffer has nothing to draw into.
bool apply_target(Target& target) {
    if (!target_of(g.draw_framebuffer, target)) {
        return false;
    }
    if (g.target_stale) {
        set_surface(target, g.drawn);
        g.target_stale = false;
        g.state_stale = true;
    }
    return true;
}

// Takes the RSX off the surface it draws into, onto the screen's other colour buffer, where
// nothing is drawn. RPCS3 writes a surface back to the program's memory when it stops being the
// target, which a read of that memory waits for.
void park_target() {
    Target screen;
    target_of(0, screen);
    set_surface(screen, g.drawn ^ 1);
    g.target_stale = true;
}

// ---- GL's state ----

u32 colour_mask() {
    return (g.colour_write[0] ? GCM_COLOR_MASK_R : 0) | (g.colour_write[1] ? GCM_COLOR_MASK_G : 0)
           | (g.colour_write[2] ? GCM_COLOR_MASK_B : 0)
           | (g.colour_write[3] ? GCM_COLOR_MASK_A : 0);
}

// GL's blend factors, equations and comparisons have the RSX's own numbers.
void apply_state(const Target& target) {
    if (!g.state_stale) {
        return;
    }
    apply_viewport(target);
    apply_scissor(target);
    const bool has_depth = target.screen || target.depth != nullptr;
    rsxSetDepthTestEnable(g.context, g.depth_test && has_depth ? GCM_TRUE : GCM_FALSE);
    rsxSetDepthFunc(g.context, g.depth_function);
    rsxSetDepthWriteEnable(g.context, g.depth_write && has_depth ? GCM_TRUE : GCM_FALSE);
    rsxSetZMinMaxControl(
        g.context,
        g.depth_clamp ? GCM_FALSE : GCM_TRUE,
        g.depth_clamp ? GCM_TRUE : GCM_FALSE,
        GCM_FALSE
    );
    rsxSetColorMask(g.context, colour_mask());
    rsxSetBlendEnable(g.context, g.blend ? GCM_TRUE : GCM_FALSE);
    rsxSetBlendEquation(
        g.context, static_cast<u16>(g.blend_equation), static_cast<u16>(g.blend_equation)
    );
    rsxSetBlendFunc(
        g.context,
        static_cast<u16>(g.blend_factor[0]),
        static_cast<u16>(g.blend_factor[1]),
        static_cast<u16>(g.blend_factor[2]),
        static_cast<u16>(g.blend_factor[3])
    );
    rsxSetBlendColor(g.context, g.blend_colour, 0);
    rsxSetCullFaceEnable(g.context, g.cull ? GCM_TRUE : GCM_FALSE);
    rsxSetCullFace(g.context, g.cull_face == GL_FRONT ? GCM_CULL_FRONT : GCM_CULL_BACK);
    // Off the screen the picture is upside down in the RSX's eyes, and so is every winding.
    const bool counter = (g.front_face == GL_CCW) == target.screen;
    rsxSetFrontFace(g.context, counter ? GCM_FRONTFACE_CCW : GCM_FRONTFACE_CW);
    g.state_stale = false;
}

void enable(GLenum what, bool on) {
    switch (what) {
        case GL_SCISSOR_TEST:
            g.scissor_test = on;
            break;
        case GL_BLEND:
            g.blend = on;
            break;
        case GL_DEPTH_TEST:
            g.depth_test = on;
            break;
        case GL_DEPTH_CLAMP:
            g.depth_clamp = on;
            break;
        case GL_CULL_FACE:
            g.cull = on;
            break;
        default:
            return;
    }
    g.state_stale = true;
}

void gl_enable(GLenum what) {
    enable(what, true);
}

void gl_disable(GLenum what) {
    enable(what, false);
}

void gl_viewport(GLint x, GLint y, GLsizei wide, GLsizei high) {
    g.viewport[0] = x;
    g.viewport[1] = y;
    g.viewport[2] = wide;
    g.viewport[3] = high;
    g.state_stale = true;
}

void gl_scissor(GLint x, GLint y, GLsizei wide, GLsizei high) {
    g.scissor[0] = x;
    g.scissor[1] = y;
    g.scissor[2] = wide;
    g.scissor[3] = high;
    g.state_stale = true;
}

void depth_function(GLenum function) {
    g.depth_function = function;
    g.state_stale = true;
}

void depth_mask(GLboolean on) {
    g.depth_write = on != GL_FALSE;
    g.state_stale = true;
}

void colour_write(GLboolean r, GLboolean green, GLboolean b, GLboolean a) {
    g.colour_write[0] = r != GL_FALSE;
    g.colour_write[1] = green != GL_FALSE;
    g.colour_write[2] = b != GL_FALSE;
    g.colour_write[3] = a != GL_FALSE;
    g.state_stale = true;
}

void blend_function(
    GLenum source, GLenum destination, GLenum source_alpha, GLenum destination_alpha
) {
    g.blend_factor[0] = source;
    g.blend_factor[1] = destination;
    g.blend_factor[2] = source_alpha;
    g.blend_factor[3] = destination_alpha;
    g.state_stale = true;
}

void blend_equation(GLenum equation) {
    g.blend_equation = equation;
    g.state_stale = true;
}

u32 packed_colour(GLfloat r, GLfloat green, GLfloat b, GLfloat a) {
    const auto channel = [](GLfloat value) {
        return static_cast<u32>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return channel(a) << 24 | channel(r) << 16 | channel(green) << 8 | channel(b);
}

void blend_colour(GLfloat r, GLfloat green, GLfloat b, GLfloat a) {
    g.blend_colour = packed_colour(r, green, b, a);
    g.state_stale = true;
}

void cull_face(GLenum face) {
    g.cull_face = face;
    g.state_stale = true;
}

void front_face(GLenum winding) {
    g.front_face = winding;
    g.state_stale = true;
}

void clear_colour(GLfloat r, GLfloat green, GLfloat b, GLfloat a) {
    g.clear_colour = packed_colour(r, green, b, a);
}

void clear_depth(GLdouble depth) {
    g.clear_depth = static_cast<GLfloat>(depth);
}

// As GL clears: within the scissor, and only what the masks let be written.
void clear(GLbitfield mask) {
    Target target;
    if (!apply_target(target)) {
        return;
    }
    apply_state(target);
    u32 what = 0;
    if ((mask & GL_COLOR_BUFFER_BIT) != 0 && (target.screen || target.colour != nullptr)) {
        what |= (g.colour_write[0] ? GCM_CLEAR_R : 0) | (g.colour_write[1] ? GCM_CLEAR_G : 0)
                | (g.colour_write[2] ? GCM_CLEAR_B : 0) | (g.colour_write[3] ? GCM_CLEAR_A : 0);
        rsxSetClearColor(g.context, g.clear_colour);
    }
    const bool has_depth = target.screen || target.depth != nullptr;
    if (has_depth && (mask & GL_DEPTH_BUFFER_BIT) != 0 && g.depth_write) {
        what |= GCM_CLEAR_Z;
    }
    if (has_depth && (mask & GL_STENCIL_BUFFER_BIT) != 0) {
        what |= GCM_CLEAR_S;
    }
    if ((what & (GCM_CLEAR_Z | GCM_CLEAR_S)) != 0) {
        const auto depth =
            static_cast<u32>(std::clamp(g.clear_depth, 0.0f, 1.0f) * 16777215.0f + 0.5f);
        rsxSetClearDepthStencil(g.context, depth << 8);
    }
    if (what != 0) {
        rsxClearSurface(g.context, what);
    }
}

// ---- Textures ----

u8 filter(GLenum gl) {
    // One level a texture: a mip filter (the renderer's are linear) is linear within the level.
    return gl == GL_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
}

u8 wrap(GLenum gl) {
    return gl == GL_CLAMP_TO_EDGE ? GCM_TEXTURE_CLAMP_TO_EDGE : GCM_TEXTURE_REPEAT;
}

// A texture on a unit of the RSX, sampled as `sampler` says (the texture's own way without one).
void load_texture(u32 unit, Image& texture, const Sampler* sampler) {
    gcmTexture loaded;
    std::memset(&loaded, 0, sizeof loaded);
    loaded.format = static_cast<u8>(
        (texture.depth ? GCM_TEXTURE_FORMAT_DEPTH24_D8 : GCM_TEXTURE_FORMAT_A8R8G8B8)
        | GCM_TEXTURE_FORMAT_LIN
    );
    loaded.mipmap = 1;
    loaded.dimension = GCM_TEXTURE_DIMS_2D;
    loaded.cubemap = GCM_FALSE;
    loaded.remap = GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT
                   | GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT
                   | GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT
                   | GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT
                   | GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT
                   | GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT
                   | GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT
                   | GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT;
    loaded.width = static_cast<u16>(texture.width);
    loaded.height = static_cast<u16>(texture.height);
    loaded.depth = 1;
    loaded.location = GCM_LOCATION_RSX;
    loaded.pitch = texture.pitch;
    loaded.offset = texture.memory.offset;
    const auto index = static_cast<u8>(unit);
    rsxLoadTexture(g.context, index, &loaded);
    rsxTextureControl(g.context, index, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(
        g.context,
        index,
        0,
        filter(sampler != nullptr ? sampler->min_filter : texture.min_filter),
        filter(sampler != nullptr ? sampler->mag_filter : texture.mag_filter),
        GCM_TEXTURE_CONVOLUTION_QUINCUNX
    );
    rsxTextureWrapMode(
        g.context,
        index,
        wrap(sampler != nullptr ? sampler->wrap_s : texture.wrap_s),
        wrap(sampler != nullptr ? sampler->wrap_t : texture.wrap_t),
        GCM_TEXTURE_CLAMP_TO_EDGE,
        0,
        GCM_TEXTURE_ZFUNC_LESS,
        0
    );
    texture.used = g.sync;
}

// The texture GL has on a unit, or the white texel.
void load_bound_texture(u32 unit) {
    Image* texture = image(g.texture[unit]);
    const auto sampler = g.samplers.find(g.sampler[unit]);
    load_texture(
        unit,
        texture != nullptr ? *texture : g_white,
        texture != nullptr && sampler != g.samplers.end() ? &sampler->second : nullptr
    );
}

// ---- The direct program's numbers ----

constexpr float kOpaque = 255.0f / 128.0f;  // the chip's 0x80 is 1

// direct.vp's seven constants (programs/direct.fp says what each is) from direct.frag's uniforms.
void direct_constants(const Program& program, float c[7][4]) {
    const auto value = [&](DirectUniform uniform) {
        return program.uniforms[static_cast<std::size_t>(uniform)][0];
    };
    std::memset(c, 0, sizeof(float) * 7 * 4);
    const auto& offset = program.uniforms[static_cast<std::size_t>(DirectUniform::PixelOffset)];
    c[0][0] = offset[0];
    c[0][1] = offset[1];
    // The texture function.
    const bool textured = value(DirectUniform::Textured) != 0.0f;
    const bool texture_alpha = textured && value(DirectUniform::Tcc) != 0.0f;
    const int function = static_cast<int>(value(DirectUniform::Tfx));
    const float alpha_scale = value(DirectUniform::TexAlphaScale);
    if (!textured) {
        c[1][3] = 1.0f;
    } else if (function == 1) {
        c[1][1] = 1.0f;
    } else {
        c[1][0] = kOpaque;
        c[1][2] = function >= 2 ? 1.0f : 0.0f;
    }
    if (!texture_alpha) {
        c[2][2] = 1.0f;
    } else if (function == 0) {
        c[2][0] = kOpaque * alpha_scale;
    } else {
        c[2][1] = alpha_scale;
        c[2][2] = function == 2 ? 1.0f : 0.0f;
    }
    // Fog.
    const bool fog = value(DirectUniform::FogEnable) != 0.0f;
    const auto& fog_colour = program.uniforms[static_cast<std::size_t>(DirectUniform::FogColour)];
    c[2][3] = fog ? 1.0f : 0.0f;
    c[3][0] = fog_colour[0] / 255.0f;
    c[3][1] = fog_colour[1] / 255.0f;
    c[3][2] = fog_colour[2] / 255.0f;
    c[3][3] = fog ? 0.0f : 1.0f;
    // The alpha test: d is alpha less the reference, both whole numbers.
    switch (static_cast<int>(value(DirectUniform::AlphaTest))) {
        case 2:  // less
            c[4][0] = -1.0f;
            c[4][2] = -0.5f;
            break;
        case 3:  // less or equal
            c[4][0] = -1.0f;
            c[4][2] = 0.5f;
            break;
        case 4:  // equal
            c[4][1] = -1.0f;
            c[4][2] = 0.5f;
            break;
        case 5:  // greater or equal
            c[4][0] = 1.0f;
            c[4][2] = 0.5f;
            break;
        case 6:  // greater
            c[4][0] = 1.0f;
            c[4][2] = -0.5f;
            break;
        case 7:  // not equal
            c[4][1] = 1.0f;
            c[4][2] = -0.5f;
            break;
        default:  // off, or always
            c[4][2] = 1.0f;
            break;
    }
    // The second pass of the double draw keeps what the test fails.
    if (value(DirectUniform::AlphaTest) != 0.0f && value(DirectUniform::AlphaKeepFailing) != 0.0f) {
        c[4][0] = -c[4][0];
        c[4][1] = -c[4][1];
        c[4][2] = -c[4][2];
    }
    c[4][3] = value(DirectUniform::AlphaRef);
    // The blend's weight of the source.
    const float fix = value(DirectUniform::SourceFix);
    switch (static_cast<int>(value(DirectUniform::SourceScale))) {
        case 1:
            c[5][1] = kOpaque;
            break;
        case 2:
            c[5][0] = fix;
            break;
        case 3:
            c[5][2] = kOpaque;
            break;
        case 4:
            c[5][3] = std::min(fix, 1.0f);
            break;
        default:
            c[5][0] = 1.0f;
            break;
    }
    c[6][0] = 255.0f;
    c[6][1] = 0.5f;
    c[6][2] = kOpaque;
}

// The numbers of a copy: the texture as it is, alpha with it.
void copy_constants(float c[7][4]) {
    std::memset(c, 0, sizeof(float) * 7 * 4);
    c[1][1] = 1.0f;
    c[2][1] = 1.0f;
    c[3][3] = 1.0f;
    c[4][2] = 1.0f;
    c[5][0] = 1.0f;
    c[6][0] = 255.0f;
    c[6][1] = 0.5f;
    c[6][2] = 1.0f;
}

void load_direct(const float constants[7][4]) {
    if (g.loaded != ProgramKind::Direct) {
        rsxLoadVertexProgram(g.context, g_direct.vertex, g_direct.vertex_code);
        rsxLoadFragmentProgramLocation(
            g.context, g_direct.fragment, g_direct.fragment_code.offset, GCM_LOCATION_RSX
        );
        g.loaded = ProgramKind::Direct;
    }
    // Seven constants of four numbers. (librsx's rsxSetVertexProgramConstants counts single
    // numbers and converts each to an integer on its way into the command buffer.)
    rsxLoadVertexProgramParameterBlock(
        g.context, g_direct.vertex->const_start, 7, &constants[0][0]
    );
}

// ---- Vertices ----

// The RSX's type of a GL attribute, or 0 for one it has none for.
u8 data_type(const Attribute& attribute) {
    if (attribute.type == GL_FLOAT) {
        return GCM_VERTEX_DATA_TYPE_F32;
    }
    if (attribute.type == GL_UNSIGNED_BYTE && attribute.normalized) {
        return GCM_VERTEX_DATA_TYPE_U8;
    }
    return 0;
}

void bind_attribute(u32 index, u32 offset, u32 stride, u32 size, u8 type) {
    rsxBindVertexArrayAttrib(
        g.context,
        static_cast<u8>(index),
        0,
        offset,
        static_cast<u8>(stride),
        static_cast<u8>(size),
        type,
        GCM_LOCATION_RSX
    );
    g.attributes_bound |= 1u << index;
}

void unbind_attributes(u32 kept) {
    for (u32 i = 0; i < kAttributes; ++i) {
        if ((g.attributes_bound & ~kept & 1u << i) != 0) {
            rsxBindVertexArrayAttrib(
                g.context,
                static_cast<u8>(i),
                0,
                0,
                0,
                0,
                GCM_VERTEX_DATA_TYPE_F32,
                GCM_LOCATION_RSX
            );
        }
    }
    g.attributes_bound &= kept;
}

// The bound vertex array's attributes. False when one of them the RSX cannot read.
bool bind_vertex_array() {
    const auto found = g.vertex_arrays.find(g.vertex_array);
    if (found == g.vertex_arrays.end()) {
        return false;
    }
    u32 kept = 0;
    for (u32 i = 0; i < kAttributes; ++i) {
        const Attribute& attribute = found->second.attributes[i];
        if (!attribute.enabled) {
            continue;
        }
        const auto buffer = g.buffers.find(attribute.buffer);
        const u8 type = data_type(attribute);
        if (buffer == g.buffers.end() || buffer->second.memory.address == nullptr || type == 0) {
            return false;
        }
        bind_attribute(
            i,
            buffer->second.memory.offset + attribute.pointer,
            static_cast<u32>(attribute.stride),
            static_cast<u32>(attribute.size),
            type
        );
        buffer->second.drawn = g.frame;
        kept |= 1u << i;
    }
    unbind_attributes(kept);
    return true;
}

u32 primitive(GLenum mode) {
    switch (mode) {
        case GL_POINTS:
            return GCM_TYPE_POINTS;
        case GL_LINES:
            return GCM_TYPE_LINES;
        case GL_LINE_STRIP:
            return GCM_TYPE_LINE_STRIP;
        case GL_TRIANGLE_STRIP:
            return GCM_TYPE_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN:
            return GCM_TYPE_TRIANGLE_FAN;
        default:
            return GCM_TYPE_TRIANGLES;
    }
}

void mark_used(const Target& target) {
    const auto mark = [](GLuint name) {
        if (Image* drawn = image(name)) {
            drawn->used = g.sync;
        }
    };
    if (!target.screen) {
        const Framebuffer& framebuffer = g.framebuffers[g.draw_framebuffer];
        mark(framebuffer.colour);
        mark(framebuffer.depth);
    }
}

// A draw with a program the layer has for the RSX; one without is left out.
void draw_arrays(GLenum mode, GLint first, GLsizei count) {
    const auto program = g.programs.find(g.program);
    if (program == g.programs.end() || program->second.kind != ProgramKind::Direct || count <= 0) {
        ++g.draws_left_out;
        return;
    }
    Target target;
    if (!apply_target(target) || !bind_vertex_array()) {
        ++g.draws_left_out;
        return;
    }
    ++g.draws;
    apply_state(target);
    float constants[7][4];
    direct_constants(program->second, constants);
    load_direct(constants);
    load_bound_texture(0);
    rsxDrawVertexArray(
        g.context, primitive(mode), static_cast<u32>(first), static_cast<u32>(count)
    );
    mark_used(target);
}

// ---- Copies ----

// A rectangle of a texture drawn over a rectangle of the draw framebuffer, both in GL's
// coordinates, with nothing of GL's state but the scissor. GL's state is told to the RSX again
// at the next draw.
void draw_copy(Image& source, const GLint from[4], const GLint to[4], bool linear) {
    Target target;
    if (!apply_target(target) || (!target.screen && target.colour == nullptr)) {
        return;
    }
    // The copy's own vertices, given back with the frame.
    Memory memory = allocate(kCopyVertices * sizeof(CopyVertex));
    if (memory.address == nullptr) {
        return;
    }
    auto* vertices = static_cast<CopyVertex*>(memory.address);
    const u32 offset = memory.offset;
    retire(memory);
    const auto wide = static_cast<float>(target.width);
    const auto high = static_cast<float>(target.height);
    const float x[2] =
        {static_cast<float>(to[0]) / wide * 2.0f - 1.0f,
         static_cast<float>(to[2]) / wide * 2.0f - 1.0f};
    const float y[2] =
        {static_cast<float>(to[1]) / high * 2.0f - 1.0f,
         static_cast<float>(to[3]) / high * 2.0f - 1.0f};
    const float s[2] = {
        static_cast<float>(from[0]) / static_cast<float>(source.width),
        static_cast<float>(from[2]) / static_cast<float>(source.width),
    };
    const float t[2] = {
        static_cast<float>(from[1]) / static_cast<float>(source.height),
        static_cast<float>(from[3]) / static_cast<float>(source.height),
    };
    constexpr int corners[kCopyVertices][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    for (u32 i = 0; i < kCopyVertices; ++i) {
        const int* corner = corners[i];
        vertices[i] = CopyVertex{
            x[corner[0]],
            y[corner[1]],
            0.0f,
            s[corner[0]],
            t[corner[1]],
            1.0f,
            {255, 255, 255, 255},
            1.0f,
        };
    }

    const GLint whole[4] =
        {0, 0, static_cast<GLint>(target.width), static_cast<GLint>(target.height)};
    const GLint kept[4] = {g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3]};
    std::copy(std::begin(whole), std::end(whole), g.viewport);
    apply_viewport(target);
    std::copy(std::begin(kept), std::end(kept), g.viewport);
    apply_scissor(target);
    rsxSetDepthTestEnable(g.context, GCM_FALSE);
    rsxSetDepthWriteEnable(g.context, GCM_FALSE);
    rsxSetColorMask(
        g.context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A
    );
    rsxSetBlendEnable(g.context, GCM_FALSE);
    rsxSetCullFaceEnable(g.context, GCM_FALSE);
    g.state_stale = true;

    bind_attribute(
        0, offset + offsetof(CopyVertex, x), sizeof(CopyVertex), 3, GCM_VERTEX_DATA_TYPE_F32
    );
    bind_attribute(
        1, offset + offsetof(CopyVertex, s), sizeof(CopyVertex), 3, GCM_VERTEX_DATA_TYPE_F32
    );
    bind_attribute(
        2, offset + offsetof(CopyVertex, rgba), sizeof(CopyVertex), 4, GCM_VERTEX_DATA_TYPE_U8
    );
    bind_attribute(
        3, offset + offsetof(CopyVertex, fog), sizeof(CopyVertex), 1, GCM_VERTEX_DATA_TYPE_F32
    );
    unbind_attributes(0xFu);
    float constants[7][4];
    copy_constants(constants);
    load_direct(constants);
    Sampler sampler;
    sampler.min_filter = sampler.mag_filter = linear ? GL_LINEAR : GL_NEAREST;
    sampler.wrap_s = sampler.wrap_t = GL_CLAMP_TO_EDGE;
    load_texture(0, source, &sampler);
    ++g.copies;
    rsxDrawVertexArray(g.context, GCM_TYPE_TRIANGLES, 0, kCopyVertices);
    mark_used(target);
}

// The colour of the read framebuffer over the draw framebuffer. Depth is not copied yet, and
// nothing is read from the screen.
void blit_framebuffer(
    GLint from_x0,
    GLint from_y0,
    GLint from_x1,
    GLint from_y1,
    GLint to_x0,
    GLint to_y0,
    GLint to_x1,
    GLint to_y1,
    GLbitfield mask,
    GLenum how
) {
    Target source;
    if ((mask & GL_COLOR_BUFFER_BIT) == 0 || !target_of(g.read_framebuffer, source)
        || source.colour == nullptr || g.read_framebuffer == g.draw_framebuffer) {
        return;
    }
    const GLint from[4] = {from_x0, from_y0, from_x1, from_y1};
    const GLint to[4] = {to_x0, to_y0, to_x1, to_y1};
    draw_copy(g.images[g.framebuffers[g.read_framebuffer].colour], from, to, how == GL_LINEAR);
}

// A rectangle of the read framebuffer's colour into the bound texture, as it is.
void copy_tex_sub_image(
    GLenum target, GLint level, GLint to_x, GLint to_y, GLint x, GLint y, GLsizei wide, GLsizei high
) {
    Target source;
    Image* into =
        target == GL_TEXTURE_2D && g.unit < kTextureUnits ? image(g.texture[g.unit]) : nullptr;
    if (into == nullptr || level != 0 || !target_of(g.read_framebuffer, source)
        || source.colour == nullptr || x < 0 || y < 0 || to_x < 0 || to_y < 0 || wide <= 0
        || high <= 0 || static_cast<u32>(x + wide) > source.width
        || static_cast<u32>(y + high) > source.height || static_cast<u32>(to_x + wide) > into->width
        || static_cast<u32>(to_y + high) > into->height) {
        return;
    }
    rsxSetTransferImage(
        g.context,
        GCM_TRANSFER_LOCAL_TO_LOCAL,
        into->memory.offset,
        into->pitch,
        static_cast<u32>(to_x),
        static_cast<u32>(to_y),
        source.colour->memory.offset,
        source.colour->pitch,
        static_cast<u32>(x),
        static_cast<u32>(y),
        static_cast<u32>(wide),
        static_cast<u32>(high),
        4
    );
    into->used = g.sync;
}

// ---- Reading back ----

// The read framebuffer's pixels as GL_RGBA bytes or floats, the bottom row first (the port's
// screenshots, OPENRAC_SHOT). The colour is the program's own memory, read once the RSX has drawn
// everything sent; under RPCS3 that needs "Write Color Buffers", and the RSX taken off the surface
// first (park_target).
void read_pixels(
    GLint x, GLint y, GLsizei wide, GLsizei high, GLenum format, GLenum type, void* pixels
) {
    Target source;
    if (format != GL_RGBA || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)
        || !target_of(g.read_framebuffer, source) || (!source.screen && source.colour == nullptr)) {
        return;
    }
    park_target();
    wait_idle();
    const auto* picture = static_cast<const u32*>(
        source.screen ? g.colour[g.drawn].address : source.colour->memory.address
    );
    const u32 pitch = source.screen ? g.colour_pitch : source.colour->pitch;
    const auto source_wide = static_cast<GLint>(source.width);
    const auto source_high = static_cast<GLint>(source.height);
    auto* bytes = static_cast<unsigned char*>(pixels);
    auto* floats = static_cast<GLfloat*>(pixels);
    for (GLsizei row = 0; row < high; ++row) {
        const GLint line = source.screen ? source_high - 1 - (y + row) : y + row;
        for (GLsizei column = 0; column < wide; ++column) {
            const GLint across = x + column;
            u32 pixel = 0;  // A8R8G8B8, read as the console's own word
            if (line >= 0 && line < source_high && across >= 0 && across < source_wide) {
                pixel = picture[static_cast<u32>(line) * (pitch / 4) + static_cast<u32>(across)];
            }
            const unsigned char rgba[4] = {
                static_cast<unsigned char>(pixel >> 16),
                static_cast<unsigned char>(pixel >> 8),
                static_cast<unsigned char>(pixel),
                source.screen ? static_cast<unsigned char>(0xFF)
                              : static_cast<unsigned char>(pixel >> 24),
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

void finish() {
    wait_idle();
}

}  // namespace

bool target_of(GLuint framebuffer, Target& target) {
    target = Target{};
    if (framebuffer == 0) {
        target.screen = true;
        target.width = g.width;
        target.height = g.height;
        return true;
    }
    const auto found = g.framebuffers.find(framebuffer);
    if (found == g.framebuffers.end()) {
        return false;
    }
    target.colour = image(found->second.colour);
    target.depth = image(found->second.depth);
    const Image* sized = target.colour != nullptr ? target.colour : target.depth;
    if (sized == nullptr) {
        return false;
    }
    target.width = sized->width;
    target.height = sized->height;
    return true;
}

bool prepare_programs(std::string& error) {
    g_white.width = g_white.height = 1;
    g_white.pitch = 4;
    g_white.memory = allocate(4);
    if (g_white.memory.address == nullptr
        || !load(g_direct, programs::direct_vpo, programs::direct_fpo)) {
        error = "no RSX memory for the layer's programs";
        return false;
    }
    std::memset(g_white.memory.address, 0xFF, 4);
    return true;
}

const Entry kDrawFunctions[] = {
    OPENRAC_RSXGL_ENTRY(glEnable, gl_enable),
    OPENRAC_RSXGL_ENTRY(glDisable, gl_disable),
    OPENRAC_RSXGL_ENTRY(glViewport, gl_viewport),
    OPENRAC_RSXGL_ENTRY(glScissor, gl_scissor),
    OPENRAC_RSXGL_ENTRY(glDepthFunc, depth_function),
    OPENRAC_RSXGL_ENTRY(glDepthMask, depth_mask),
    OPENRAC_RSXGL_ENTRY(glColorMask, colour_write),
    OPENRAC_RSXGL_ENTRY(glBlendFuncSeparate, blend_function),
    OPENRAC_RSXGL_ENTRY(glBlendEquation, blend_equation),
    OPENRAC_RSXGL_ENTRY(glBlendColor, blend_colour),
    OPENRAC_RSXGL_ENTRY(glCullFace, cull_face),
    OPENRAC_RSXGL_ENTRY(glFrontFace, front_face),
    OPENRAC_RSXGL_ENTRY(glClearColor, clear_colour),
    OPENRAC_RSXGL_ENTRY(glClearDepth, clear_depth),
    OPENRAC_RSXGL_ENTRY(glClear, clear),
    OPENRAC_RSXGL_ENTRY(glDrawArrays, draw_arrays),
    OPENRAC_RSXGL_ENTRY(glBlitFramebuffer, blit_framebuffer),
    OPENRAC_RSXGL_ENTRY(glCopyTexSubImage2D, copy_tex_sub_image),
    OPENRAC_RSXGL_ENTRY(glReadPixels, read_pixels),
    OPENRAC_RSXGL_ENTRY(glFinish, finish),
};
const std::size_t kDrawFunctionCount = std::size(kDrawFunctions);

}  // namespace openrac::rsxgl
