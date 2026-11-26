// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors

#include "platform/lv2_events.h"

#include <array>
#include <cstdint>

#include <SDL3/SDL.h>

#include "common/log.h"

#include <io/pad.h>
#include <sysutil/sysutil.h>

namespace openrac::platform {
namespace {

// The console's ports that are read: more pads than the game has ports for can be connected, and
// the player chooses among them (players.h).
constexpr std::uint32_t kPorts = 4;

// A DUALSHOCK 3's USB identity, by which SDL knows the pad's kind (the buttons' PlayStation names).
constexpr Uint16 kSonyVendor = 0x054C;
constexpr Uint16 kDualShock3Product = 0x0268;

struct Port {
    SDL_JoystickID id = 0;  // 0: no pad on the port
    SDL_Joystick* joystick = nullptr;
};

std::array<Port, kPorts> ports;

// A bit of one of the pad's two button words, and the gamepad button it is.
struct ButtonBit {
    int word;  // the index in padData::button
    std::uint16_t bit;
    SDL_GamepadButton button;
};

constexpr ButtonBit kButtons[] = {
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_SELECT, SDL_GAMEPAD_BUTTON_BACK},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_START, SDL_GAMEPAD_BUTTON_START},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {PAD_BUTTON_OFFSET_DIGITAL1, PAD_CTRL_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_CROSS, SDL_GAMEPAD_BUTTON_SOUTH},
    {PAD_BUTTON_OFFSET_DIGITAL2, PAD_CTRL_SQUARE, SDL_GAMEPAD_BUTTON_WEST},
};

// A stick's axis and where the pad reports it.
struct StickAxis {
    int word;
    SDL_GamepadAxis axis;
};

constexpr StickAxis kSticks[] = {
    {PAD_BUTTON_OFFSET_ANALOG_LEFT_X, SDL_GAMEPAD_AXIS_LEFTX},
    {PAD_BUTTON_OFFSET_ANALOG_LEFT_Y, SDL_GAMEPAD_AXIS_LEFTY},
    {PAD_BUTTON_OFFSET_ANALOG_RIGHT_X, SDL_GAMEPAD_AXIS_RIGHTX},
    {PAD_BUTTON_OFFSET_ANALOG_RIGHT_Y, SDL_GAMEPAD_AXIS_RIGHTY},
};

// A trigger: its bit, where its pressure is reported, and its axis.
struct Trigger {
    std::uint16_t bit;
    int pressure;
    SDL_GamepadAxis axis;
};

constexpr Trigger kTriggers[] = {
    {PAD_CTRL_L2, PAD_BUTTON_OFFSET_PRESS_L2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {PAD_CTRL_R2, PAD_BUTTON_OFFSET_PRESS_R2, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

// One of the pad's bytes (0..255, a stick's centre 128) as a joystick axis's whole range, which is
// what SDL takes a virtual gamepad's sticks and triggers in: 0 is -32768, 255 is 32767.
Sint16 axis_of(std::uint16_t byte) {
    return static_cast<Sint16>(static_cast<int>(byte & 0xFF) * 257 - 32768);
}

// SDL_RumbleGamepad for a console pad: its large motor takes a speed, its small one is on or off.
bool SDLCALL rumble(void* userdata, Uint16 low, Uint16 high) {
    padActParam motors{};
    motors.large_motor = static_cast<u8>(low >> 8);
    motors.small_motor = high != 0 ? 1 : 0;
    const auto port = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(userdata));
    return ioPadSetActDirect(port, &motors) == 0;
}

void connect(std::uint32_t index) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = kSonyVendor;
    desc.product_id = kDualShock3Product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    // Every button up to the last of the pad's and every axis, so that each one's number as a
    // joystick's is its number as a gamepad's: SDL numbers a virtual gamepad's buttons and axes in
    // a row over those it is told are there. The PS button is among them and never pressed (the
    // system keeps it).
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.button_mask = (1u << desc.nbuttons) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "PlayStation 3 Controller";
    desc.userdata = reinterpret_cast<void*>(static_cast<std::uintptr_t>(index));
    desc.Rumble = rumble;

    Port& port = ports[index];
    port.id = SDL_AttachVirtualJoystick(&desc);
    port.joystick = port.id != 0 ? SDL_OpenJoystick(port.id) : nullptr;
    if (port.joystick == nullptr) {
        log::warn("the pad on port {} cannot be read: {}", index + 1, SDL_GetError());
        if (port.id != 0) {
            SDL_DetachVirtualJoystick(port.id);
        }
        port = Port{};
        return;
    }
    // The triggers' pressures come only in this mode; a pad without it reports their bits alone.
    ioPadSetPressMode(index, PAD_PRESS_MODE_ON);
    // At rest until the pad's first report: a virtual joystick's axes start in the middle, which
    // is a trigger half pressed.
    for (const Trigger& t : kTriggers) {
        SDL_SetJoystickVirtualAxis(port.joystick, t.axis, axis_of(0));
    }
}

void disconnect(std::uint32_t index) {
    Port& port = ports[index];
    SDL_CloseJoystick(port.joystick);
    SDL_DetachVirtualJoystick(port.id);
    port = Port{};
}

// The pad's report, when it has a new one, into its virtual gamepad.
void read(std::uint32_t index) {
    padData data{};
    // len 0: nothing changed since the last read, and the structure holds nothing.
    if (ioPadGetData(index, &data) != 0 || data.len <= PAD_BUTTON_OFFSET_ANALOG_LEFT_Y) {
        return;
    }
    SDL_Joystick* joystick = ports[index].joystick;
    for (const ButtonBit& b : kButtons) {
        SDL_SetJoystickVirtualButton(joystick, b.button, (data.button[b.word] & b.bit) != 0);
    }
    for (const StickAxis& s : kSticks) {
        SDL_SetJoystickVirtualAxis(joystick, s.axis, axis_of(data.button[s.word]));
    }
    for (const Trigger& t : kTriggers) {
        const bool down = (data.button[PAD_BUTTON_OFFSET_DIGITAL2] & t.bit) != 0;
        const std::uint16_t pressure =
            data.len > t.pressure ? data.button[t.pressure] : static_cast<std::uint16_t>(0);
        // A held trigger whose pressure is not reported (no pressure mode) is fully pressed.
        const std::uint16_t byte = down && pressure == 0 ? 0xFF : pressure;
        SDL_SetJoystickVirtualAxis(joystick, t.axis, axis_of(byte));
    }
}

void read_pads() {
    static const bool started = [] {
        const s32 result = ioPadInit(kPorts);
        if (result != 0) {
            log::warn(
                "the console's pads cannot be read (ioPadInit: {:#x})",
                static_cast<std::uint32_t>(result)
            );
        }
        return result == 0;
    }();
    padInfo2 info{};
    // Without SDL's joysticks (a program that opened no input) there is nothing to attach a pad to.
    if (!started || !SDL_WasInit(SDL_INIT_JOYSTICK) || ioPadGetInfo2(&info) != 0) {
        return;
    }
    for (std::uint32_t index = 0; index < kPorts; ++index) {
        const bool connected = (info.port_status[index] & 1u) != 0;
        if (connected && ports[index].id == 0) {
            connect(index);
        } else if (!connected && ports[index].id != 0) {
            disconnect(index);
        }
        if (ports[index].id != 0) {
            read(index);
        }
    }
}

// The system's call when it has something for the program (sysUtilCheckCallback makes it).
void on_system_event(u64 status, u64, void*) {
    if (status == SYSUTIL_EXIT_GAME) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
}

void read_system() {
    static const bool registered = [] {
        const s32 result = sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_system_event, nullptr);
        if (result != 0) {
            log::warn(
                "the XMB's request to quit will not be heard (sysUtilRegisterCallback: {:#x})",
                static_cast<std::uint32_t>(result)
            );
        }
        return result == 0;
    }();
    if (registered) {
        sysUtilCheckCallback();
    }
}

}  // namespace

void read_lv2_events() {
    read_system();
    read_pads();
}

}  // namespace openrac::platform
