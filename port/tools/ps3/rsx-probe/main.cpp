// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The first picture from the console: the RSX set up through PSL1GHT, a clear and one triangle
// shaded by two assembled programs (cgcomp -a, no Cg toolkit), then the colour buffer read back
// from the RSX's memory and written beside the program as frame.ppm.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include "triangle_fpo.h"
#include "triangle_vpo.h"

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

namespace {

constexpr u32 kCommandBuffer = 0x80000;
constexpr u32 kHostSize = 32 * 1024 * 1024;
constexpr u32 kLabel = 255;

gcmContextData* context = nullptr;
videoResolution resolution;
u32 colour_pitch, colour_offset[2], depth_pitch, depth_offset;
u32* colour_buffer[2];
u32 label_value = 1;

struct Vertex {
    float position[4];
    float colour[4];
};

// Waits until the RSX has run every command sent so far.
void wait_idle() {
    rsxSetWriteBackendLabel(context, kLabel, label_value);
    rsxFlushBuffer(context);
    while (*(vu32*)gcmGetLabelAddress(kLabel) != label_value) {
        usleep(100);
    }
    ++label_value;
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
    surface.colorOffset[0] = colour_offset[index];
    surface.colorPitch[0] = colour_pitch;
    surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    surface.depthLocation = GCM_LOCATION_RSX;
    surface.depthOffset = depth_offset;
    surface.depthPitch = depth_pitch;
    surface.type = GCM_SURFACE_TYPE_LINEAR;
    surface.antiAlias = GCM_SURFACE_CENTER_1;
    surface.width = resolution.width;
    surface.height = resolution.height;
    rsxSetSurface(context, &surface);
}

}  // namespace

int main() {
    void* host = memalign(1024 * 1024, kHostSize);
    rsxInit(&context, kCommandBuffer, kHostSize, host);

    videoState state;
    if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0
        || videoGetResolution(state.displayMode.resolution, &resolution) != 0) {
        std::printf("rsx-probe: no video state\n");
        return 1;
    }
    videoConfiguration config;
    std::memset(&config, 0, sizeof config);
    config.resolution = state.displayMode.resolution;
    config.format = VIDEO_BUFFER_FORMAT_XRGB;
    config.aspect = VIDEO_ASPECT_AUTO;
    config.pitch = resolution.width * 4;
    if (videoConfigure(VIDEO_PRIMARY, &config, nullptr, 0) != 0) {
        std::printf("rsx-probe: videoConfigure failed\n");
        return 1;
    }
    std::printf("rsx-probe: %ux%u\n", (unsigned)resolution.width, (unsigned)resolution.height);
    wait_idle();
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    colour_pitch = resolution.width * 4;
    depth_pitch = resolution.width * 4;
    for (u32 i = 0; i < 2; ++i) {
        colour_buffer[i] = (u32*)rsxMemalign(64, resolution.height * colour_pitch);
        rsxAddressToOffset(colour_buffer[i], &colour_offset[i]);
        gcmSetDisplayBuffer(i, colour_offset[i], colour_pitch, resolution.width, resolution.height);
    }
    void* depth = rsxMemalign(64, resolution.height * depth_pitch);
    rsxAddressToOffset(depth, &depth_offset);

    // The programs: the vertex program's code goes in the command buffer, the fragment program's
    // is read by the RSX from its own memory.
    auto* vertex_program = (rsxVertexProgram*)triangle_vpo;
    auto* fragment_program = (rsxFragmentProgram*)triangle_fpo;
    void* vertex_code;
    void* fragment_code;
    u32 vertex_size, fragment_size, fragment_offset;
    rsxVertexProgramGetUCode(vertex_program, &vertex_code, &vertex_size);
    rsxFragmentProgramGetUCode(fragment_program, &fragment_code, &fragment_size);
    void* fragment_memory = rsxMemalign(64, fragment_size);
    std::memcpy(fragment_memory, fragment_code, fragment_size);
    rsxAddressToOffset(fragment_memory, &fragment_offset);
    std::printf(
        "rsx-probe: programs %u and %u bytes\n", (unsigned)vertex_size, (unsigned)fragment_size
    );

    auto* vertices = (Vertex*)rsxMemalign(128, 3 * sizeof(Vertex));
    const Vertex triangle[3] = {
        {{0.0f, 0.8f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.8f, -0.8f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.8f, -0.8f, 0.5f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    };
    std::memcpy(vertices, triangle, sizeof triangle);
    u32 position_offset, colour_attribute_offset;
    rsxAddressToOffset(&vertices[0].position, &position_offset);
    rsxAddressToOffset(&vertices[0].colour, &colour_attribute_offset);

    const float width = (float)resolution.width, height = (float)resolution.height;
    float scale[4] = {width * 0.5f, height * -0.5f, 0.5f, 0.0f};
    float offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};

    u32 target = 0;
    for (int frame = 0; frame < 4; ++frame) {
        set_target(target);
        rsxSetColorMask(
            context, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A
        );
        rsxSetColorMaskMrt(context, 0);
        rsxSetViewport(
            context, 0, 0, resolution.width, resolution.height, 0.0f, 1.0f, scale, offset
        );
        rsxSetScissor(context, 0, 0, resolution.width, resolution.height);
        rsxSetDepthTestEnable(context, GCM_FALSE);
        rsxSetCullFaceEnable(context, GCM_FALSE);
        rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
        rsxSetClearColor(context, 0xFF203040);
        rsxSetClearDepthStencil(context, 0xFFFFFF00);
        rsxClearSurface(
            context,
            GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S
        );

        rsxBindVertexArrayAttrib(
            context,
            GCM_VERTEX_ATTRIB_POS,
            0,
            position_offset,
            sizeof(Vertex),
            4,
            GCM_VERTEX_DATA_TYPE_F32,
            GCM_LOCATION_RSX
        );
        rsxBindVertexArrayAttrib(
            context,
            GCM_VERTEX_ATTRIB_COLOR0,
            0,
            colour_attribute_offset,
            sizeof(Vertex),
            4,
            GCM_VERTEX_DATA_TYPE_F32,
            GCM_LOCATION_RSX
        );
        rsxLoadVertexProgram(context, vertex_program, vertex_code);
        rsxLoadFragmentProgramLocation(
            context, fragment_program, fragment_offset, GCM_LOCATION_RSX
        );
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLES, 0, 3);

        wait_idle();
        gcmResetFlipStatus();
        gcmSetFlip(context, target);
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
        while (gcmGetFlipStatus() != 0) {
            usleep(200);
        }
        if (frame == 3) {
            break;  // the last drawn buffer is read below
        }
        target ^= 1;
    }
    wait_idle();

    FILE* file = std::fopen("/app_home/frame.ppm", "wb");
    if (file == nullptr) {
        std::printf("rsx-probe: cannot write frame.ppm\n");
        return 1;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", (unsigned)resolution.width, (unsigned)resolution.height);
    const u32* pixels = colour_buffer[target];
    for (u32 i = 0; i < (u32)resolution.width * resolution.height; ++i) {
        const u32 pixel = pixels[i];  // X8R8G8B8, read as the console's word
        const unsigned char rgb[3] =
            {(unsigned char)(pixel >> 16), (unsigned char)(pixel >> 8), (unsigned char)pixel};
        std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
    std::printf(
        "rsx-probe: frame.ppm written, centre pixel %08x, corner %08x\n",
        (unsigned)pixels[(resolution.height / 2) * resolution.width + resolution.width / 2],
        (unsigned)pixels[0]
    );
    return 0;
}
