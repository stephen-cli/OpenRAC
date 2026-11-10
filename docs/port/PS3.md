# A PS3 port: study

What stands between the port as it is and a native build for the
PlayStation 3, run as homebrew on a console with HEN or custom firmware.
Ratchet & Clank (PAL) is the game in question. Nothing here is built yet; the
study was made on 2026-11-06 from the port's sources and published accounts
of the 2012 Ratchet & Clank Collection.

## 1. Goal and order of work

1. **The original game, faithfully.** The PS2 game as it plays, with fewer
   bugs than the Ratchet & Clank Collection (section 4). No enhancements.
2. **A higher resolution.**
3. **The Executive Edition** on the PS3 build (section 5).
4. **Co-op multiplayer**, after the above.

The player's setup is the port's own (DESIGN.md, decided item 3): the PS3
program contains nothing from the disc; on first start it reads the player's
disc image from the console's hard drive, checks it and extracts its files.
Nothing is compiled on the console.

## 2. What stands in the way

| | The port today | The PS3 | Work |
|---|---|---|---|
| Byte order | little-endian everywhere: the console's data in game memory, the asset readers, the renderer's packet readers (`port/game/common/mod_scripts.cpp`: "little-endian, as the console and every host of the port") | the Cell's PPU is big-endian | the largest item (section 3); done for the headless game, the asset readers and the port's tests (section 6); the other games' host C and the renderer remain |
| Game memory | one 4 GB reservation whose offsets are the console's addresses, the uncached and accelerated ranges mapped as views of the same RAM (`port/runtime/memory.cpp`) | a 32-bit address space, 256 MB of main RAM, no reservation without backing | regions packed into a small allocation; `G()` folds the aliases by masking; the 256 MB expanded arena (mods) left out |
| Textures | converted to RGBA8, several times the console's 4- and 8-bit size | 256 MB of video RAM | measure a level's textures; keep palettised textures if needed |
| Renderer | OpenGL 4.1 core, OpenGL ES 3.2 or Vulkan; GLSL 4.10 | RSX, a GeForce 7-class GPU: Shader Model 3, no compute; homebrew draws through PSL1GHT's RSX library with Cg programs compiled ahead of time | a new backend and every shader written again; reversed depth checked against the RSX's depth formats |
| Toolchain | C++20, CMake, SDL3 | ps3toolchain's GCC 13.2 compiles C++20, but its libstdc++ has no threads and its newlib (1.20, 2012) no fenv and no C99 names in std (section 6); no SDL3 | the toolchain's libraries rebuilt for the port; pads, sound, files and the display on PSL1GHT's libraries |
| CPU | modern out-of-order cores | the PPU, in order, 3.2 GHz; six SPUs | unmeasured; the Collection shows the game's code fits the Cell |

## 3. Byte order

`G(a)` (`port/runtime/include/openrac/guest.h`) is a plain pointer into game
memory, and translated C reads and writes through it with the host's loads
and stores. Game memory holds the console's data in the console's order: the
executable's data, every level's data and the display lists the game builds.

Choices:

- **Keep game memory in the console's order and swap on access.** hostgen
  already rewrites every pointer use in the game's C, so it can emit
  byte-reversed loads and stores (the PPU has `lwbrx`/`stwbrx`). Byte
  accesses need nothing. What hostgen does not see needs finding by hand:
  the port's C++ reading game memory, `memcpy` into host structs, the
  128-bit copies (`qcopy`), the hand-written C for functions still in
  assembly, and every reader of extracted data.
- **Convert the data to the host's order at load.** This needs the type of
  every word of every data section and asset: not known for most of it
  (DECOMP_STATUS.md, section 3: none of the data is C).

The first is the workable one. The cheap test before committing to it:
build the game for a big-endian target (big-endian PowerPC Linux under QEMU,
or PSL1GHT under RPCS3) and run the boot and title loop with no renderer,
counting what breaks.

## 4. Lessons from the Ratchet & Clank Collection

Idol Minds' 2012 PS3 collection (720p, 60 fps) ran the games' own code on
the Cell, built with Sony's tools. Its known faults are in the layers a port
replaces:

| Fault | Where a port has the same risk | Guard |
|---|---|---|
| sounds out of step with the picture (the Heli-Pack glide) | the sound library replaced at its API | sound timing compared with PS2 recordings |
| loading times that change play: Drek Skip works on PS3 and not on PS2 | disc reads replaced by file reads | disc timing kept where the game's behaviour depends on it |
| another build's data: the PAL Platinum Kalebo III text, "Galactron" for Gadgetron | the wrong data on the right code | the extractor's check of the disc against `game.json` |
| the US debug menu left in, invisible but working | | |
| freezes and stutters (reviews) | | the regression runs (REGRESSION.md) on the PS3 build |

Sources: [TCRF: Ratchet & Clank Collection](https://www.tcrf.net/Ratchet_%26_Clank_Collection),
[TCRF: bugs](https://new.tcrf.net/Bugs:Ratchet_%26_Clank_Collection),
[speedrun.com thread on the PS3 versions](https://www.speedrun.com/post/6ei3x).

## 5. The Executive Edition on the port

The Executive Edition (`rac-editions`) is a patch to the PS2 executable and
a rebuilt disc image: its code runs in a mod segment of the retail program.
The port does not run patched executable bytes. The edition's code is C
written against the same decompilation, so on the port it comes in as the
game's C does, through hostgen and the port's mods (MODS.md). Features that
patch the PS2's picture, such as widescreen, become renderer settings.

The edition's PS2 image is built on a PS3 by a separate tool: a homebrew
build of the editions installer, whose core is a C library shared with the
desktop builder (`rac-editions/installer`).

## 6. The big-endian boot test (2026-11-09)

The test of section 3, run with [port/tools/bigendian](../../port/tools/bigendian/README.md):
the port built headless for 64-bit big-endian PowerPC Linux (with Clang
for this first run, GCC afterwards) and run under QEMU's user mode, beside
the same build for x86-64, both started straight into Veldin for 1800
frames.

| | x86-64 | big-endian PowerPC |
|---|---|---|
| The PAL game's translated C (1,090 files, 1.28 million lines), the runtime and the libraries | compile | compile, unchanged |
| The asset readers (`assets/bytes.h` and the 56 files on it) | compile | refused by the readers' own guard, a `static_assert` that the host is little-endian; built as they are under `OPENRAC_BIG_ENDIAN_TRIAL`, to see the run |
| Link | 57.8 MB | 62.8 MB, ELF64 MSB |
| The run | the card warning passed at frame 100, the movies skipped, the title, Veldin loaded; the hero at (132.09, 115.48, 31.43) at frame 1800, in 30 s | the executable loaded and the display set up as on x86-64; then the game's first disc read reached `lib/cdvd.c` as 2,717,908,992 sectors at 490,209,280 (`0xA2000000` and `0x1D380000`: 162 sectors at 14,365, byte-reversed), failed, and was asked for again until the run was stopped |

So the compiler is not the obstacle, and the first break is the one
section 3 names: the host's libraries read the game's arguments out of
game memory with plain loads (`G()` and `GREF` in `guest.h` are pointers),
while the game's own translated C, which wrote them, would read them back
the same way and agree with itself. Nothing in the translated code broke
before that point; what breaks after it is not measured yet, as the read
is the first thing the boot waits on.

The headless game had no way through the boot's memory card warning
(`check_memory_card` in bmain.c loops until a button is pressed; the
scripted press, `OPENRAC_PRESS`, was read by the window's input only), so
the no-window build now takes the plain `FRAME:MASK:FRAMES` entries too.

Toolchains: the editions builder's ps3dev image carries GCC 7.2 (C++17,
not the port's C++20); ps3toolchain's current nightly (2026-10-11) builds
GCC 13.2, which has it.

### Byte order, done the first way

The same day, game memory was given the console's order on a big-endian
host as section 3 chooses, and the test run again with GCC in place of
Clang:

- **The translated C** declares the game's structures under GCC's
  `#pragma scalar_storage_order little-endian` (`OPENRAC_GUEST_ORDER`,
  which hostgen emits at the top of every unit), and `GREF` is a field of
  such a structure, so every load and store through it swaps bytes. No
  change to hostgen's lowering was needed: a game pointer is already an
  address, and every access through one already goes through `GREF` or a
  structure cast of `G()`. Clang has no such pragma, so the big-endian
  build is GCC's, as the PS3's would be, and for the Cell's PPU
  (`-mcpu=cell`: GCC 14's generic PowerPC target stops with an internal
  error on a byte-swapped `volatile` 64-bit load, the game's hardware
  registers). The game's `long double` is lowered to `double`, the 8 bytes
  it is on the console (GCC has no reversed storage order for a 16-byte
  float, and the x86 build read 16 bytes from an 8-byte argument).
- **The hand-written C** (the libraries in `game/common/lib`, the game's
  host C in `game/rac1/host`) copied words in and out of game memory with
  `memcpy` and host pointers: about sixty sites now read and write through
  `GREF`, `openrac_guest_read_words` and `openrac_guest_write_words`
  (`guest.h`). Taking the address of a swapped scalar is an error, so three
  sites that kept a host pointer into game memory keep the address instead.
  The two image structures of `graph.c` and the card directory entry of
  `mc.c` are declared under the pragma.
- **The runtime and the C++ program** read the game's code words through
  `word_at` (`runtime/guest.cpp`), a level piece's header and the sound
  engine's command words through `openrac_le32`.
- **The asset readers** read a number in the console's order whatever the
  host's (`ByteView::read`, `ByteWriter::put`); a record of several fields
  was still copied as its bytes are, under `OPENRAC_BIG_ENDIAN_TRIAL`
  (lifted the next day, below).

With that, the big-endian build (101 MB, GCC 14, `-mcpu=cell`) under QEMU
passes the card warning, skips the movies, loads both sound banks (10 and
236 sounds), loads Veldin, and ends frame 1800 with the hero at (132.09,
115.48, 31.43) heading 0.66, the same level relocation counts and the same
two reached functions without C as the x86-64 run: its log and its state
file are the baseline's, line for line, in 31 s. The
x86-64 build's own state is unchanged by the work, and its 143 tests pass.

### The asset readers' records (2026-11-10)

The readers copy a record (a level header, a moby class header, a
tie's or a tfrag's) as its bytes are and convert it whole through
`common/byte_order.h`: `from_little_endian()` swaps a number, an array
element by element and an aggregate record field by field, through
nesting, and is its own inverse, so a writer uses it too. C++20 has no
reflection, so a record's fields are found by testing which aggregate
initialisations compile (`field_count()`) and bound with a structured
binding of that many names, generated for 0 to 40 fields
(`byte_order_fields.h`, from `tools/gen_byte_order_fields.py`). A
record's arrays must be `std::array` (a C array member elides its braces
and is counted element by element). The guard in `assets/bytes.h` and
`OPENRAC_BIG_ENDIAN_TRIAL` are gone: `read<T>`, `read_array<T>` and
`put<T>` convert for any host.

Besides the readers' `read<T>` calls, about a dozen sites copied a number
or a record with `memcpy` in the host's order (the WAD size, the title
world's index, the overlay's, the save game's and the fonts' code words,
the gameplay and camera fixups, a shrub packet's quadword records, an
animation's quaternion lanes, a replaced sound's offset), as did the
tests' synthetic data writers; they convert now.

The readers' check on big-endian is the port's test suite under QEMU
(the headless build has no renderer, so `--check-levels` is not
available to it): every test that reads synthetic data through the
readers passes on big-endian PowerPC as on x86-64. The GCC build's boot
run is unchanged: the same state as x86-64 at frame 1800, line for line.
Of the 144 tests, 12 failed on big-endian at first, in two groups outside
the readers, both done the same day:

- **C++ and tests that read game memory through host pointers**
  (`runtime`, `ship_destination`, `mod_scripts`, `mod_boundary`,
  `graph_dma`, `hero_animation`, `effect_quad`, `world_menu`,
  `particle53`). The port's C++ has no `GREF` that swaps, so a C++ test
  reads and writes game memory through `openrac_guest_u32`, `_u16` and
  the word copies; a C test's own buffers go through `GREF`. Two readers
  of the hand-written C were still raw: the effect quad record
  (`effect_quad.c`, `lib/effect_texture.h`) and the pause menu's held
  camera copy (`world.c`); the runtime's relocation scanner had three raw
  word reads. A hand-written level file read a word's low byte as its
  first byte (`level_hero_animation.c`), which hostgen's lowering hid in
  the game and the test showed. The NTSC game's `water.c` took the
  address of a swapped byte and clears through `G()` now; the other games'
  host C has not been built for big-endian yet (rac2-ntsc's `render.c`
  reads its quad record raw, as rac1's did).
- **The lighting probes' packed irradiance** (`lighting_probes`,
  `lighting_probe_atlas`, `lighting_probe_decode`): the R11G11B10 words
  are stored little-endian by the writer and were read back in the host's
  order.

Two of the failures were QEMU's, not the port's. The mods' Luau scripts
run some ten times slower under it, past the game's 2 ms budget, so the
tests run with `OPENRAC_SCRIPT_BUDGET_MS=100`. And QEMU's PowerPC FPU has
no non-IEEE mode: `runtime`'s check that a denormal is zero to the EE
cannot pass under it. `openrac_ee_float_mode` sets FPSCR's NI bit on
PowerPC, which the Cell's PPU honours (denormal inputs and results are
flushed), to be confirmed on the console. With that one check excepted,
the big-endian build passes the suite, 143 of 144, and the boot run's
state is x86-64's.

### The toolchain (2026-11-10)

[port/tools/ps3](../../port/tools/ps3/README.md): a container with
ps3toolchain's 2026-10-11 nightly (GCC 13.2.0 for the PPU, where the
editions builder's image has 7.2), PSL1GHT and ps3libraries at pinned
commits, a CMake toolchain file (`ppu.cmake`, PSL1GHT's `ppu_rules`
flags, `__lv2ppu__` the platform test) and a `ps3` preset without the
games, the window and the enhanced rendering. The port's common library
and runtime took the console as a platform: no signals, unwinder, loader,
`mmap` or `/proc`, so the crash reporter captures no backtrace, no fault
handler is installed, the process statistics are zero, and game memory
is main RAM alone until section 2's layout.

The compiler compiles C++20; its libraries do not take the port's. GCC's
thread model is `single` (libstdc++ built without gthreads, before
PSL1GHT's pthread existed in ps3toolchain's order), so there is no
`std::thread`, `std::mutex` or `std::condition_variable`; and newlib is
1.20.0 of 2012, without `<fenv.h>` and with its C99 names hidden from
`-std=c++20` and absent from `std::`, which `<string>` and `<format>`
reach, so no unit of the port compiles as the toolchain ships. The
measurements and the two routes are in the README: a toolchain of the
port's own from ps3toolchain's scripts (a current newlib, libstdc++
configured after PSL1GHT with posix threads; GCC and its PS3 patch as
they are), or a layer of the port's own, which cannot reach the
libstdc++ configuration and so is not enough alone.

## 7. Next steps

1. The toolchain's libraries (section 6): a current newlib and a
   libstdc++ with posix threads over PSL1GHT's pthread, built from
   ps3toolchain's scripts in the port's container; then the runtime and
   the common library against PSL1GHT.
2. The memory layout for 256 MB (section 2).
3. The renderer backend, once the game runs headless on the console; the
   extracted levels through the readers on big-endian (`--check-levels`)
   come with it.
