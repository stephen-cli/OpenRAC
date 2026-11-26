// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors

// SDL's audio driver for the PlayStation 3, which SDL has none for: the console's audio port
// (PSL1GHT's audio library) as SDL's one playback device. sdl3-lv2.py copies this file into the
// SDL the port fetched (src/audio/lv2) and names the driver to SDL's build and its list of
// drivers.
//
// The port is a ring of blocks of 256 frames of 32-bit floats at 48 kHz, which the system plays
// one after the other, with an event at each block it starts. The driver waits for the event and
// writes the block after the one being played.

#include "SDL_internal.h"

#ifdef __lv2ppu__

#include "../SDL_audiodev_c.h"
#include "../SDL_sysaudio.h"

#include <audio/audio.h>
#include <sys/event_queue.h>

#define LV2AUDIO_CHANNELS 2
#define LV2AUDIO_RATE     48000
// How long the device's thread waits for the port's event before it looks again, in microseconds:
// a port that sends none (the system holding the sound back) must not hold the thread for ever.
#define LV2AUDIO_WAIT_USEC (20 * 1000)

struct SDL_PrivateAudioData
{
    u32 port;
    bool port_open;
    sys_event_queue_t queue;
    sys_ipc_key_t queue_key;
    bool queue_set;
    audioPortConfig config;
    // The block SDL mixes into, copied into the port's ring when it is whole.
    float mix[LV2AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES];
};

static void LV2AUDIO_CloseDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    if (!hidden) {
        return;
    }
    if (hidden->port_open) {
        audioPortStop(hidden->port);
    }
    if (hidden->queue_set) {
        audioRemoveNotifyEventQueue(hidden->queue_key);
        sysEventQueueDestroy(hidden->queue, 0);
    }
    if (hidden->port_open) {
        audioPortClose(hidden->port);
    }
    audioQuit();
    SDL_free(hidden);
    device->hidden = NULL;
}

static bool LV2AUDIO_OpenDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*hidden));
    if (!hidden) {
        return false;
    }
    device->hidden = hidden;

    audioInit();

    audioPortParam param;
    SDL_zero(param);
    param.numChannels = AUDIO_PORT_2CH;
    param.numBlocks = AUDIO_BLOCK_8;
    if (audioPortOpen(&param, &hidden->port) != 0) {
        return SDL_SetError("The console's audio port cannot be opened");
    }
    hidden->port_open = true;
    if (audioGetPortConfig(hidden->port, &hidden->config) != 0) {
        return SDL_SetError("The console's audio port does not tell its buffer");
    }
    if (audioCreateNotifyEventQueue(&hidden->queue, &hidden->queue_key) != 0) {
        return SDL_SetError("The console's audio port has no event queue");
    }
    if (audioSetNotifyEventQueue(hidden->queue_key) != 0) {
        sysEventQueueDestroy(hidden->queue, 0);
        return SDL_SetError("The console's audio port takes no event queue");
    }
    hidden->queue_set = true;
    sysEventQueueDrain(hidden->queue);

    // The port's format is the device's, whatever was asked for: SDL converts to it.
    device->spec.format = SDL_AUDIO_F32;
    device->spec.channels = LV2AUDIO_CHANNELS;
    device->spec.freq = LV2AUDIO_RATE;
    device->sample_frames = AUDIO_BLOCK_SAMPLES;
    SDL_UpdatedAudioDeviceFormat(device);

    if (audioPortStart(hidden->port) != 0) {
        return SDL_SetError("The console's audio port does not start");
    }
    return true;
}

static bool LV2AUDIO_WaitDevice(SDL_AudioDevice *device)
{
    sys_event_t event;
    // An event or the wait's end: either way the next block is written.
    sysEventQueueReceive(device->hidden->queue, &event, LV2AUDIO_WAIT_USEC);
    return true;
}

static Uint8 *LV2AUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return (Uint8 *)device->hidden->mix;
}

static bool LV2AUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    const audioPortConfig *config = &device->hidden->config;
    // The config's read index is the address of the number of the block being played, which the
    // system keeps as 64 bits.
    const u64 playing = *(const u64 *)(uintptr_t)config->readIndex;
    const u32 next = (u32)((playing + 1) % config->numBlocks);
    float *ring = (float *)(uintptr_t)config->audioDataStart;
    SDL_memcpy(ring + (size_t)next * LV2AUDIO_CHANNELS * AUDIO_BLOCK_SAMPLES, buffer, (size_t)buflen);
    return true;
}

static bool LV2AUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = LV2AUDIO_OpenDevice;
    impl->WaitDevice = LV2AUDIO_WaitDevice;
    impl->GetDeviceBuf = LV2AUDIO_GetDeviceBuf;
    impl->PlayDevice = LV2AUDIO_PlayDevice;
    impl->CloseDevice = LV2AUDIO_CloseDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    return true;
}

AudioBootStrap LV2AUDIO_bootstrap = {
    "lv2", "PlayStation 3 audio port", LV2AUDIO_Init, false, false
};

#endif // __lv2ppu__
