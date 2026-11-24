// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// A console program that checks the RSX layer (renderer/rsx) by its pictures: it calls the layer's
// GL as the renderer does, draws rectangles of known colours with the direct program, reads the
// pixels back and says which were not what GL would have drawn. README.md says how it is run.
// It ends with status 0 when every check passed.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "renderer/gl.h"
#include "renderer/rsx/rsxgl.h"

using namespace openrac;
using namespace openrac::gl;

namespace {

// The layer's GL function of that name, as gl::load would have it.
#define RSXGL(name) const auto name = reinterpret_cast<PFN_##name>(rsxgl::get_proc(#name))
RSXGL(glGenTextures);
RSXGL(glBindTexture);
RSXGL(glActiveTexture);
RSXGL(glTexImage2D);
RSXGL(glTexSubImage2D);
RSXGL(glTexParameteri);
RSXGL(glCopyTexSubImage2D);
RSXGL(glGenBuffers);
RSXGL(glBindBuffer);
RSXGL(glBufferData);
RSXGL(glGenVertexArrays);
RSXGL(glBindVertexArray);
RSXGL(glEnableVertexAttribArray);
RSXGL(glVertexAttribPointer);
RSXGL(glVertexAttribIPointer);
RSXGL(glVertexAttribDivisor);
RSXGL(glCreateShader);
RSXGL(glShaderSource);
RSXGL(glCreateProgram);
RSXGL(glAttachShader);
RSXGL(glLinkProgram);
RSXGL(glUseProgram);
RSXGL(glGetUniformLocation);
RSXGL(glUniform1i);
RSXGL(glUniform1f);
RSXGL(glUniform2f);
RSXGL(glUniform3f);
RSXGL(glUniform4f);
RSXGL(glUniform4fv);
RSXGL(glUniformMatrix4fv);
RSXGL(glGenFramebuffers);
RSXGL(glBindFramebuffer);
RSXGL(glFramebufferTexture2D);
RSXGL(glBlitFramebuffer);
RSXGL(glViewport);
RSXGL(glScissor);
RSXGL(glEnable);
RSXGL(glDisable);
RSXGL(glBlendFuncSeparate);
RSXGL(glBlendEquation);
RSXGL(glClearColor);
RSXGL(glClear);
RSXGL(glDrawArrays);
RSXGL(glDrawElementsInstanced);
RSXGL(glReadPixels);
#undef RSXGL

// renderer/direct.h's DirectVertex.
struct Vertex {
    float x, y, z;
    float s, t, q;
    std::uint8_t rgba[4];
    float fog;
};

struct Colour {
    std::uint8_t r, g, b, a;
};

constexpr Colour kRed{255, 0, 0, 0x80};
constexpr Colour kGreen{0, 255, 0, 0x20};
constexpr Colour kBlue{0, 0, 255, 0x80};
constexpr Colour kWhite{255, 255, 255, 0x80};
constexpr Colour kGrey{128, 128, 128, 0x80};  // the chip's 1.0: a texture modulated by it is itself

int g_failed = 0;
int g_checked = 0;
GLuint g_program = 0;
GLuint g_buffer = 0;

void uniform_i(const char* name, int value) {
    glUniform1i(glGetUniformLocation(g_program, name), value);
}

void uniform_f(const char* name, float value) {
    glUniform1f(glGetUniformLocation(g_program, name), value);
}

// A rectangle in device coordinates, the texture's whole square over it.
void rectangle(float x0, float y0, float x1, float y1, Colour colour) {
    const auto corner = [&](float x, float y, float s, float t) {
        return Vertex{x, y, 0.0f, s, t, 1.0f, {colour.r, colour.g, colour.b, colour.a}, 1.0f};
    };
    const Vertex vertices[6] = {
        corner(x0, y0, 0.0f, 0.0f),
        corner(x1, y0, 1.0f, 0.0f),
        corner(x1, y1, 1.0f, 1.0f),
        corner(x0, y0, 0.0f, 0.0f),
        corner(x1, y1, 1.0f, 1.0f),
        corner(x0, y1, 0.0f, 1.0f),
    };
    glBindBuffer(GL_ARRAY_BUFFER, g_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// The pixel of the read framebuffer at (x, y), GL's way up, against what GL would have there.
void expect(const char* what, GLuint framebuffer, int x, int y, Colour wanted) {
    std::uint8_t got[4] = {};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    const auto near = [](int a, int b) {
        return std::abs(a - b) <= 3;
    };
    const bool passed = near(got[0], wanted.r) && near(got[1], wanted.g) && near(got[2], wanted.b);
    ++g_checked;
    g_failed += passed ? 0 : 1;
    std::printf(
        "%s %s at %d,%d: %d %d %d, wanted %d %d %d\n",
        passed ? "ok  " : "FAIL",
        what,
        x,
        y,
        got[0],
        got[1],
        got[2],
        wanted.r,
        wanted.g,
        wanted.b
    );
}

// The four quarters of a picture `wide` by `high`: bottom left, bottom right, top left, top right.
void expect_quarters(
    const char* what,
    GLuint framebuffer,
    int wide,
    int high,
    Colour bl,
    Colour br,
    Colour tl,
    Colour tr
) {
    const std::string name(what);
    expect((name + ", bottom left").c_str(), framebuffer, wide / 4, high / 4, bl);
    expect((name + ", bottom right").c_str(), framebuffer, wide * 3 / 4, high / 4, br);
    expect((name + ", top left").c_str(), framebuffer, wide / 4, high * 3 / 4, tl);
    expect((name + ", top right").c_str(), framebuffer, wide * 3 / 4, high * 3 / 4, tr);
}

// The layer finds its program by a line in both stages (renderer/shaders/direct.vert).
GLuint shader(GLenum type, const char* source = "// rsx-program: direct\n") {
    const GLuint made = glCreateShader(type);
    glShaderSource(made, 1, &source, nullptr);
    return made;
}

void texture_parameters() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
}

// The direct program's uniforms for a draw: untextured, or the texture as it is (decal) with its
// alpha, or modulated by the vertex colour.
enum class Mode {
    Colour,
    Decal,
    Modulate
};

void set_mode(Mode mode) {
    uniform_i("textured", mode == Mode::Colour ? 0 : 1);
    uniform_i("tcc", 1);
    uniform_i("tfx", mode == Mode::Decal ? 1 : 0);
    uniform_f("tex_alpha_scale", 1.0f);
    uniform_i("fog_enable", 0);
    glUniform3f(glGetUniformLocation(g_program, "fog_colour"), 0.0f, 0.0f, 0.0f);
    uniform_i("alpha_test", 0);
    uniform_f("alpha_ref", 0.0f);
    uniform_i("alpha_keep_failing", 0);
    uniform_i("source_scale", 0);
    uniform_f("source_fix", 0.0f);
    glUniform2f(glGetUniformLocation(g_program, "pixel_offset"), 0.0f, 0.0f);
}

void clear(float r, float g, float b) {
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// The sky program (viewer/shaders/sky.vert): a square of four vertices and six indices, moved by
// the shell's matrix and then halved by the view's, so it covers the upper right quarter of the
// screen and nothing else; the texel (the chip's 1.0, half of full) times a vertex colour above
// one, times the tint.
void sky_checks(int wide, int high) {
    struct SkyVertex {
        float position[3];
        float uv[2];
        float colour[4];
    };

    const SkyVertex vertices[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.5f, 0.5f}, {2.0f, 1.0f, 0.5f, 1.0f}},
        {{1.0f, -1.0f, 0.0f}, {0.5f, 0.5f}, {2.0f, 1.0f, 0.5f, 1.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.5f, 0.5f}, {2.0f, 1.0f, 0.5f, 1.0f}},
        {{-1.0f, 1.0f, 0.0f}, {0.5f, 0.5f}, {2.0f, 1.0f, 0.5f, 1.0f}},
    };
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    const GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, "// rsx-program: sky\n"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, "// rsx-program: sky\n"));
    glLinkProgram(program);
    glUseProgram(program);
    GLuint vertex_array = 0;
    GLuint buffers[2] = {};
    glGenVertexArrays(1, &vertex_array);
    glGenBuffers(2, buffers);
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    constexpr auto stride = static_cast<GLsizei>(sizeof(SkyVertex));
    const auto at = [](std::size_t offset) {
        return reinterpret_cast<const void*>(offset);
    };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(SkyVertex, position)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(SkyVertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(SkyVertex, colour)));

    GLuint grey = 0;
    glGenTextures(1, &grey);
    glBindTexture(GL_TEXTURE_2D, grey);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kGrey
    );
    texture_parameters();

    // Columns, as GL has a matrix.
    const float halved[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float moved[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(program, "view_projection"), 1, GL_FALSE, halved);
    glUniformMatrix4fv(glGetUniformLocation(program, "shell"), 1, GL_FALSE, moved);
    glUniform3f(glGetUniformLocation(program, "sky_tint"), 1.0f, 0.5f, 1.0f);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    clear(0.0f, 0.0f, 1.0f);
    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, at(0), 1);
    // 2 x 128/255, 1 x 0.5 x 128/255, 0.5 x 128/255.
    expect("sky: indexed, two matrices", 0, wide * 5 / 8, high * 5 / 8, Colour{255, 64, 64, 255});
    expect("sky: within the quarter", 0, wide * 7 / 8, high * 7 / 8, Colour{255, 64, 64, 255});
    expect("sky: nothing left of it", 0, wide * 3 / 8, high * 5 / 8, kBlue);
    expect("sky: nothing below it", 0, wide * 5 / 8, high * 3 / 8, kBlue);
    glUseProgram(g_program);
}

// The mesh program (viewer/shaders/mesh.vert): one square, drawn as two instances, each with a
// matrix and a tint of its own from a buffer GL reads once an instance. The texel is the chip's
// 1.0 (half of full) with an alpha below a cutout's, and the material's colour doubles it.
void mesh_checks(int wide, int high) {
    struct MeshVertex {
        float position[3];
        float normal[3];
        float uv[2];
        float colour[4];
    };

    // scene/level_renderer.h's InstanceData, as far as the program reads it.
    struct Instance {
        float matrix[16];
        float tint[4];
    };

    const auto corner = [](float x, float y) {
        return MeshVertex{{x, y, 0.0f}, {0.0f, 0.0f, 2.0f}, {0.5f, 0.5f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    };
    const MeshVertex vertices[4] =
        {corner(-1.0f, -1.0f), corner(1.0f, -1.0f), corner(1.0f, 1.0f), corner(-1.0f, 1.0f)};
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    // Halved, to the lower left quarter in red and to the upper right in green.
    const Instance instances[2] = {
        {{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, -0.5f, -0.5f, 0, 1}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1}, {0.0f, 1.0f, 0.0f, 1.0f}},
    };
    const GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, "// rsx-program: mesh\n"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, "// rsx-program: mesh\n"));
    glLinkProgram(program);
    glUseProgram(program);
    GLuint vertex_array = 0;
    GLuint buffers[3] = {};
    glGenVertexArrays(1, &vertex_array);
    glGenBuffers(3, buffers);
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    constexpr auto stride = static_cast<GLsizei>(sizeof(MeshVertex));
    const auto at = [](std::size_t offset) {
        return reinterpret_cast<const void*>(offset);
    };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(MeshVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(MeshVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(MeshVertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(MeshVertex, colour)));
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    glBufferData(GL_ARRAY_BUFFER, sizeof instances, instances, GL_STATIC_DRAW);
    constexpr auto instance_stride = static_cast<GLsizei>(sizeof(Instance));
    for (GLuint column = 0; column < 4; ++column) {
        glEnableVertexAttribArray(4 + column);
        glVertexAttribPointer(
            4 + column, 4, GL_FLOAT, GL_FALSE, instance_stride, at(column * 4 * sizeof(float))
        );
        glVertexAttribDivisor(4 + column, 1);
    }
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, instance_stride, at(offsetof(Instance, tint)));
    glVertexAttribDivisor(8, 1);

    GLuint grey = 0;
    glGenTextures(1, &grey);
    glBindTexture(GL_TEXTURE_2D, grey);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kGrey
    );
    texture_parameters();

    const auto uniform = [program](const char* name) {
        return glGetUniformLocation(program, name);
    };
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(uniform("view_projection"), 1, GL_FALSE, identity);
    glUniformMatrix4fv(uniform("view"), 1, GL_FALSE, identity);
    glUniform4f(uniform("material_colour"), 2.0f, 2.0f, 2.0f, 1.0f);
    glUniform4f(uniform("fog_colour"), 0.0f, 0.0f, 1.0f, 0.0f);
    glUniform4f(uniform("fog_params"), 0.0f, 128.0f, 0.0f, 255.0f);
    const auto draw = [&] {
        clear(0.0f, 0.0f, 0.0f);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, at(0), 2);
    };
    const Colour black{0, 0, 0, 255};

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    draw();
    expect_quarters(
        "mesh: two instances", 0, wide, high, kRed, black, black, Colour{0, 255, 0, 255}
    );
    // A frame of a level: more commands than the command buffer holds, before any flip.
    clear(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 10000; ++i) {
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, at(0), 2);
    }
    expect_quarters(
        "mesh: 20000 draws a frame", 0, wide, high, kRed, black, black, Colour{0, 255, 0, 255}
    );
    // The viewer's sun on a normal straight up, of any length: 0.55 + 0.45 x 0.80.
    glUniform1i(uniform("lighting"), 1);
    draw();
    expect("mesh: the viewer's sun", 0, wide / 4, high / 4, Colour{232, 0, 0, 255});
    glUniform1i(uniform("lighting"), 0);
    // The chip's fog, 128 of 255 of the colour kept.
    glUniform4f(uniform("fog_colour"), 0.0f, 0.0f, 1.0f, 1.0f);
    draw();
    expect("mesh: fog", 0, wide / 4, high / 4, Colour{128, 0, 127, 255});
    glUniform4f(uniform("fog_colour"), 0.0f, 0.0f, 1.0f, 0.0f);
    // A cutout's part-transparent texel: left out of the solid pass, drawn by the translucent
    // one; a material without a cutout has nothing in the translucent pass.
    glUniform1i(uniform("cutout"), 1);
    draw();
    expect("mesh: cutout, solid pass", 0, wide / 4, high / 4, black);
    glUniform1i(uniform("translucent_pass"), 1);
    draw();
    expect("mesh: cutout, translucent pass", 0, wide / 4, high / 4, kRed);
    glUniform1i(uniform("cutout"), 0);
    draw();
    expect("mesh: no cutout, translucent pass", 0, wide / 4, high / 4, black);
    glUniform1i(uniform("translucent_pass"), 0);
    glUseProgram(g_program);
}

// The mesh program with `skinning` set, the live mobys' (programs/moby.vp), in the mobys' passes
// (scene/moby_passes.h): a square over the picture's left half, as one instance with the
// renderer's whole vertex and instance.
void moby_checks(int wide, int high) {
    // scene/level.h's Vertex.
    struct MobyVertex {
        float position[3];
        float normal[3];
        float uv[2];
        float colour[4];
        std::uint8_t joints[4];
        float weights[4];
        float light_slot;
    };

    // scene/level_renderer.h's InstanceData.
    struct Instance {
        float matrix[16];
        float tint[4];
        float palette;
        float lights;
        float moby_light[4];
        float moby_ambient[4];
    };

    // Half of joint 2 and half of joint 3 of the instance's palette.
    const auto corner = [](float x, float y, float slot) {
        return MobyVertex{
            {x, y, 0.0f},
            {0.0f, 0.0f, 1.0f},
            {0.5f, 0.5f},
            {1.0f, 1.0f, 1.0f, 1.0f},
            {2, 3, 0, 0},
            {0.5f, 0.5f, 0.0f, 0.0f},
            slot
        };
    };
    const auto square = [&corner](float slot) {
        return std::array<MobyVertex, 4>{
            corner(-1.0f, -1.0f, slot),
            corner(0.0f, -1.0f, slot),
            corner(0.0f, 1.0f, slot),
            corner(-1.0f, 1.0f, slot)
        };
    };
    const std::array<MobyVertex, 4> vertices = square(-1.0f);
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    Instance instance{
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
        {1.0f, 0.0f, 0.0f, 1.0f},
        1.0f,
        -1.0f,
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f}
    };
    const GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, "// rsx-program: mesh\n"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, "// rsx-program: mesh\n"));
    glLinkProgram(program);
    glUseProgram(program);
    GLuint vertex_array = 0;
    GLuint buffers[3] = {};
    glGenVertexArrays(1, &vertex_array);
    glGenBuffers(3, buffers);
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices.data(), GL_STATIC_DRAW);
    constexpr auto stride = static_cast<GLsizei>(sizeof(MobyVertex));
    const auto at = [](std::size_t offset) {
        return reinterpret_cast<const void*>(offset);
    };
    const auto floats = [&](GLuint index, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride, at(offset));
    };
    floats(0, 3, offsetof(MobyVertex, position));
    floats(1, 3, offsetof(MobyVertex, normal));
    floats(2, 2, offsetof(MobyVertex, uv));
    floats(3, 4, offsetof(MobyVertex, colour));
    glEnableVertexAttribArray(10);
    glVertexAttribIPointer(10, 4, GL_UNSIGNED_BYTE, stride, at(offsetof(MobyVertex, joints)));
    floats(11, 4, offsetof(MobyVertex, weights));
    floats(12, 1, offsetof(MobyVertex, light_slot));
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    glBufferData(GL_ARRAY_BUFFER, sizeof instance, &instance, GL_DYNAMIC_DRAW);
    constexpr auto instance_stride = static_cast<GLsizei>(sizeof(Instance));
    const auto each = [&](GLuint index, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, instance_stride, at(offset));
        glVertexAttribDivisor(index, 1);
    };
    for (GLuint column = 0; column < 4; ++column) {
        each(4 + column, 4, column * 4 * sizeof(float));
    }
    each(8, 4, offsetof(Instance, tint));
    each(9, 1, offsetof(Instance, palette));
    each(13, 1, offsetof(Instance, lights));
    each(14, 4, offsetof(Instance, moby_light));
    each(15, 4, offsetof(Instance, moby_ambient));

    // The palette texture, on the renderer's unit: four texels (columns) a matrix. The
    // instance's palette starts at matrix 1: its joint 2 moves two to the right, its joint 3
    // nowhere, and half of each is one to the right.
    std::array<float, 16 * 8> palette{};
    const auto matrix = [&palette](std::size_t index, float x) {
        const float columns[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
        std::copy(std::begin(columns), std::end(columns), palette.begin() + index * 16);
    };
    matrix(3, 2.0f);
    matrix(4, 0.0f);
    GLuint joints = 0;
    glGenTextures(1, &joints);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, joints);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA32F), 32, 1, 0, GL_RGBA, GL_FLOAT, nullptr
    );
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 1, GL_RGBA, GL_FLOAT, palette.data());
    glActiveTexture(GL_TEXTURE0);
    GLuint grey = 0;
    glGenTextures(1, &grey);
    glBindTexture(GL_TEXTURE_2D, grey);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kGrey
    );
    texture_parameters();

    const auto uniform = [program](const char* name) {
        return glGetUniformLocation(program, name);
    };
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(uniform("view_projection"), 1, GL_FALSE, identity);
    glUniformMatrix4fv(uniform("view"), 1, GL_FALSE, identity);
    glUniform4f(uniform("material_colour"), 2.0f, 2.0f, 2.0f, 1.0f);
    glUniform4f(uniform("fog_colour"), 0.0f, 0.0f, 1.0f, 0.0f);
    glUniform4f(uniform("fog_params"), 0.0f, 128.0f, 0.0f, 255.0f);
    glUniform1i(uniform("skinning"), 1);
    // Light set 1: a red light the square faces, a green one behind it that leaks half through.
    const float sets[8][4] = {
        {0, 0, 0, 0},
        {0, 0, -1, 0},
        {0, 0, 0, 0},
        {0, 0, 1, 0},
        {0.25f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.5f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    glUniform4fv(uniform("light_sets"), 8, &sets[0][0]);
    const auto draw = [&] {
        glBufferData(GL_ARRAY_BUFFER, sizeof instance, &instance, GL_DYNAMIC_DRAW);
        clear(0.0f, 0.0f, 0.0f);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, at(0), 1);
    };
    const Colour black{0, 0, 0, 255};

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    draw();
    expect_quarters("moby: two joints' skin", 0, wide, high, black, kRed, black, kRed);
    instance.palette = -1.0f;
    draw();
    expect_quarters("moby: no palette, as stored", 0, wide, high, kRed, black, kRed, black);
    // Ambient 0.5, the red light full on, the green one's half leaking through from behind.
    instance.tint[1] = instance.tint[2] = 1.0f;
    instance.moby_light[0] = 1.0f;
    instance.moby_light[3] = 1.0f;
    instance.moby_ambient[0] = instance.moby_ambient[1] = instance.moby_ambient[2] = 0.5f;
    draw();
    expect("moby: MobyProc's lights", 0, wide / 4, high / 4, Colour{192, 192, 128, 255});
    // Cross-faded half way from set 0, which has the directions and no colour: half the red,
    // and of the green half the colour through half the leak.
    instance.moby_light[0] = 0.0f;
    instance.moby_light[1] = 1.0f;
    instance.moby_light[2] = 0.5f;
    draw();
    expect("moby: two light sets cross-faded", 0, wide / 4, high / 4, Colour{160, 144, 128, 255});
    // A glow packet's vertices (slot -2) of a moby with a glow colour are the glow pass's: the
    // solid pass leaves them out. Without a glow colour they are drawn as any vertex.
    const std::array<MobyVertex, 4> glowing = square(-2.0f);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof glowing, glowing.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    draw();
    expect(
        "moby: glow vertices, no glow colour", 0, wide / 4, high / 4, Colour{160, 144, 128, 255}
    );
    instance.moby_ambient[3] = 1.0f + 0x0000FF;
    draw();
    expect("moby: glow vertices left out of the solid pass", 0, wide / 4, high / 4, black);

    // The glow pass draws them, in the glow colour (red, the chip's 0xFF) and unlit.
    const auto at_left = [&](const char* what, Colour wanted) {
        expect(what, 0, wide / 4, high / 4, wanted);
    };
    glUniform1i(uniform("glow_pass"), 1);
    draw();
    at_left("moby: glow pass", kRed);
    // In two draws split at the alpha 0x60: the texel's is 0x80 of 0xFF, below it.
    glUniform1i(uniform("glow_split"), 1);
    draw();
    at_left("moby: glow pass, the texels from the reference", black);
    glUniform1i(uniform("glow_split"), 2);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    draw();
    at_left("moby: glow pass, the texels below it, blended", Colour{128, 0, 0, 255});
    // The other faces are not the glow pass's.
    glUniform1i(uniform("glow_split"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
    draw();
    at_left("moby: glow pass, no glow vertices", black);
    glUniform1i(uniform("glow_pass"), 0);

    // A blended moby: all of it, lit, at the texel's alpha times its own.
    glUniform1i(uniform("effect_pass"), 1);
    glUniform1f(uniform("effect_reference"), 96.0f / 128.0f);
    draw();
    at_left("moby: effect pass, blended", Colour{80, 72, 64, 255});
    glUniform1i(uniform("glow_split"), 1);
    draw();
    at_left("moby: effect pass, the texels from the reference", black);
    glUniform1i(uniform("glow_split"), 0);
    // An added one: its colour times its alpha, which goes past 1 as the chip's does.
    glUniform1i(uniform("effect_additive"), 1);
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    instance.tint[3] = 3.0f;
    draw();
    at_left("moby: effect pass, added", Colour{241, 217, 193, 255});
    instance.tint[3] = 1.0f;
    glUniform1i(uniform("effect_additive"), 0);
    glUniform1i(uniform("effect_pass"), 0);

    // The metal packets: a sphere map read by the normal as the camera sees it (to the right
    // of the picture's middle, then to the left), times the chip's 0x70, not lit and not the
    // material's colour; its alpha the shine the moby's distance gives.
    const Colour sphere[2] = {{255, 0, 0, 255}, {0, 255, 0, 255}};
    GLuint metal = 0;
    glGenTextures(1, &metal);
    glBindTexture(GL_TEXTURE_2D, metal);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, sphere
    );
    texture_parameters();
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUniform1i(uniform("metal_pass"), 1);
    const float right[16] = {0.8f, 0, -0.6f, 0, 0, 1, 0, 0, 0.6f, 0, 0.8f, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(uniform("view"), 1, GL_FALSE, right);
    draw();
    at_left("moby: metal pass, the sphere map's right", Colour{0, 223, 0, 255});
    const float left[16] = {0.8f, 0, 0.6f, 0, 0, 1, 0, 0, -0.6f, 0, 0.8f, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(uniform("view"), 1, GL_FALSE, left);
    draw();
    at_left("moby: metal pass, the sphere map's left", Colour{223, 0, 0, 255});
    // A shine distance of 8 units: half the shine at the camera.
    instance.lights = -9.0f;
    draw();
    at_left("moby: metal pass, half the shine", Colour{112, 0, 0, 255});
    glUniform1i(uniform("metal_pass"), 0);
    glDisable(GL_BLEND);
    glUseProgram(g_program);
}

// The checks. Within the namespace, so that the names above are the layer's functions and not
// renderer/gl.h's pointers of the same names.
int run() {
    std::string error;
    if (!rsxgl::open(rsxgl::Config{}, error)) {
        std::printf("FAIL the RSX: %s\n", error.c_str());
        return 2;
    }
    const int wide = rsxgl::width();
    const int high = rsxgl::height();
    std::printf("rsxgl-test: %s\n", rsxgl::device_name().c_str());

    g_program = glCreateProgram();
    glAttachShader(g_program, shader(GL_VERTEX_SHADER));
    glAttachShader(g_program, shader(GL_FRAGMENT_SHADER));
    glLinkProgram(g_program);
    glUseProgram(g_program);

    GLuint vertex_array = 0;
    glGenVertexArrays(1, &vertex_array);
    glGenBuffers(1, &g_buffer);
    glBindVertexArray(vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, g_buffer);
    constexpr auto stride = static_cast<GLsizei>(sizeof(Vertex));
    const auto at = [](std::size_t offset) {
        return reinterpret_cast<const void*>(offset);
    };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex, s)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(Vertex, rgba)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, at(offsetof(Vertex, fog)));

    // Two by two texels: red and green in the first row (t = 0), blue and white in the second.
    const Colour texels[4] = {kRed, kGreen, kBlue, kWhite};
    GLuint squares = 0;
    glGenTextures(1, &squares);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, squares);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels
    );
    texture_parameters();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, wide, high);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    // The clear, and the scissor within it.
    const Colour cleared{64, 128, 191, 255};
    clear(0.25f, 0.5f, 0.75f);
    expect("clear", 0, wide / 2, high / 2, cleared);
    clear(0.25f, 0.5f, 0.75f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, wide / 2, high / 2);
    clear(0.0f, 0.0f, 1.0f);
    glDisable(GL_SCISSOR_TEST);
    expect_quarters("scissored clear", 0, wide, high, kBlue, cleared, cleared, cleared);

    // The scissor around a draw: the top right quarter of a rectangle over everything.
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Colour);
    glEnable(GL_SCISSOR_TEST);
    glScissor(wide / 2, high / 2, wide / 2, high / 2);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kRed);
    glDisable(GL_SCISSOR_TEST);
    expect_quarters("scissored draw", 0, wide, high, cleared, cleared, cleared, kRed);

    // Each of the fragment program's inputs by itself. The blend's weight of 0.5 as the colour
    // (source_scale 4): the picture is grey whatever the vertex and the texture are.
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Colour);
    uniform_i("source_scale", 4);
    uniform_f("source_fix", 0.5f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kRed);
    expect("constant weight", 0, wide / 2, high / 2, Colour{128, 128, 128, 255});
    // All fog (the vertices' fog is 1, none of it; a weight of 0 on it would be all): the fog's
    // colour through the vertex colour's place.
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Colour);
    uniform_i("fog_enable", 1);
    glUniform3f(glGetUniformLocation(g_program, "fog_colour"), 10.0f, 200.0f, 30.0f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kRed);
    expect("no fog on a fogged draw", 0, wide / 2, high / 2, Colour{255, 0, 0, 255});

    // The vertex colour alone, in the bottom left quarter: which way is up, the bytes' order.
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Colour);
    const Colour orange{200, 100, 50, 0x80};
    rectangle(-1.0f, -1.0f, 0.0f, 0.0f, orange);
    expect_quarters("vertex colour", 0, wide, high, orange, cleared, cleared, cleared);

    // The texture as it is, then modulated by the chip's 1.0.
    set_mode(Mode::Decal);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
    expect_quarters("texture", 0, wide, high, kRed, kGreen, kBlue, kWhite);
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Modulate);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kGrey);
    expect_quarters("texture modulated", 0, wide, high, kRed, kGreen, kBlue, kWhite);

    // The alpha test: greater than 0x40 keeps all but the green texel (alpha 0x20).
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Decal);
    uniform_i("alpha_test", 6);
    uniform_f("alpha_ref", 64.0f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
    expect_quarters("alpha test", 0, wide, high, kRed, cleared, kBlue, kWhite);
    // And the pass that keeps what the test fails.
    clear(0.25f, 0.5f, 0.75f);
    uniform_i("alpha_keep_failing", 1);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
    expect_quarters("alpha test's failures", 0, wide, high, cleared, kGreen, cleared, cleared);

    // The vertex colour's alpha, through the alpha test: 0x40 is greater than 0x30 and not than 0x50.
    clear(0.25f, 0.5f, 0.75f);
    set_mode(Mode::Colour);
    uniform_i("alpha_test", 6);
    uniform_f("alpha_ref", 48.0f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, Colour{255, 255, 255, 0x40});
    expect("vertex alpha above the reference", 0, wide / 2, high / 2, kWhite);
    clear(0.25f, 0.5f, 0.75f);
    uniform_f("alpha_ref", 80.0f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, Colour{255, 255, 255, 0x40});
    expect("vertex alpha below the reference", 0, wide / 2, high / 2, cleared);

    // Blending. One and one: a quarter grey added to a quarter grey.
    clear(0.25f, 0.25f, 0.25f);
    set_mode(Mode::Colour);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, Colour{64, 64, 64, 0x80});
    glDisable(GL_BLEND);
    expect("blend, added", 0, wide / 2, high / 2, Colour{128, 128, 128, 255});

    // White of alpha 0x40, a half, over black.
    clear(0.0f, 0.0f, 0.0f);
    set_mode(Mode::Colour);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, Colour{255, 255, 255, 0x40});
    glDisable(GL_BLEND);
    expect("blend", 0, wide / 2, high / 2, Colour{128, 128, 128, 255});

    // A framebuffer of its own: green, with red drawn into its bottom left quarter.
    constexpr int side = 64;
    GLuint picture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &picture);
    glBindTexture(GL_TEXTURE_2D, picture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        static_cast<GLint>(GL_RGBA8),
        side,
        side,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );
    texture_parameters();
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, picture, 0);
    glViewport(0, 0, side, side);
    clear(0.0f, 1.0f, 0.0f);
    set_mode(Mode::Colour);
    rectangle(-1.0f, -1.0f, 0.0f, 0.0f, kRed);
    const Colour green{0, 255, 0, 255};
    expect_quarters("framebuffer", framebuffer, side, side, kRed, green, green, green);

    // Copied to the screen, sampled as a texture, and copied into another texture.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, wide, high);
    clear(0.0f, 0.0f, 0.0f);
    glBlitFramebuffer(0, 0, side, side, 0, 0, wide, high, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    expect_quarters("blit to the screen", 0, wide, high, kRed, green, green, green);

    clear(0.0f, 0.0f, 0.0f);
    set_mode(Mode::Decal);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
    expect_quarters("framebuffer as a texture", 0, wide, high, kRed, green, green, green);

    GLuint copied = 0;
    glGenTextures(1, &copied);
    glBindTexture(GL_TEXTURE_2D, copied);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        static_cast<GLint>(GL_RGBA8),
        side,
        side,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );
    texture_parameters();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, side, side);
    clear(0.0f, 0.0f, 0.0f);
    rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
    expect_quarters("copied texture", 0, wide, high, kRed, green, green, green);

    sky_checks(wide, high);
    mesh_checks(wide, high);
    moby_checks(wide, high);
    glBindVertexArray(vertex_array);

    // Many frames: the command buffer wraps and the flips keep coming.
    for (int frame = 0; frame < 300; ++frame) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        clear(0.0f, 0.0f, 0.0f);
        glBindTexture(GL_TEXTURE_2D, squares);
        set_mode(Mode::Decal);
        for (int i = 0; i < 100; ++i) {
            rectangle(-1.0f, -1.0f, 1.0f, 1.0f, kWhite);
        }
        if (frame == 299) {
            expect_quarters("frame 300", 0, wide, high, kRed, kGreen, kBlue, kWhite);
        }
        rsxgl::present();
        if (frame % 50 == 0) {
            std::printf("frame %d shown\n", frame);
        }
    }

    rsxgl::close();
    std::printf("rsxgl-test: %d of %d checks failed\n", g_failed, g_checked);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace

int main() {
    return run();
}
