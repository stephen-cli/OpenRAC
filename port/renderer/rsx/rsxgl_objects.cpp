// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// GL's objects as the layer keeps them (rsxgl_internal.h): textures and renderbuffers in the RSX's
// memory, buffers, vertex arrays, samplers, framebuffers, shaders and programs with their uniforms.

#include <algorithm>
#include <cstring>
#include <string_view>

#include "common/log.h"
#include "renderer/rsx/rsxgl_internal.h"

namespace openrac::rsxgl {

using namespace openrac::gl;

Memory allocate(u32 bytes) {
    Memory memory;
    memory.address = rsxMemalign(64, bytes);
    if (memory.address == nullptr) {
        log::error("rsxgl: no RSX memory for {} bytes", bytes);
        return memory;
    }
    rsxAddressToOffset(memory.address, &memory.offset);
    return memory;
}

void retire(Memory& memory) {
    if (memory.address != nullptr) {
        g.retired.push_back(memory.address);
    }
    memory = Memory{};
}

Image* image(GLuint name) {
    const auto found = g.images.find(name);
    return found != g.images.end() && found->second.memory.address != nullptr ? &found->second
                                                                              : nullptr;
}

void before_write(u32 used) {
    if (used == g.sync) {
        wait_idle();
    }
}

namespace {

// ---- Names ----

void generate(GLsizei count, GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        names[i] = g.next_name++;
    }
}

// ---- Textures and renderbuffers ----

bool is_depth_format(GLenum format) {
    return format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8
           || format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT32F;
}

// Colour the layer holds: eight bits a channel. A float format is held so too until the RSX's
// float surfaces are used.
bool is_colour_format(GLenum format) {
    return format == GL_RGBA8 || format == GL_RGBA || format == GL_RGBA16F;
}

// Memory for an image of that size, kept when the size is the one it has.
void store(Image& into, GLsizei width, GLsizei height, bool depth) {
    const auto wide = static_cast<u32>(std::max(width, 0));
    const auto high = static_cast<u32>(std::max(height, 0));
    if (into.memory.address != nullptr && into.width == wide && into.height == high
        && into.depth == depth) {
        before_write(into.used);
        return;
    }
    retire(into.memory);
    into.width = wide;
    into.height = high;
    into.pitch = wide * 4;
    into.depth = depth;
    if (wide == 0 || high == 0) {
        return;
    }
    into.memory = allocate(high * into.pitch);
    if (into.memory.address != nullptr) {
        std::memset(into.memory.address, 0, high * into.pitch);
    }
    g.target_stale = true;
}

// GL's RGBA bytes into the RSX's A8R8G8B8, a rectangle of an image.
void write_rgba(Image& into, u32 x, u32 y, u32 wide, u32 high, const void* pixels) {
    const auto* from = static_cast<const unsigned char*>(pixels);
    for (u32 row = 0; row < high; ++row) {
        auto* to = static_cast<unsigned char*>(into.memory.address)
                   + static_cast<std::size_t>(y + row) * into.pitch
                   + static_cast<std::size_t>(x) * 4;
        for (u32 column = 0; column < wide; ++column, from += 4, to += 4) {
            to[0] = from[3];
            to[1] = from[0];
            to[2] = from[1];
            to[3] = from[2];
        }
    }
}

void bind_texture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D && g.unit < kTextureUnits) {
        g.texture[g.unit] = texture;
    }
}

void active_texture(GLenum texture) {
    g.unit = texture - GL_TEXTURE0;
}

// The texture bound to the active unit, made on its first use.
Image* bound_texture(GLenum target) {
    if (target != GL_TEXTURE_2D || g.unit >= kTextureUnits || g.texture[g.unit] == 0) {
        return nullptr;
    }
    return &g.images[g.texture[g.unit]];
}

void tex_image(
    GLenum target,
    GLint level,
    GLint internal_format,
    GLsizei width,
    GLsizei height,
    GLint,
    GLenum format,
    GLenum type,
    const void* pixels
) {
    Image* into = bound_texture(target);
    if (into == nullptr || level != 0) {
        return;  // one level: the RSX's mip chains come with the textures' own step
    }
    const auto internal = static_cast<GLenum>(internal_format);
    if (!is_depth_format(internal) && !is_colour_format(internal)) {
        static bool told = false;
        if (!told) {
            told = true;
            log::info("rsxgl: texture format {:#x} has no memory yet", internal);
        }
        retire(into->memory);
        return;
    }
    store(*into, width, height, is_depth_format(internal));
    if (pixels != nullptr && into->memory.address != nullptr && format == GL_RGBA
        && type == GL_UNSIGNED_BYTE) {
        write_rgba(*into, 0, 0, into->width, into->height, pixels);
    }
}

void tex_sub_image(
    GLenum target,
    GLint level,
    GLint x,
    GLint y,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    const void* pixels
) {
    Image* into = bound_texture(target);
    if (into == nullptr || into->memory.address == nullptr || level != 0 || pixels == nullptr
        || format != GL_RGBA || type != GL_UNSIGNED_BYTE || x < 0 || y < 0 || width < 0
        || height < 0 || static_cast<u32>(x + width) > into->width
        || static_cast<u32>(y + height) > into->height) {
        return;
    }
    before_write(into->used);
    write_rgba(
        *into,
        static_cast<u32>(x),
        static_cast<u32>(y),
        static_cast<u32>(width),
        static_cast<u32>(height),
        pixels
    );
}

void set_parameter(GLenum& min, GLenum& mag, GLenum& s, GLenum& t, GLenum name, GLint value) {
    const auto to = static_cast<GLenum>(value);
    switch (name) {
        case GL_TEXTURE_MIN_FILTER:
            min = to;
            break;
        case GL_TEXTURE_MAG_FILTER:
            mag = to;
            break;
        case GL_TEXTURE_WRAP_S:
            s = to;
            break;
        case GL_TEXTURE_WRAP_T:
            t = to;
            break;
        default:
            break;
    }
}

void tex_parameter(GLenum target, GLenum name, GLint value) {
    if (Image* of = bound_texture(target)) {
        set_parameter(of->min_filter, of->mag_filter, of->wrap_s, of->wrap_t, name, value);
    }
}

void sampler_parameter(GLuint sampler, GLenum name, GLint value) {
    Sampler& of = g.samplers[sampler];
    set_parameter(of.min_filter, of.mag_filter, of.wrap_s, of.wrap_t, name, value);
}

void bind_sampler(GLuint unit, GLuint sampler) {
    if (unit < kTextureUnits) {
        g.sampler[unit] = sampler;
    }
}

void delete_images(GLsizei count, const GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        const auto found = g.images.find(names[i]);
        if (found == g.images.end()) {
            continue;
        }
        retire(found->second.memory);
        g.images.erase(found);
        for (GLuint& bound : g.texture) {
            if (bound == names[i]) {
                bound = 0;
            }
        }
        g.target_stale = true;
    }
}

void delete_samplers(GLsizei count, const GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        g.samplers.erase(names[i]);
    }
}

void bind_renderbuffer(GLenum, GLuint renderbuffer) {
    g.renderbuffer = renderbuffer;
}

void renderbuffer_storage(GLenum, GLenum format, GLsizei width, GLsizei height) {
    if (g.renderbuffer != 0) {
        store(g.images[g.renderbuffer], width, height, is_depth_format(format));
    }
}

// One sample a pixel: the RSX's multisampled surfaces are not used yet.
void renderbuffer_storage_multisample(
    GLenum target, GLsizei, GLenum format, GLsizei width, GLsizei height
) {
    renderbuffer_storage(target, format, width, height);
}

// ---- Buffers and vertex arrays ----

void bind_buffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) {
        g.array_buffer = buffer;
    }
}

// A buffer a draw of this frame read is not written in place: the RSX reads a draw's vertices
// later, and the draw may still be queued. It takes new memory without waiting for the RSX, and
// the old is given back with the frame (present).
void new_store(Buffer& buffer, u32 bytes, bool keep) {
    Memory old = buffer.memory;
    buffer.memory = bytes != 0 ? allocate(bytes) : Memory{};
    if (keep && old.address != nullptr && buffer.memory.address != nullptr) {
        std::memcpy(buffer.memory.address, old.address, std::min(bytes, buffer.size));
    }
    retire(old);
    buffer.size = bytes;
    buffer.drawn = 0;
}

void buffer_data(GLenum target, GLsizeiptr size, const void* data, GLenum) {
    if (target != GL_ARRAY_BUFFER || g.array_buffer == 0 || size < 0) {
        return;
    }
    Buffer& into = g.buffers[g.array_buffer];
    const auto bytes = static_cast<u32>(size);
    if (into.memory.address == nullptr || into.size != bytes || into.drawn == g.frame) {
        new_store(into, bytes, false);
    }
    if (data != nullptr && into.memory.address != nullptr) {
        std::memcpy(into.memory.address, data, bytes);
    }
}

void buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (target != GL_ARRAY_BUFFER || g.array_buffer == 0 || offset < 0 || size < 0) {
        return;
    }
    Buffer& into = g.buffers[g.array_buffer];
    if (into.memory.address == nullptr || static_cast<u32>(offset + size) > into.size) {
        return;
    }
    if (into.drawn == g.frame) {
        new_store(into, into.size, true);
        if (into.memory.address == nullptr) {
            return;
        }
    }
    std::memcpy(
        static_cast<unsigned char*>(into.memory.address) + offset, data, static_cast<u32>(size)
    );
}

void delete_buffers(GLsizei count, const GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        const auto found = g.buffers.find(names[i]);
        if (found != g.buffers.end()) {
            retire(found->second.memory);
            g.buffers.erase(found);
        }
        if (g.array_buffer == names[i]) {
            g.array_buffer = 0;
        }
    }
}

void bind_vertex_array(GLuint vertex_array) {
    g.vertex_array = vertex_array;
}

void enable_attribute(GLuint index) {
    if (index < kAttributes && g.vertex_array != 0) {
        g.vertex_arrays[g.vertex_array].attributes[index].enabled = true;
    }
}

void attribute_pointer(
    GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer
) {
    if (index >= kAttributes || g.vertex_array == 0) {
        return;
    }
    Attribute& attribute = g.vertex_arrays[g.vertex_array].attributes[index];
    attribute.size = size;
    attribute.type = type;
    attribute.normalized = normalized != GL_FALSE;
    attribute.stride = stride;
    attribute.pointer = static_cast<u32>(reinterpret_cast<std::uintptr_t>(pointer));
    attribute.buffer = g.array_buffer;
}

void delete_vertex_arrays(GLsizei count, const GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        g.vertex_arrays.erase(names[i]);
        if (g.vertex_array == names[i]) {
            g.vertex_array = 0;
        }
    }
}

// ---- Framebuffers ----

void bind_framebuffer(GLenum target, GLuint framebuffer) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        if (g.draw_framebuffer != framebuffer) {
            g.target_stale = true;
        }
        g.draw_framebuffer = framebuffer;
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
        g.read_framebuffer = framebuffer;
    }
}

// The framebuffer a target names: GL_FRAMEBUFFER is the one drawn into.
Framebuffer* bound_framebuffer(GLenum target) {
    const GLuint name = target == GL_READ_FRAMEBUFFER ? g.read_framebuffer : g.draw_framebuffer;
    return name != 0 ? &g.framebuffers[name] : nullptr;
}

void attach(GLenum target, GLenum attachment, GLuint name) {
    Framebuffer* to = bound_framebuffer(target);
    if (to == nullptr) {
        return;
    }
    if (attachment == GL_COLOR_ATTACHMENT0) {
        to->colour = name;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        to->depth = name;
    }
    g.target_stale = true;
}

void framebuffer_texture(GLenum target, GLenum attachment, GLenum, GLuint texture, GLint) {
    attach(target, attachment, texture);
}

void framebuffer_texture_multisample(
    GLenum target, GLenum attachment, GLenum, GLuint texture, GLint, GLsizei
) {
    attach(target, attachment, texture);
}

void framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum, GLuint renderbuffer) {
    attach(target, attachment, renderbuffer);
}

GLenum framebuffer_status(GLenum) {
    return GL_FRAMEBUFFER_COMPLETE;
}

void delete_framebuffers(GLsizei count, const GLuint* names) {
    for (GLsizei i = 0; i < count; ++i) {
        g.framebuffers.erase(names[i]);
        if (g.draw_framebuffer == names[i]) {
            g.draw_framebuffer = 0;
            g.target_stale = true;
        }
        if (g.read_framebuffer == names[i]) {
            g.read_framebuffer = 0;
        }
    }
}

// ---- Shaders and programs ----

// The direct program's uniforms, in DirectUniform's order.
constexpr std::string_view kDirectUniforms[] = {
    "pixel_offset",
    "textured",
    "tcc",
    "tfx",
    "tex_alpha_scale",
    "fog_enable",
    "fog_colour",
    "alpha_test",
    "alpha_ref",
    "alpha_keep_failing",
    "source_scale",
    "source_fix",
};
static_assert(std::size(kDirectUniforms) <= kUniformSlots);

// The line in the GLSL of both stages that names the RSX's program for them.
constexpr std::string_view kDirectMark = "rsx-program: direct";

GLuint create_shader(GLenum type) {
    const GLuint name = g.next_name++;
    g.shaders[name].type = type;
    return name;
}

void shader_source(
    GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths
) {
    std::string& source = g.shaders[shader].source;
    source.clear();
    for (GLsizei i = 0; i < count; ++i) {
        if (lengths != nullptr && lengths[i] >= 0) {
            source.append(strings[i], static_cast<std::size_t>(lengths[i]));
        } else {
            source.append(strings[i]);
        }
    }
}

void delete_shader(GLuint shader) {
    g.shaders.erase(shader);
}

GLuint create_program() {
    const GLuint name = g.next_name++;
    g.programs[name] = Program{};
    return name;
}

void attach_shader(GLuint program, GLuint shader) {
    const auto found = g.shaders.find(shader);
    if (found == g.shaders.end()) {
        return;
    }
    Program& to = g.programs[program];
    (found->second.type == GL_VERTEX_SHADER ? to.vertex : to.fragment) = shader;
}

// Nothing is compiled: the program is the RSX's one its sources name, or none.
void link_program(GLuint program) {
    Program& linked = g.programs[program];
    const auto vertex = g.shaders.find(linked.vertex);
    const auto fragment = g.shaders.find(linked.fragment);
    linked.kind = ProgramKind::None;
    if (vertex != g.shaders.end() && fragment != g.shaders.end()
        && vertex->second.source.find(kDirectMark) != std::string::npos
        && fragment->second.source.find(kDirectMark) != std::string::npos) {
        linked.kind = ProgramKind::Direct;
    }
}

void delete_program(GLuint program) {
    g.programs.erase(program);
    if (g.program == program) {
        g.program = 0;
    }
}

void use_program(GLuint program) {
    g.program = program;
}

// A shader compiled and a program linked report success and an empty log.
void get_status(GLuint, GLenum name, GLint* value) {
    *value = name == GL_COMPILE_STATUS || name == GL_LINK_STATUS ? GL_TRUE : 0;
}

GLint uniform_location(GLuint program, const GLchar* name) {
    const auto found = g.programs.find(program);
    if (found == g.programs.end() || found->second.kind != ProgramKind::Direct) {
        return -1;
    }
    const std::string_view wanted(name);
    for (std::size_t i = 0; i < std::size(kDirectUniforms); ++i) {
        if (kDirectUniforms[i] == wanted) {
            return static_cast<GLint>(i);
        }
    }
    return -1;
}

void set_uniform(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    const auto found = g.programs.find(g.program);
    if (found == g.programs.end() || location < 0 || static_cast<u32>(location) >= kUniformSlots) {
        return;
    }
    found->second.uniforms[static_cast<std::size_t>(location)] = {x, y, z, w};
}

void uniform_1i(GLint location, GLint x) {
    set_uniform(location, static_cast<GLfloat>(x), 0.0f, 0.0f, 0.0f);
}

void uniform_1f(GLint location, GLfloat x) {
    set_uniform(location, x, 0.0f, 0.0f, 0.0f);
}

void uniform_2f(GLint location, GLfloat x, GLfloat y) {
    set_uniform(location, x, y, 0.0f, 0.0f);
}

void uniform_3f(GLint location, GLfloat x, GLfloat y, GLfloat z) {
    set_uniform(location, x, y, z, 0.0f);
}

void uniform_4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    set_uniform(location, x, y, z, w);
}

}  // namespace

const Entry kObjectFunctions[] = {
    OPENRAC_RSXGL_ENTRY(glGenTextures, generate),
    OPENRAC_RSXGL_ENTRY(glGenSamplers, generate),
    OPENRAC_RSXGL_ENTRY(glGenBuffers, generate),
    OPENRAC_RSXGL_ENTRY(glGenVertexArrays, generate),
    OPENRAC_RSXGL_ENTRY(glGenFramebuffers, generate),
    OPENRAC_RSXGL_ENTRY(glGenRenderbuffers, generate),
    OPENRAC_RSXGL_ENTRY(glBindTexture, bind_texture),
    OPENRAC_RSXGL_ENTRY(glActiveTexture, active_texture),
    OPENRAC_RSXGL_ENTRY(glTexImage2D, tex_image),
    OPENRAC_RSXGL_ENTRY(glTexSubImage2D, tex_sub_image),
    OPENRAC_RSXGL_ENTRY(glTexParameteri, tex_parameter),
    OPENRAC_RSXGL_ENTRY(glSamplerParameteri, sampler_parameter),
    OPENRAC_RSXGL_ENTRY(glBindSampler, bind_sampler),
    OPENRAC_RSXGL_ENTRY(glDeleteTextures, delete_images),
    OPENRAC_RSXGL_ENTRY(glDeleteRenderbuffers, delete_images),
    OPENRAC_RSXGL_ENTRY(glDeleteSamplers, delete_samplers),
    OPENRAC_RSXGL_ENTRY(glBindRenderbuffer, bind_renderbuffer),
    OPENRAC_RSXGL_ENTRY(glRenderbufferStorage, renderbuffer_storage),
    OPENRAC_RSXGL_ENTRY(glRenderbufferStorageMultisample, renderbuffer_storage_multisample),
    OPENRAC_RSXGL_ENTRY(glRenderbufferStorageMultisampleEXT, renderbuffer_storage_multisample),
    OPENRAC_RSXGL_ENTRY(glBindBuffer, bind_buffer),
    OPENRAC_RSXGL_ENTRY(glBufferData, buffer_data),
    OPENRAC_RSXGL_ENTRY(glBufferSubData, buffer_sub_data),
    OPENRAC_RSXGL_ENTRY(glDeleteBuffers, delete_buffers),
    OPENRAC_RSXGL_ENTRY(glBindVertexArray, bind_vertex_array),
    OPENRAC_RSXGL_ENTRY(glEnableVertexAttribArray, enable_attribute),
    OPENRAC_RSXGL_ENTRY(glVertexAttribPointer, attribute_pointer),
    OPENRAC_RSXGL_ENTRY(glDeleteVertexArrays, delete_vertex_arrays),
    OPENRAC_RSXGL_ENTRY(glBindFramebuffer, bind_framebuffer),
    OPENRAC_RSXGL_ENTRY(glFramebufferTexture2D, framebuffer_texture),
    OPENRAC_RSXGL_ENTRY(glFramebufferTexture2DMultisampleEXT, framebuffer_texture_multisample),
    OPENRAC_RSXGL_ENTRY(glFramebufferRenderbuffer, framebuffer_renderbuffer),
    OPENRAC_RSXGL_ENTRY(glCheckFramebufferStatus, framebuffer_status),
    OPENRAC_RSXGL_ENTRY(glDeleteFramebuffers, delete_framebuffers),
    OPENRAC_RSXGL_ENTRY(glCreateShader, create_shader),
    OPENRAC_RSXGL_ENTRY(glShaderSource, shader_source),
    OPENRAC_RSXGL_ENTRY(glDeleteShader, delete_shader),
    OPENRAC_RSXGL_ENTRY(glCreateProgram, create_program),
    OPENRAC_RSXGL_ENTRY(glAttachShader, attach_shader),
    OPENRAC_RSXGL_ENTRY(glLinkProgram, link_program),
    OPENRAC_RSXGL_ENTRY(glDeleteProgram, delete_program),
    OPENRAC_RSXGL_ENTRY(glUseProgram, use_program),
    OPENRAC_RSXGL_ENTRY(glGetShaderiv, get_status),
    OPENRAC_RSXGL_ENTRY(glGetProgramiv, get_status),
    OPENRAC_RSXGL_ENTRY(glGetUniformLocation, uniform_location),
    OPENRAC_RSXGL_ENTRY(glUniform1i, uniform_1i),
    OPENRAC_RSXGL_ENTRY(glUniform1f, uniform_1f),
    OPENRAC_RSXGL_ENTRY(glUniform2f, uniform_2f),
    OPENRAC_RSXGL_ENTRY(glUniform3f, uniform_3f),
    OPENRAC_RSXGL_ENTRY(glUniform4f, uniform_4f),
};
const std::size_t kObjectFunctionCount = std::size(kObjectFunctions);

}  // namespace openrac::rsxgl
