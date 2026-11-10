// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
// Exercise the actual C menu bridge on synthetic game memory: no disc assets or injected game code.
#include <cstdlib>
#include <cstring>

#include "check.h"
#include "openrac/game_host.h"
#include "openrac/memory.h"

namespace {
int vanilla_calls, requested = -1, mount = -1, result;
const openrac_front_end descriptor{.levels = 19};
const openrac_map_config maps[] = {{.name = "Harbor", .level = 1}, {.name = "Arena", .level = 1}};
}  // namespace

extern "C" {
const openrac_front_end* openrac_game_front_end = &descriptor;
const openrac_map_config* openrac_game_destinations = maps;
unsigned openrac_game_destination_count = 0;
int openrac_game_active_destination = -1;
const openrac_map_config* openrac_game_map = nullptr;
int func_00222848(gaddr menu);
int func_0021BB90(gaddr menu);
void func_L00_00290030(int target);
gaddr rac1_ship_destination_text(int id);

int func_00222848__game(gaddr) {
    return 0;
}

int func_0021BB90__game(gaddr) {
    return result;
}

void func_L00_00290030__game(int) {
    ++vanilla_calls;
}

void func_0022F4A0(int level) {
    requested = level;
}

void openrac_game_select_destination(int index) {
    mount = index;
    openrac_game_active_destination = index;
    openrac_game_map = index >= 0 ? &maps[index] : nullptr;
}

[[noreturn]] void openrac_game_crash(const char*) {
    std::abort();
}
}

int main() {
    openrac::runtime::Memory::create();
    constexpr gaddr menu = 0x10000, page = 0x11000, old_indices = 0x12000;
    func_L00_00290030(1);
    CHECK(vanilla_calls == 1 && requested == -1);
    openrac_game_destination_count = 2;
    openrac_guest_set_u32(0x001602E0, old_indices);
    openrac_guest_set_u32(0x001D5F74, page);
    openrac_guest_set_u32(page + 0x40, menu);
    for (int i = 1; i < 19; ++i) {
        openrac_guest_set_u32(0x0013D618 + (i - 1) * 4, i);
        openrac_guest_set_u16(0x001DE0C0 + i * 12, static_cast<uint16_t>(20000 + i));
    }
    func_00222848(menu);
    const gaddr rows = openrac_guest_u32(menu + 0x34);
    CHECK(rows != 0x001D6648);
    CHECK(static_cast<short>(openrac_guest_u16(rows + 18 * 12)) == 0x7B00);
    CHECK(static_cast<short>(openrac_guest_u16(rows + 19 * 12)) == 0x7B01);
    CHECK(static_cast<short>(openrac_guest_u16(rows + 20 * 12)) == 0);
    CHECK(std::strcmp(static_cast<char*>(G(rac1_ship_destination_text(0x7B00))), "Harbor") == 0);
    CHECK(rac1_ship_destination_text(20001) == 0);
    openrac_guest_set_u32(menu + 0x40, 18);
    openrac_guest_set_u32(0x0013CC04, 0x40);
    func_0021BB90(menu);
    func_L00_00290030(1);
    CHECK(mount == 0 && requested == 1);  // logical 19 never reaches the guest loader
    CHECK(openrac_guest_u32(0x001602E0) == old_indices);
    func_00222848(menu);
    openrac_guest_set_u32(menu + 0x40, 19);
    func_0021BB90(menu);
    func_L00_00290030(1);
    CHECK(mount == 1 && requested == 1);  // destination 20 shares code, not its world, with 19
    CHECK(std::strcmp(static_cast<char*>(G(rac1_ship_destination_text(0x7B01))), "Arena") == 0);
    func_00222848(menu);
    openrac_guest_set_u32(menu + 0x40, 0);
    func_0021BB90(menu);
    func_L00_00290030(1);
    CHECK(mount == -1 && requested == 1);  // same template, different world
    func_L00_00290030(2);
    CHECK(vanilla_calls == 2);  // ordinary planet-to-planet travel keeps its original code
    CHECK(static_cast<int>(openrac_guest_u32(0x0013D618)) == 1);
    CHECK(static_cast<int>(openrac_guest_u32(0x0013D618 + 18 * 4)) == 0);
    func_00222848(menu);
    openrac_guest_set_u32(menu + 0x40, 18);
    openrac_guest_set_u32(page + 0x40, 0);  // an unfocused list cannot request a map
    func_0021BB90(menu);
    func_L00_00290030(1);
    CHECK(vanilla_calls == 3 && mount == -1);
    openrac_guest_set_u32(page + 0x40, menu);
    func_0021BB90(menu);
    result = -1;  // cancelling must discard a previously selected custom destination
    func_0021BB90(menu);
    result = 0;
    func_L00_00290030(1);
    CHECK(vanilla_calls == 4 && mount == -1);
    return openrac::test::result();
}
