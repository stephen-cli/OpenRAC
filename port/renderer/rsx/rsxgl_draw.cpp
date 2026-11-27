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
#include <map>

#include "common/log.h"
#include "renderer/rsx/rsxgl_internal.h"
#include "rsx_programs.h"

namespace openrac::rsxgl {

using namespace openrac::gl;

namespace {

// ---- The programs ----

// A vertex program.
struct Loaded {
    const rsxVertexProgram* vertex = nullptr;
    const void* vertex_code = nullptr;
};

// A fragment program.
struct Fragment {
    const rsxFragmentProgram* program = nullptr;
    Memory code;  // in the RSX's memory, where the RSX reads a fragment program from
};

// The parts of mesh.fp and of direct.fp that a draw can do without (the programs say what
// each is). A set of parts is the sum of their numbers, and each program is assembled once
// for every set (cmake/Window.cmake names the parts in this order): the RSX fills a pixel
// the faster the shorter its program (README.md, "Fragment programs by their parts").
namespace mesh_part {
constexpr u32 kMaterial = 1;
constexpr u32 kCutout = 2;
constexpr u32 kLeftOut = 4;
constexpr u32 kFog = 8;
constexpr u32 kAdded = 16;
constexpr u32 kSets = 32;
}  // namespace mesh_part

namespace direct_part {
constexpr u32 kFog = 1;
constexpr u32 kAlphaTest = 2;
constexpr u32 kWeight = 4;
constexpr u32 kSets = 8;
}  // namespace direct_part

const unsigned char* const kMeshFragments[mesh_part::kSets] = {
    programs::mesh_00_fpo, programs::mesh_01_fpo, programs::mesh_02_fpo, programs::mesh_03_fpo,
    programs::mesh_04_fpo, programs::mesh_05_fpo, programs::mesh_06_fpo, programs::mesh_07_fpo,
    programs::mesh_08_fpo, programs::mesh_09_fpo, programs::mesh_10_fpo, programs::mesh_11_fpo,
    programs::mesh_12_fpo, programs::mesh_13_fpo, programs::mesh_14_fpo, programs::mesh_15_fpo,
    programs::mesh_16_fpo, programs::mesh_17_fpo, programs::mesh_18_fpo, programs::mesh_19_fpo,
    programs::mesh_20_fpo, programs::mesh_21_fpo, programs::mesh_22_fpo, programs::mesh_23_fpo,
    programs::mesh_24_fpo, programs::mesh_25_fpo, programs::mesh_26_fpo, programs::mesh_27_fpo,
    programs::mesh_28_fpo, programs::mesh_29_fpo, programs::mesh_30_fpo, programs::mesh_31_fpo,
};

const unsigned char* const kDirectFragments[direct_part::kSets] = {
    programs::direct_00_fpo,
    programs::direct_01_fpo,
    programs::direct_02_fpo,
    programs::direct_03_fpo,
    programs::direct_04_fpo,
    programs::direct_05_fpo,
    programs::direct_06_fpo,
    programs::direct_07_fpo,
};

Loaded g_direct;
Loaded g_sky;
Loaded g_mesh;
Loaded g_moby;
Loaded g_effect;
Fragment g_sky_fragment;
Fragment g_copy_fragment;
Fragment g_mesh_fragments[mesh_part::kSets];      // mesh.vp's draws and moby.vp's
Fragment g_direct_fragments[direct_part::kSets];  // direct.vp's draws and effect.vp's
const Fragment* g_fragment_set = nullptr;         // the RSX's fragment program now
Image g_white;  // one white texel: what a unit samples when the draw binds nothing

// The layer's own vertices for a copy, in the direct program's layout.
struct CopyVertex {
    float x, y, z;
    float s, t, q;
    unsigned char rgba[4];
    float fog;
};

constexpr u32 kCopyVertices = 6;

void load(Loaded& into, const unsigned char* vertex) {
    into.vertex = reinterpret_cast<const rsxVertexProgram*>(vertex);
    void* code = nullptr;
    u32 size = 0;
    rsxVertexProgramGetUCode(into.vertex, &code, &size);
    into.vertex_code = code;
}

bool load(Fragment& into, const unsigned char* fragment) {
    into.program = reinterpret_cast<const rsxFragmentProgram*>(fragment);
    void* code = nullptr;
    u32 size = 0;
    rsxFragmentProgramGetUCode(into.program, &code, &size);
    into.code = allocate(size);
    if (into.code.address == nullptr) {
        return false;
    }
    write_memory(into.code.address, code, size);
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
    // Its buffers' tiled regions (rsxgl_internal.h, allocate_surface): the screen's are laid
    // out for one, an image where it was made to be drawn into.
    if (surface.colorTarget != GCM_SURFACE_TARGET_NONE
        && (target.screen || target.colour->surface)) {
        bind_surface(surface.colorOffset[0], surface.colorPitch[0], target.height);
    }
    if (target.depth == nullptr || target.depth->surface) {
        bind_surface(
            surface.depthOffset,
            surface.depthPitch,
            target.depth != nullptr ? target.depth->height : g.height
        );
    }
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

// GL's filters between levels, GL_NEAREST_MIPMAP_NEAREST to GL_LINEAR_MIPMAP_LINEAR: the RSX has
// its own in the same order.
constexpr GLenum kFirstMipFilter = 0x2700;
constexpr GLenum kLastMipFilter = 0x2703;

// A filter for a texture of that many levels. With one level a filter between levels is the
// filter within the level (the renderer's are linear).
u8 filter(GLenum gl, u32 levels) {
    if (levels > 1 && gl >= kFirstMipFilter && gl <= kLastMipFilter) {
        return static_cast<u8>(GCM_TEXTURE_NEAREST_MIPMAP_NEAREST + (gl - kFirstMipFilter));
    }
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
    loaded.mipmap = static_cast<u8>(texture.levels);
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
        filter(sampler != nullptr ? sampler->min_filter : texture.min_filter, texture.levels),
        filter(sampler != nullptr ? sampler->mag_filter : texture.mag_filter, 1),
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

// The RSX's programs are told to it when they change: a draw's fragment program is the one
// with the parts the draw needs, whatever its vertex program.
void use_vertex(ProgramKind kind, const Loaded& program) {
    if (g.loaded != kind) {
        rsxLoadVertexProgram(g.context, program.vertex, program.vertex_code);
        g.loaded = kind;
    }
}

void use_fragment(const Fragment& fragment) {
    if (g_fragment_set != &fragment) {
        rsxLoadFragmentProgramLocation(
            g.context, fragment.program, fragment.code.offset, GCM_LOCATION_RSX
        );
        g_fragment_set = &fragment;
    }
}

// Makes `program` and `fragment` the RSX's, with `count` constants of four numbers from its
// first. (librsx's
// rsxSetVertexProgramConstants counts single numbers and converts each to an integer on its way
// into the command buffer.)
void load_program(
    ProgramKind kind,
    const Loaded& program,
    const Fragment& fragment,
    const float* constants,
    u32 count
) {
    use_vertex(kind, program);
    use_fragment(fragment);
    rsxLoadVertexProgramParameterBlock(g.context, program.vertex->const_start, count, constants);
}

// The parts of direct.fp that a draw needs, from the numbers of its texcoord[1] to [6]
// (direct_constants' second row on).
u32 direct_parts(const float of[6][4]) {
    const float* fog_weight = of[1];
    const float* fog = of[2];
    const float* test = of[3];
    const float* weight = of[4];
    u32 parts = 0;
    if (fog_weight[3] != 0.0f || fog[3] != 1.0f) {
        parts |= direct_part::kFog;
    }
    if (test[0] != 0.0f || test[1] != 0.0f || test[2] < 0.0f) {
        parts |= direct_part::kAlphaTest;
    }
    if (weight[0] != 1.0f || weight[1] != 0.0f || weight[2] != 0.0f || weight[3] != 0.0f) {
        parts |= direct_part::kWeight;
    }
    return parts;
}

void load_direct(const float constants[7][4]) {
    load_program(
        ProgramKind::Direct,
        g_direct,
        g_direct_fragments[direct_parts(constants + 1)],
        &constants[0][0],
        7
    );
}

// ---- The effect program's numbers ----

constexpr u32 kEffectConstants = 13;

// effect.vp's constants (programs/effect.vp says what each is) from effect.vert's uniforms and
// direct.frag's.
void effect_constants(const Program& program, float c[kEffectConstants][4]) {
    std::memset(c, 0, sizeof(float) * kEffectConstants * 4);
    const auto* projection = program.uniforms.data() + effect::kViewProjection;
    const auto* view = program.uniforms.data() + effect::kView;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            c[row][column] = projection[column][row];
        }
        // The view looks down -z; the game's raw units are 1024 to one.
        c[4][column] = -1024.0f * view[column][2];
    }
    const auto& fog_line = program.uniforms[effect::kFogLine];
    c[5][0] = fog_line[0];
    c[5][1] = fog_line[1];
    c[5][2] = std::min(fog_line[2], fog_line[3]);
    c[5][3] = std::max(fog_line[2], fog_line[3]);
    c[6][0] = 1.0f / 255.0f;
    c[6][3] = 1.0f;
    float direct[7][4];
    direct_constants(program, direct);
    std::memcpy(c[7], direct[1], sizeof(float) * 6 * 4);
}

// ---- The sky program's numbers ----

// sky.vp's five constants from sky.vert's uniforms: the rows of view_projection * shell (a
// uniform matrix is four slots, its columns), and the tint.
void sky_constants(const Program& program, float c[5][4]) {
    const auto* projection = program.uniforms.data() + sky::kViewProjection;
    const auto* shell = program.uniforms.data() + sky::kShell;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += projection[k][row] * shell[column][k];
            }
            c[row][column] = sum;
        }
    }
    const auto& tint = program.uniforms[sky::kTint];
    c[4][0] = tint[0];
    c[4][1] = tint[1];
    c[4][2] = tint[2];
    c[4][3] = 1.0f;
}

// ---- The mesh program's numbers ----

constexpr u32 kMeshInstanceConstants = 9;   // mesh.vp's c[0] to c[8]
constexpr u32 kMeshDrawConstants = 7;       // c[9] to c[15]
constexpr u32 kMeshPassFirstConstant = 25;  // c[25] to c[29] are the pass's (programs/moby.vp)
constexpr u32 kMeshPassConstants = 5;

// The pass of the mobys a draw of the mesh program is of (scene/moby_passes.h), as mesh.frag
// tells them apart; Solid is the static layers' passes too.
enum class MobyPass {
    Solid,
    Glow,
    Effects,
    Metal,
};

MobyPass moby_pass(const Program& program) {
    if (program.uniforms[mesh::kMetalPass][0] != 0.0f) {
        return MobyPass::Metal;
    }
    if (program.uniforms[mesh::kEffectPass][0] != 0.0f) {
        return MobyPass::Effects;
    }
    return program.uniforms[mesh::kGlowPass][0] != 0.0f ? MobyPass::Glow : MobyPass::Solid;
}

// mesh.vp's constants of a draw from mesh.vert's and mesh.frag's uniforms (programs/mesh.fp says
// what the fragment program's are).
void mesh_constants(const Program& program, float c[kMeshDrawConstants][4]) {
    const auto value = [&](u32 slot) {
        return program.uniforms[slot][0];
    };
    const MobyPass pass = moby_pass(program);
    std::memset(c, 0, sizeof(float) * kMeshDrawConstants * 4);
    std::memcpy(c[0], program.uniforms[mesh::kFogParams].data(), sizeof(float) * 4);
    // The viewer's sun (mesh.frag's kSun), on what the level did not light. The blended mobys
    // and the metal packets get none.
    if (value(mesh::kLighting) != 0.0f && pass != MobyPass::Effects && pass != MobyPass::Metal) {
        c[1][0] = 0.45f * 0.36f;
        c[1][1] = 0.45f * 0.48f;
        c[1][2] = 0.45f * 0.80f;
        c[1][3] = 0.55f;
    } else {
        c[1][3] = 1.0f;
    }
    c[2][0] = 1.0f / 255.0f;
    c[2][2] = 1.0e-20f;
    c[2][3] = 1.0f;
    // The metal packets' texel is the chrome or glass picture's, not the material's.
    if (pass == MobyPass::Metal) {
        c[3][0] = c[3][1] = c[3][2] = c[3][3] = 1.0f;
    } else {
        std::memcpy(c[3], program.uniforms[mesh::kMaterialColour].data(), sizeof(float) * 4);
    }
    const auto& fog_colour = program.uniforms[mesh::kFogColour];
    const bool fog = fog_colour[3] > 0.5f;
    c[4][0] = fog_colour[0];
    c[4][1] = fog_colour[1];
    c[4][2] = fog_colour[2];
    c[4][3] = fog ? 1.0f : 0.0f;
    // The cutout: a solid pass keeps a cutout material's opaque texels (alpha from 0.99), the
    // translucent pass its part-transparent ones (0.02 to 0.99) and nothing of another material.
    const bool cutout = value(mesh::kCutout) != 0.0f;
    const bool translucent = value(mesh::kTranslucentPass) != 0.0f;
    c[5][1] = 1.0f;
    c[5][3] = 1.0f;
    c[6][0] = fog ? 0.0f : 1.0f;
    // The glow faces and the blended mobys: every texel at its alpha, or in two draws split at
    // an alpha reference (glow_split: 1 the texels at or above it, 2 the rest). The metal
    // packets: every texel.
    if (pass != MobyPass::Solid) {
        const float reference =
            pass == MobyPass::Effects ? value(mesh::kEffectReference) : 96.0f / 128.0f;
        const float split = pass == MobyPass::Metal ? 0.0f : value(mesh::kGlowSplit);
        if (split == 1.0f) {
            c[5][0] = 1.0f;
            c[5][1] = -reference;
        } else if (split != 0.0f) {
            c[5][0] = -1.0f;
            c[5][1] = reference - 1.0e-5f;
        }
        // An added moby's alpha is in its colour (pass_constants).
        const bool added = pass == MobyPass::Effects && value(mesh::kEffectAdditive) != 0.0f;
        c[6][1] = added ? 0.0f : 1.0f;
        return;
    }
    if (translucent && cutout) {
        c[5][0] = 1.0f;
        c[5][1] = -0.02f;
        c[5][2] = -1.0f;
        c[5][3] = 0.98999f;
    } else if (translucent) {
        c[5][1] = -1.0f;
    } else if (cutout) {
        c[5][0] = 1.0f;
        c[5][1] = -0.99f;
    }
    c[6][1] = translucent ? 1.0f : 0.0f;
    c[6][2] = translucent ? 0.0f : 1.0f;
}

// The parts of mesh.fp that a draw needs, from mesh_constants' numbers and the pass's
// (texcoord[3] to [6] are `draw`'s fourth row on, texcoord[7] the pass's first).
u32 mesh_parts(
    ProgramKind kind,
    const float draw[kMeshDrawConstants][4],
    const float pass[kMeshPassConstants][4]
) {
    const float* material = draw[3];
    const float* fog = draw[4];
    const float* cutout = draw[5];
    const float* rest = draw[6];
    u32 parts = kind == ProgramKind::Moby ? mesh_part::kLeftOut : 0;
    if (material[0] != 1.0f || material[1] != 1.0f || material[2] != 1.0f || material[3] != 1.0f) {
        parts |= mesh_part::kMaterial;
    }
    if (cutout[0] != 0.0f || cutout[1] != 1.0f || cutout[2] != 0.0f || cutout[3] != 1.0f) {
        parts |= mesh_part::kCutout;
    }
    if (fog[3] != 0.0f || rest[0] != 1.0f) {
        parts |= mesh_part::kFog;
    }
    if (pass[0][0] != 0.0f || pass[0][1] != 1.0f) {
        parts |= mesh_part::kAdded;
    }
    return parts;
}

// The constants of a draw's pass, c[25] to c[29] (programs/moby.vp says what each is; mesh.vp
// reads the first).
void pass_constants(const Program& program, float c[kMeshPassConstants][4]) {
    const MobyPass pass = moby_pass(program);
    std::memset(c, 0, sizeof(float) * kMeshPassConstants * 4);
    const bool added =
        pass == MobyPass::Effects && program.uniforms[mesh::kEffectAdditive][0] != 0.0f;
    c[0][0] = added ? 1.0f : 0.0f;
    c[0][1] = added ? 0.0f : 1.0f;
    c[0][2] = kOpaque;
    // The glow packets' faces are the glow pass's and no other's; the blended mobys and the
    // metal packets are drawn whole.
    c[1][0] = pass == MobyPass::Solid ? -2.0f : pass == MobyPass::Glow ? 2.0f : 0.0f;
    c[1][1] = pass == MobyPass::Glow ? -1.0f : 1.0f;
    if (pass != MobyPass::Metal) {
        return;
    }
    // MobyProc's pull toward the camera (mesh.vert).
    c[1][2] = program.uniforms[mesh::kClipZeroToOne][0] != 0.0f ? 0.5e-4f : 1.0e-4f;
    // st = (e.xy + 1) / 2, e the normal in camera space, t growing downward.
    const auto* view = program.uniforms.data() + mesh::kView;
    for (std::size_t column = 0; column < 3; ++column) {
        c[2][column] = 0.5f * view[column][0];
        c[3][column] = -0.5f * view[column][1];
    }
    c[2][3] = c[3][3] = 0.5f;
    c[4][0] = c[4][1] = c[4][2] = 112.0f / 128.0f;
    c[4][3] = 1.0f;
}

// Where the bound vertex array has the value of an instance for `attribute`: its first, and the
// bytes from one to the next. Null when the attribute is not one value an instance. They are
// read in the program's memory, where the buffer's bytes are kept from its first such draw.
const float* instance_values(u32 attribute, u32& stride) {
    const auto array = g.vertex_arrays.find(g.vertex_array);
    if (array == g.vertex_arrays.end()) {
        return nullptr;
    }
    const Attribute& instanced = array->second.attributes[attribute];
    const auto buffer = g.buffers.find(instanced.buffer);
    if (!instanced.enabled || instanced.divisor == 0 || instanced.type != GL_FLOAT
        || buffer == g.buffers.end() || buffer->second.memory.address == nullptr) {
        return nullptr;
    }
    Buffer& values = buffer->second;
    if (values.kept.size() != values.size) {
        values.kept.resize(values.size);
        read_memory(values.kept.data(), values.memory.address, values.size);
    }
    stride = static_cast<u32>(instanced.stride);
    return reinterpret_cast<const float*>(values.kept.data() + instanced.pointer);
}

// The value of `instance` read into `into`, which keeps what it has where the vertex array has
// no such value (`values` null).
template <std::size_t Count>
void stepped(const float* values, u32 stride, GLsizei instance, float (&into)[Count]) {
    if (values != nullptr) {
        std::memcpy(
            into,
            reinterpret_cast<const unsigned char*>(values) + stride * static_cast<u32>(instance),
            sizeof into
        );
    }
}

// mesh.vp's constants of an instance: what mesh.vert works out at every vertex from the
// instance's matrix (columns, as GL has a matrix) and tint. `lit`: its vertices take its colours
// of the light texture (load_lights).
void mesh_instance_constants(
    const Program& program,
    const float* model,
    const float* tint,
    bool lit,
    float c[kMeshInstanceConstants][4]
) {
    const auto* projection = program.uniforms.data() + mesh::kViewProjection;
    const auto* view = program.uniforms.data() + mesh::kView;
    for (std::size_t column = 0; column < 4; ++column) {
        float depth = 0.0f;
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += projection[k][row] * model[column * 4 + k];
            }
            c[row][column] = sum;
            depth += view[row][2] * model[column * 4 + row];
        }
        // The view looks down -z; the game's raw units are 1024 to one.
        c[4][column] = -1024.0f * depth;
    }
    std::memcpy(c[5], tint, sizeof(float) * 4);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            c[6 + row][column] = model[column * 4 + row];
        }
        c[6 + row][3] = 0.0f;
    }
    c[8][3] = lit ? 1.0f : 0.0f;
}

// ---- The moby program's numbers ----

constexpr u32 kMobyFirstConstant = 16;  // moby.vp's c[16] to c[24] are an instance's
constexpr u32 kMobyInstanceConstants = 9;
constexpr u32 kJointFirstConstant = 32;  // then the joints' matrices, three rows a joint
constexpr u32 kJointsHeld = (468 - kJointFirstConstant) / 3;  // the RSX has 468 constants

// The RSX program a draw with `program` bound takes: the mesh program's draws with `skinning`
// set, or of a pass only the mobys have, are the live mobys'.
ProgramKind drawn_kind(const Program& program) {
    return program.kind == ProgramKind::Mesh
                   && (program.uniforms[mesh::kSkinning][0] != 0.0f
                       || moby_pass(program) != MobyPass::Solid)
               ? ProgramKind::Moby
               : program.kind;
}

// The metal pass's alpha of an instance (mesh.vert): MobyProc's gate, full up to 16 game units
// nearer than the moby's shine distance (`lights` = -1 - distance; 0x18 unless the class changes
// it), none past it.
float moby_shine(const Program& program, const float* model, float lights) {
    const auto* view = program.uniforms.data() + mesh::kView;
    const float reach = lights < -1.5f ? -lights - 1.0f : 24.0f;
    float centre = -view[3][2];
    for (std::size_t k = 0; k < 3; ++k) {
        centre -= view[k][2] * model[12 + k];
    }
    return std::clamp((reach - centre) / 16.0f, 0.0f, 1.0f);
}

// moby.vp's constants of an instance (programs/moby.vp says what each is): what mesh.vert works
// out at every vertex from the instance's moby_light and moby_ambient and the uniform arrays they
// index, which is the same for all of an instance's vertices.
void moby_instance_constants(
    const Program& program,
    const float* light,
    const float* ambient,
    bool skinned,
    float shine,
    float c[kMobyInstanceConstants][4]
) {
    std::memset(c, 0, sizeof(float) * kMobyInstanceConstants * 4);
    c[0][0] = c[0][1] = c[0][2] = 1.0f;
    c[0][3] = skinned ? 1.0f : 0.0f;
    c[4][3] = kOpaque;
    c[5][3] = shine;
    c[8][0] = 765.0f;
    c[8][1] = 0.5f;
    c[8][2] = -1.5f;
    // Not lit by MobyProc: the vertex's own colour, and the viewer's sun where it is asked for.
    if (std::fabs(light[3]) <= 0.5f) {
        return;
    }
    using Four = std::array<GLfloat, 4>;
    const Four* sets = program.uniforms.data() + mesh::kLightSets;
    const int first = static_cast<int>(light[0] + 0.5f);
    const auto set0 = static_cast<std::size_t>(first & 15) * 4;
    const auto set1 = static_cast<std::size_t>(static_cast<int>(light[1] + 0.5f) & 15) * 4;
    Four colour_a = sets[set0];
    Four direction_a = sets[set0 + 1];
    Four colour_b = sets[set0 + 2];
    Four direction_b = sets[set0 + 3];
    // Two sets cross-faded.
    const float fade = light[2];
    if (fade > 0.0f) {
        const auto mixed = [fade](Four& from, const Four& to) {
            for (std::size_t i = 0; i < 4; ++i) {
                from[i] += (to[i] - from[i]) * fade;
            }
        };
        const auto unit = [](Four& v) {
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (std::size_t i = 0; i < 3 && length > 0.0f; ++i) {
                v[i] /= length;
            }
        };
        mixed(colour_a, sets[set1]);
        mixed(direction_a, sets[set1 + 1]);
        mixed(colour_b, sets[set1 + 2]);
        mixed(direction_b, sets[set1 + 3]);
        unit(direction_a);
        unit(direction_b);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        c[0][i] = ambient[i];
        c[1][i] = colour_a[i];
        c[2][i] = -direction_a[i];
        c[3][i] = colour_b[i];
        c[4][i] = -direction_b[i];
    }
    c[1][3] = -std::fabs(colour_a[3]);
    c[2][3] = 1.0f;
    c[3][3] = -std::fabs(colour_b[3]);
    // A point-lit moby's third light: the record its first number names above the set's.
    const int point = (first >> 8) - 1;
    if (point >= 0 && point < 32) {
        const Four* points = program.uniforms.data() + mesh::kMobyPointLights;
        for (std::size_t i = 0; i < 3; ++i) {
            c[5][i] = points[2 * point][i];
            c[6][i] = -points[2 * point + 1][i];
        }
    }
    // The glow colour of a moby whose mode has 0x10: 1 + its RGB as a whole number.
    if (ambient[3] > 0.5f) {
        const auto glow = static_cast<u32>(std::lround(ambient[3])) - 1;
        c[7][0] = static_cast<float>(glow & 255) / 128.0f;
        c[7][1] = static_cast<float>(glow >> 8 & 255) / 128.0f;
        c[7][2] = static_cast<float>(glow >> 16 & 255) / 128.0f;
        c[7][3] = 1.0f;
        c[8][3] = 1.0f;
    }
}

// A lit tie's or shrub's colours are mesh.vp's c[32] to c[95], where moby.vp has its joints: the
// one's load is the end of the other's.
constexpr u32 kLightFirstConstant = kJointFirstConstant;
constexpr u32 kLightsHeld = 64;  // a tie's light slots; a shrub has 24

// The colours the RSX's constants hold now: an instance is several draws (a material each).
struct LightsLoaded {
    const Image* texture = nullptr;
    u32 frame = 0;
    u32 written = 0;
    u32 first = 0;
    u32 slots = 0;
};

LightsLoaded g_lights_loaded;

// The joints the RSX's constants hold now: a moby is several draws of a frame (a material each),
// and the constants stay through them and through the other programs' draws.
struct JointsLoaded {
    const Image* palette = nullptr;
    u32 frame = 0;
    u32 written = 0;
    u32 first = 0;
    u32 joints = 0;
};

JointsLoaded g_joints_loaded;

// Hands the RSX `joints` matrices of the palette texture from `first` (four columns a matrix)
// as moby.vp's rows. False when the palette has not so many.
bool load_joints(const Image& palette, u32 first, u32 joints) {
    if (joints == 0 || joints > kJointsHeld
        || (static_cast<std::size_t>(first) + joints) * 16 > palette.floats.size()) {
        return false;
    }
    if (g_joints_loaded.palette == &palette && g_joints_loaded.frame == g.frame
        && g_joints_loaded.written == palette.written && g_joints_loaded.first == first
        && g_joints_loaded.joints >= joints) {
        return true;
    }
    g_joints_loaded = {&palette, g.frame, palette.written, first, joints};
    g_lights_loaded = {};
    float rows[kJointsHeld * 3][4];
    const float* matrix = palette.floats.data() + static_cast<std::size_t>(first) * 16;
    for (u32 joint = 0; joint < joints; ++joint, matrix += 16) {
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                rows[joint * 3 + row][column] = matrix[column * 4 + row];
            }
        }
    }
    rsxLoadVertexProgramParameterBlock(
        g.context, g_moby.vertex->const_start + kJointFirstConstant, joints * 3, &rows[0][0]
    );
    return true;
}

// Hands the RSX `slots` colours of the light texture from texel `first` as mesh.vp's c[32] up,
// as mesh.vert has them: 0x80 is 1. The texture's texels (A8R8G8B8, which no draw of the RSX's
// reads) are read in the program's memory, where they are kept from its first such draw and
// read again from the RSX's when it has been written. False when the texture has no such texel.
bool load_lights(const Image& texture, u32 first, u32 slots) {
    const u32 held = texture.width * texture.height;
    if (texture.memory.address == nullptr || texture.depth || first >= held) {
        return false;
    }
    if (g_lights_loaded.texture == &texture && g_lights_loaded.frame == g.frame
        && g_lights_loaded.written == texture.written && g_lights_loaded.first == first
        && g_lights_loaded.slots >= slots) {
        return true;
    }
    g_lights_loaded = {&texture, g.frame, texture.written, first, slots};
    g_joints_loaded = {};
    if (texture.kept.size() != held || texture.kept_at != texture.written) {
        texture.kept.resize(held);
        for (u32 row = 0; row < texture.height; ++row) {
            read_memory(
                texture.kept.data() + static_cast<std::size_t>(row) * texture.width,
                static_cast<const unsigned char*>(texture.memory.address)
                    + static_cast<std::size_t>(row) * texture.pitch,
                static_cast<std::size_t>(texture.width) * 4
            );
        }
        texture.kept_at = texture.written;
    }
    float colours[kLightsHeld][4] = {};
    const u32 count = std::min(slots, held - first);
    for (u32 i = 0; i < count; ++i) {
        const u32 argb = texture.kept[first + i];
        const u32 rgba[4] = {argb >> 16 & 255, argb >> 8 & 255, argb & 255, argb >> 24};
        for (u32 channel = 0; channel < 4; ++channel) {
            colours[i][channel] = static_cast<float>(rgba[channel]) / 128.0f;
        }
    }
    rsxLoadVertexProgramParameterBlock(
        g.context, g_mesh.vertex->const_start + kLightFirstConstant, slots, &colours[0][0]
    );
    return true;
}

// One past the highest light slot that the draw's vertices name, and no more than the RSX's
// constants hold; 0 where none names one. Read once for a draw, as joints_drawn is. `width` is
// the bytes of an index, 0 for a draw without indices.
struct SlotsDrawn {
    u32 written = 0;  // the buffers' counts of writes, summed
    u32 slots = 0;
    bool read = false;
};

std::map<std::array<u32, 4>, SlotsDrawn> g_slots_drawn;

u32 slots_drawn(u32 first, u32 count, u32 width) {
    const auto array = g.vertex_arrays.find(g.vertex_array);
    if (array == g.vertex_arrays.end()) {
        return 0;
    }
    const Attribute& slot = array->second.attributes[mesh::kLightSlot];
    const auto slot_buffer = g.buffers.find(slot.buffer);
    const auto elements = width != 0 ? g.buffers.find(array->second.elements) : g.buffers.end();
    if (!slot.enabled || slot.type != GL_FLOAT || slot_buffer == g.buffers.end()
        || slot_buffer->second.memory.address == nullptr
        || (width != 0 && elements == g.buffers.end())) {
        return 0;
    }
    const u32 written = slot_buffer->second.written + (width != 0 ? elements->second.written : 0);
    if (g_slots_drawn.size() > 16384) {
        g_slots_drawn.clear();  // draws of buffers long gone
    }
    SlotsDrawn& kept =
        g_slots_drawn[{slot.buffer, width != 0 ? array->second.elements : 0, first, count}];
    if (kept.read && kept.written == written) {
        return kept.slots;
    }
    const u32 stride = slot.stride > 0 ? static_cast<u32>(slot.stride) : 4;
    const auto* slot_bytes = static_cast<const unsigned char*>(slot_buffer->second.memory.address);
    const auto* index_bytes =
        width != 0 ? static_cast<const unsigned char*>(elements->second.memory.address) : nullptr;
    float highest = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        u32 vertex = first + i;
        if (width == 4) {
            read_memory(&vertex, index_bytes + first + i * 4, 4);
        } else if (width == 2) {
            u16 narrow = 0;
            read_memory(&narrow, index_bytes + first + i * 2, 2);
            vertex = narrow;
        }
        const std::size_t at = slot.pointer + static_cast<std::size_t>(vertex) * stride;
        if (at + 4 > slot_buffer->second.size) {
            continue;
        }
        float named = -1.0f;
        read_memory(&named, slot_bytes + at, sizeof named);
        highest = std::max(highest, named);
    }
    kept.read = true;
    kept.written = written;
    kept.slots = highest < 0.0f ? 0 : std::min(kLightsHeld, static_cast<u32>(highest) + 1);
    return kept.slots;
}

// One past the highest joint that the draw's vertices give a weight; 0 where they have none. The
// vertices are read once for a draw, and the answer kept until one of their buffers is written.
// `width` is the bytes of an index, 0 for a draw without indices.
struct JointsDrawn {
    u32 written = 0;  // the buffers' counts of writes, summed
    u32 joints = 0;
};

std::map<std::array<u32, 4>, JointsDrawn> g_joints_drawn;

u32 joints_drawn(u32 first, u32 count, u32 width) {
    const auto array = g.vertex_arrays.find(g.vertex_array);
    if (array == g.vertex_arrays.end()) {
        return 0;
    }
    const Attribute& joints = array->second.attributes[mesh::kJoints];
    const Attribute& weights = array->second.attributes[mesh::kWeights];
    const auto joint_buffer = g.buffers.find(joints.buffer);
    const auto weight_buffer = g.buffers.find(weights.buffer);
    const auto elements = width != 0 ? g.buffers.find(array->second.elements) : g.buffers.end();
    if (!joints.enabled || !weights.enabled || joints.type != GL_UNSIGNED_BYTE || joints.size != 4
        || weights.type != GL_FLOAT || weights.size != 4 || joint_buffer == g.buffers.end()
        || weight_buffer == g.buffers.end() || joint_buffer->second.memory.address == nullptr
        || weight_buffer->second.memory.address == nullptr
        || (width != 0 && elements == g.buffers.end())) {
        return 0;
    }
    const u32 written = joint_buffer->second.written + weight_buffer->second.written
                        + (width != 0 ? elements->second.written : 0);
    if (g_joints_drawn.size() > 4096) {
        g_joints_drawn.clear();  // draws of buffers long gone
    }
    JointsDrawn& kept =
        g_joints_drawn[{joints.buffer, width != 0 ? array->second.elements : 0, first, count}];
    if (kept.written == written) {
        return kept.joints;
    }
    const u32 joint_stride = joints.stride > 0 ? static_cast<u32>(joints.stride) : 4;
    const u32 weight_stride = weights.stride > 0 ? static_cast<u32>(weights.stride) : 16;
    const auto* joint_bytes =
        static_cast<const unsigned char*>(joint_buffer->second.memory.address);
    const auto* weight_bytes =
        static_cast<const unsigned char*>(weight_buffer->second.memory.address);
    const auto* index_bytes =
        width != 0 ? static_cast<const unsigned char*>(elements->second.memory.address) : nullptr;
    u32 highest = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 vertex = first + i;
        if (width == 4) {
            read_memory(&vertex, index_bytes + first + i * 4, 4);
        } else if (width == 2) {
            u16 narrow = 0;
            read_memory(&narrow, index_bytes + first + i * 2, 2);
            vertex = narrow;
        }
        const std::size_t joint_at =
            joints.pointer + static_cast<std::size_t>(vertex) * joint_stride;
        const std::size_t weight_at =
            weights.pointer + static_cast<std::size_t>(vertex) * weight_stride;
        if (joint_at + 4 > joint_buffer->second.size
            || weight_at + 16 > weight_buffer->second.size) {
            continue;
        }
        float weight[4];
        read_memory(weight, weight_bytes + weight_at, sizeof weight);
        unsigned char joint[4];
        read_memory(joint, joint_bytes + joint_at, sizeof joint);
        for (u32 k = 0; k < 4; ++k) {
            if (weight[k] > 0.0f) {
                highest = std::max(highest, joint[k] + 1u);
            }
        }
    }
    kept.written = written;
    kept.joints = highest;
    return highest;
}

// ---- Vertices ----

// The RSX's type of a GL attribute, or 0 for one it has none for. Bytes are parts of 255 to the
// RSX whether GL has them so or as whole numbers (a moby's joints, which moby.vp multiplies back).
u8 data_type(const Attribute& attribute) {
    if (attribute.type == GL_FLOAT) {
        return GCM_VERTEX_DATA_TYPE_F32;
    }
    if (attribute.type == GL_UNSIGNED_BYTE) {
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

// The attributes a kind of program reads, a bit each: the others of a vertex array are not told
// to the RSX.
u32 attributes_read(ProgramKind kind) {
    switch (kind) {
        case ProgramKind::Direct:
            return 0b1111;
        case ProgramKind::Mesh:
            return 0b1111 | 1u << mesh::kLightSlot;
        case ProgramKind::Effect:
            return 0b0111;
        case ProgramKind::Moby:
            return 0b1111 | 1u << mesh::kJoints | 1u << mesh::kWeights | 1u << mesh::kLightSlot;
        case ProgramKind::Sky:
            return 0b1101;
        case ProgramKind::None:
            break;
    }
    return 0;
}

// The bound vertex array's attributes that `kind` reads. False when one of them the RSX cannot
// read.
bool bind_vertex_array(ProgramKind kind) {
    const auto found = g.vertex_arrays.find(g.vertex_array);
    if (found == g.vertex_arrays.end()) {
        return false;
    }
    const u32 read = attributes_read(kind);
    u32 kept = 0;
    for (u32 i = 0; i < kAttributes; ++i) {
        const Attribute& attribute = found->second.attributes[i];
        // One value an instance is not the RSX's to read: the program's kind takes it, or not.
        if (!attribute.enabled || attribute.divisor != 0 || (read & 1u << i) == 0) {
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

// How many vertices the bound vertex array's buffers hold for `kind`'s attributes: a draw past
// them would have the RSX read memory that is not the buffer's.
u32 vertices_held(ProgramKind kind) {
    const auto found = g.vertex_arrays.find(g.vertex_array);
    if (found == g.vertex_arrays.end()) {
        return 0;
    }
    const u32 read = attributes_read(kind);
    u32 held = ~0u;
    for (u32 i = 0; i < kAttributes; ++i) {
        const Attribute& attribute = found->second.attributes[i];
        if (!attribute.enabled || attribute.divisor != 0 || (read & 1u << i) == 0) {
            continue;
        }
        const auto buffer = g.buffers.find(attribute.buffer);
        const u32 bytes = static_cast<u32>(attribute.size) * (attribute.type == GL_FLOAT ? 4u : 1u);
        if (buffer == g.buffers.end() || attribute.pointer + bytes > buffer->second.size) {
            return 0;
        }
        const u32 stride = attribute.stride > 0 ? static_cast<u32>(attribute.stride) : bytes;
        held = std::min(held, (buffer->second.size - attribute.pointer - bytes) / stride + 1);
    }
    return held;
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

// Why a draw was left out, said once in the log for each reason.
enum class LeftOut : u32 {
    NoProgram,   // the GL program has no RSX program yet
    NoTarget,    // the framebuffer has nothing to draw into
    NoVertices,  // an attribute the RSX cannot read, or no buffer behind one
    NoIndices,   // no element buffer, or indices of a type the RSX has not
    PastEnd,     // vertices past the end of their buffers
    ManyJoints,  // a moby of more joints than the RSX has constants for
};

void leave_out(LeftOut reason) {
    constexpr const char* kReasons[] = {
        "of GL programs the layer has no RSX program for",
        "into a framebuffer with nothing to draw into",
        "of vertex arrays the RSX cannot read",
        "of indices the RSX cannot read",
        "of vertices past the end of their buffers",
        "of mobys with more joints than the RSX's constants hold",
    };
    ++g.draws_left_out;
    const u32 bit = 1u << static_cast<u32>(reason);
    if ((g.left_out_logged & bit) == 0) {
        g.left_out_logged |= bit;
        log::info("rsxgl: draws {} are left out", kReasons[static_cast<u32>(reason)]);
    }
}

// Whether the bound program's draw has no pixel to write, whatever its vertices: the mesh
// program's translucent pass of a material without a cutout (mesh_constants). The renderer draws
// every instance in both passes, and an instance is a draw here.
bool draws_nothing(const Program& program) {
    return program.kind == ProgramKind::Mesh && moby_pass(program) == MobyPass::Solid
           && program.uniforms[mesh::kTranslucentPass][0] != 0.0f
           && program.uniforms[mesh::kCutout][0] == 0.0f;
}

// What every draw does before its vertices: the surface, GL's state, the bound program as the
// RSX's with its numbers, the textures. Null when the draw is left out.
const Program* begin_draw(Target& target) {
    const auto program = g.programs.find(g.program);
    if (program == g.programs.end() || program->second.kind == ProgramKind::None) {
        leave_out(LeftOut::NoProgram);
        return nullptr;
    }
    if (draws_nothing(program->second)) {
        return nullptr;
    }
    if (!apply_target(target)) {
        leave_out(LeftOut::NoTarget);
        return nullptr;
    }
    const ProgramKind kind = drawn_kind(program->second);
    if (!bind_vertex_array(kind)) {
        leave_out(LeftOut::NoVertices);
        return nullptr;
    }
    apply_state(target);
    switch (kind) {
        case ProgramKind::Direct: {
            float constants[7][4];
            direct_constants(program->second, constants);
            load_direct(constants);
            break;
        }
        case ProgramKind::Effect: {
            float constants[kEffectConstants][4];
            effect_constants(program->second, constants);
            load_program(
                ProgramKind::Effect,
                g_effect,
                g_direct_fragments[direct_parts(constants + 7)],
                &constants[0][0],
                kEffectConstants
            );
            break;
        }
        case ProgramKind::Sky: {
            float constants[5][4];
            sky_constants(program->second, constants);
            load_program(ProgramKind::Sky, g_sky, g_sky_fragment, &constants[0][0], 5);
            break;
        }
        case ProgramKind::Mesh:
        case ProgramKind::Moby: {
            const Loaded& loaded = kind == ProgramKind::Moby ? g_moby : g_mesh;
            float constants[kMeshDrawConstants][4];
            mesh_constants(program->second, constants);
            use_vertex(kind, loaded);
            // The joints' constants hold numbers from the first: an instance without joints
            // weighs whatever is there by nothing.
            static bool joints_cleared = false;
            if (kind == ProgramKind::Moby && !joints_cleared) {
                joints_cleared = true;
                g_lights_loaded = {};
                static const float kNothing[kJointsHeld * 3][4] = {};
                rsxLoadVertexProgramParameterBlock(
                    g.context,
                    loaded.vertex->const_start + kJointFirstConstant,
                    kJointsHeld * 3,
                    &kNothing[0][0]
                );
            }
            rsxLoadVertexProgramParameterBlock(
                g.context,
                loaded.vertex->const_start + kMeshInstanceConstants,
                kMeshDrawConstants,
                &constants[0][0]
            );
            float of_pass[kMeshPassConstants][4];
            pass_constants(program->second, of_pass);
            use_fragment(g_mesh_fragments[mesh_parts(kind, constants, of_pass)]);
            rsxLoadVertexProgramParameterBlock(
                g.context,
                loaded.vertex->const_start + kMeshPassFirstConstant,
                kMeshPassConstants,
                &of_pass[0][0]
            );
            break;
        }
        case ProgramKind::None:
            break;
    }
    load_bound_texture(0);
    return &program->second;
}

// What a draw's instances are to the bound program: the mesh program's numbers of each, from
// the vertex array's values an instance, and a moby's joints and lights besides; nothing to the
// others, whose instances are the same draw again. `joints` is how many joints the draw's
// vertices are skinned with (joints_drawn), `slots` how many light slots they name
// (slots_drawn).
struct Instances {
    const Program* program = nullptr;
    ProgramKind kind = ProgramKind::None;
    const float* model = nullptr;
    const float* tint = nullptr;
    u32 model_stride = 0;
    u32 tint_stride = 0;
    const float* palette = nullptr;
    const float* shine = nullptr;
    const float* light = nullptr;
    const float* ambient = nullptr;
    u32 palette_stride = 0;
    u32 shine_stride = 0;
    u32 light_stride = 0;
    u32 ambient_stride = 0;
    const Image* matrices = nullptr;  // the palette texture
    const float* lights = nullptr;
    u32 lights_stride = 0;
    const Image* colours = nullptr;  // the light texture
    u32 joints = 0;
    u32 slots = 0;

    Instances(const Program* bound, u32 skinned_with, u32 slots_named)
        : program(bound),
          kind(drawn_kind(*bound)),
          joints(skinned_with),
          slots(slots_named) {
        if (program->kind == ProgramKind::Mesh) {
            model = instance_values(mesh::kModel, model_stride);
            tint = instance_values(mesh::kTint, tint_stride);
        }
        if (kind == ProgramKind::Mesh) {
            lights = instance_values(mesh::kLights, lights_stride);
            const auto texture = g.images.find(g.texture[mesh::kLightUnit]);
            if (texture != g.images.end()) {
                colours = &texture->second;
            }
        }
        if (kind == ProgramKind::Moby) {
            palette = instance_values(mesh::kPalette, palette_stride);
            shine = instance_values(mesh::kLights, shine_stride);
            light = instance_values(mesh::kMobyLight, light_stride);
            ambient = instance_values(mesh::kMobyAmbient, ambient_stride);
            const auto texture = g.images.find(g.texture[mesh::kPaletteUnit]);
            if (texture != g.images.end() && !texture->second.floats.empty()) {
                matrices = &texture->second;
            }
        }
    }

    void load(GLsizei instance) const {
        if (program->kind != ProgramKind::Mesh) {
            return;
        }
        const Loaded& loaded = kind == ProgramKind::Moby ? g_moby : g_mesh;
        float constants[kMeshInstanceConstants][4];
        float placed[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        stepped(model, model_stride, instance, placed);
        float tinted[4] = {1, 1, 1, 1};
        stepped(tint, tint_stride, instance, tinted);
        // A lit tie's or shrub's first colour in the light texture, -1 for what has none.
        float first_colour[1] = {-1.0f};
        stepped(lights, lights_stride, instance, first_colour);
        const bool has_colours =
            slots != 0 && first_colour[0] >= 0.0f && colours != nullptr
            && load_lights(*colours, static_cast<u32>(first_colour[0] + 0.5f), slots);
        mesh_instance_constants(*program, placed, tinted, has_colours, constants);
        rsxLoadVertexProgramParameterBlock(
            g.context, loaded.vertex->const_start, kMeshInstanceConstants, &constants[0][0]
        );
        if (kind != ProgramKind::Moby) {
            return;
        }
        // The instance's first matrix in the palette, -1 for one drawn as stored.
        float first[1] = {-1.0f};
        stepped(palette, palette_stride, instance, first);
        const bool skinned = first[0] >= 0.0f && matrices != nullptr
                             && load_joints(*matrices, static_cast<u32>(first[0] + 0.5f), joints);
        float lights_of[4] = {0, 0, 0, 0};
        stepped(light, light_stride, instance, lights_of);
        float ambient_of[4] = {0, 0, 0, 0};
        stepped(ambient, ambient_stride, instance, ambient_of);
        float shine_of[1] = {0.0f};
        stepped(shine, shine_stride, instance, shine_of);
        float lit[kMobyInstanceConstants][4];
        moby_instance_constants(
            *program, lights_of, ambient_of, skinned, moby_shine(*program, placed, shine_of[0]), lit
        );
        rsxLoadVertexProgramParameterBlock(
            g.context,
            loaded.vertex->const_start + kMobyFirstConstant,
            kMobyInstanceConstants,
            &lit[0][0]
        );
    }
};

// How many joints a moby's draw is skinned with: 0 for another program's draw. False, and the
// draw left out, when they are more than the RSX's constants hold.
bool joints_of_draw(const Program& program, u32 first, u32 count, u32 width, u32& joints) {
    joints = drawn_kind(program) == ProgramKind::Moby ? joints_drawn(first, count, width) : 0;
    if (joints > kJointsHeld) {
        leave_out(LeftOut::ManyJoints);
        return false;
    }
    return true;
}

// How many light slots a tie's or shrub's draw names: 0 for another program's draw.
u32 slots_of_draw(const Program& program, u32 first, u32 count, u32 width) {
    return drawn_kind(program) == ProgramKind::Mesh ? slots_drawn(first, count, width) : 0;
}

// A draw with a program the layer has for the RSX; one without is left out. The RSX has no
// instances: each is a draw of its own.
void draw_arrays_instanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    Target target;
    if (count <= 0 || instances <= 0) {
        return;
    }
    const u64 began = __builtin_ppc_get_timebase();
    const Program* program = begin_draw(target);
    if (program == nullptr) {
        return;
    }
    if (static_cast<u32>(first) + static_cast<u32>(count) > vertices_held(drawn_kind(*program))) {
        leave_out(LeftOut::PastEnd);
        return;
    }
    u32 joints = 0;
    if (!joints_of_draw(*program, static_cast<u32>(first), static_cast<u32>(count), 0, joints)) {
        return;
    }
    const Instances each(
        program,
        joints,
        slots_of_draw(*program, static_cast<u32>(first), static_cast<u32>(count), 0)
    );
    u64 before = __builtin_ppc_get_timebase();
    g.ticks_state += before - began;
    for (GLsizei instance = 0; instance < instances; ++instance) {
        ++g.draws;
        each.load(instance);
        const u64 loaded = __builtin_ppc_get_timebase();
        rsxDrawVertexArray(
            g.context, primitive(mode), static_cast<u32>(first), static_cast<u32>(count)
        );
        const u64 drawn = __builtin_ppc_get_timebase();
        g.ticks_constants += loaded - before;
        g.ticks_commands += drawn - loaded;
        before = drawn;
    }
    mark_used(target);
}

void draw_arrays(GLenum mode, GLint first, GLsizei count) {
    draw_arrays_instanced(mode, first, count, 1);
}

// Indexed: `indices` is where the first index is in the vertex array's element buffer. The RSX
// reads indices of 16 and of 32 bits, in the host's byte order as the renderer wrote them.
void draw_elements_instanced(
    GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances
) {
    if (count <= 0 || instances <= 0) {
        return;
    }
    const auto array = g.vertex_arrays.find(g.vertex_array);
    const auto elements =
        array != g.vertex_arrays.end() ? g.buffers.find(array->second.elements) : g.buffers.end();
    const auto first = static_cast<u32>(reinterpret_cast<std::uintptr_t>(indices));
    const u32 width = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 0;
    if (elements == g.buffers.end() || elements->second.memory.address == nullptr || width == 0
        || first + width * static_cast<u32>(count) > elements->second.size) {
        leave_out(LeftOut::NoIndices);
        return;
    }
    Target target;
    const u64 began = __builtin_ppc_get_timebase();
    const Program* program = begin_draw(target);
    if (program == nullptr) {
        return;
    }
    elements->second.drawn = g.frame;
    u32 joints = 0;
    if (!joints_of_draw(*program, first, static_cast<u32>(count), width, joints)) {
        return;
    }
    const Instances each(
        program, joints, slots_of_draw(*program, first, static_cast<u32>(count), width)
    );
    u64 before = __builtin_ppc_get_timebase();
    g.ticks_state += before - began;
    for (GLsizei instance = 0; instance < instances; ++instance) {
        ++g.draws;
        each.load(instance);
        const u64 loaded = __builtin_ppc_get_timebase();
        rsxDrawIndexArray(
            g.context,
            primitive(mode),
            elements->second.memory.offset + first,
            static_cast<u32>(count),
            width == 4 ? GCM_INDEX_TYPE_32B : GCM_INDEX_TYPE_16B,
            GCM_LOCATION_RSX
        );
        const u64 drawn = __builtin_ppc_get_timebase();
        g.ticks_constants += loaded - before;
        g.ticks_commands += drawn - loaded;
        before = drawn;
    }
    mark_used(target);
}

void draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    draw_elements_instanced(mode, count, type, indices, 1);
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
    CopyVertex vertices[kCopyVertices];
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
    write_memory(memory.address, vertices, sizeof vertices);
    const u32 offset = memory.offset;
    retire(memory);

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
    load_program(ProgramKind::Direct, g_direct, g_copy_fragment, &constants[0][0], 7);
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
    const auto* picture = static_cast<const volatile u32*>(
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
    load(g_direct, programs::direct_vpo);
    load(g_sky, programs::sky_vpo);
    load(g_mesh, programs::mesh_vpo);
    load(g_moby, programs::moby_vpo);
    load(g_effect, programs::effect_vpo);
    bool loaded = g_white.memory.address != nullptr && load(g_sky_fragment, programs::sky_fpo)
                  && load(g_copy_fragment, programs::copy_fpo);
    for (u32 set = 0; set < mesh_part::kSets; ++set) {
        loaded = loaded && load(g_mesh_fragments[set], kMeshFragments[set]);
    }
    for (u32 set = 0; set < direct_part::kSets; ++set) {
        loaded = loaded && load(g_direct_fragments[set], kDirectFragments[set]);
    }
    if (!loaded) {
        error = "no RSX memory for the layer's programs";
        return false;
    }
    fill_memory(g_white.memory.address, 0xFF, 4);
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
    OPENRAC_RSXGL_ENTRY(glDrawArraysInstanced, draw_arrays_instanced),
    OPENRAC_RSXGL_ENTRY(glDrawElements, draw_elements),
    OPENRAC_RSXGL_ENTRY(glDrawElementsInstanced, draw_elements_instanced),
    OPENRAC_RSXGL_ENTRY(glBlitFramebuffer, blit_framebuffer),
    OPENRAC_RSXGL_ENTRY(glCopyTexSubImage2D, copy_tex_sub_image),
    OPENRAC_RSXGL_ENTRY(glReadPixels, read_pixels),
    OPENRAC_RSXGL_ENTRY(glFinish, finish),
};
const std::size_t kDrawFunctionCount = std::size(kDrawFunctions);

}  // namespace openrac::rsxgl
