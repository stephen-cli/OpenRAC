// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// A console program that checks the RSX layer (renderer/rsx) by its pictures: it calls the layer's
// GL as the renderer does, draws rectangles of known colours with the direct program, reads the
// pixels back and says which were not what GL would have drawn. README.md says how it is run.
// It ends with status 0 when every check passed.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
RSXGL(glTexParameteri);
RSXGL(glCopyTexSubImage2D);
RSXGL(glGenBuffers);
RSXGL(glBindBuffer);
RSXGL(glBufferData);
RSXGL(glGenVertexArrays);
RSXGL(glBindVertexArray);
RSXGL(glEnableVertexAttribArray);
RSXGL(glVertexAttribPointer);
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

GLuint shader(GLenum type) {
    // The layer finds its program by this line in both stages (renderer/shaders/direct.vert).
    const char* source = "// rsx-program: direct\n";
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
