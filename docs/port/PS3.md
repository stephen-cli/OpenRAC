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
| Game memory | one 4 GB reservation whose offsets are the console's addresses, the uncached and accelerated ranges mapped as views of the same RAM (`port/runtime/memory.cpp`) | a 32-bit address space, 256 MB of main RAM, no reservation without backing | done (section 6): the regions packed into one 48 MB allocation, `G()` through a table of 256 offsets, the aliases onto the one RAM; the 256 MB expanded arena (mods) left out |
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

The compiler compiles C++20; its libraries, as ps3toolchain ships them,
did not take the port's. GCC's thread model was `single` (libstdc++
built without gthreads, before PSL1GHT's pthread existed in
ps3toolchain's order), so there was no `std::thread`, `std::mutex` or
`std::condition_variable`; and newlib was 1.20.0 of 2012, without
`<fenv.h>` and with its C99 names hidden from `-std=c++20` and absent
from `std::`, which `<string>` and `<format>` reach, so no unit of the
port compiled.

So the toolchain's libraries are built for the port (the README's
"Route 1", `toolchain.sh`, the next day): newlib 4.6 from devkitPro's
fork with the console's pieces carried over from ps3toolchain's 2012
patch (its configuration, `__getreent` per thread, `dirent`, the
`__syscalls` table PSL1GHT's librt fills, newlib's retargetable locks
over lv2's mutexes), GCC 13.2 again on it with posix threads over
pthreads-embedded (whose headers get C++ linkage and whose struct
`pthread_t` gets the comparisons `std::thread::id` needs), one fix to
the compiler (its backend chose out-of-line register saves the target's
libgcc does not have), the system call glue built by hand, and PSL1GHT
again on the new C library. What PSL1GHT still expects of newlib 1.20
(`struct stat`'s layout, `fd_set`, `MAXPATHLEN`, the `_DEFUN` macros) is
kept for the console. With that, `ppu-g++` reports the posix thread
model, a C++20 program with threads and `std::format` links, and the
port's common library and runtime build and archive for the console
with the `ps3` preset: the first of the port's code compiled for the
PS3.

### The headless program on the console (2026-11-11)

The whole port but the window and the renderer, built for the PS3 with
the `ps3` preset and `OPENRAC_PORT_GAMES=ON`
([port/tools/ps3](../../port/tools/ps3/README.md)): the PAL game's
1,090 translated units, the mods and Luau, the asset readers, the sound
engine and `main.cpp`, linked against PSL1GHT into a 108 MB ELF. What
the console needed of the port is small and listed in the README: the
game's own libc defines four of newlib's names, Luau and the runtime's
loader of native mods each wanted a line, the stuck watch's signal has
no console, and a program there starts with no command line or
environment, so it reads both from a file beside it.

Run under RPCS3 (0.0.42) with the data on the host's disk through its
`/host_root` mount, the program boots, sets its session log up from the
PS3 side ("PlayStation 3 (PSL1GHT), 256 MB of memory"; "Cell Broadband
Engine PPU, 2 threads"), allocates main RAM and then stops at the
first write outside it, the one region it mapped (the `memset` that
clears the EE's register page, `0x10000000` past the allocation, which
the 4 GB reservation of the other platforms maps and the console
cannot): section 2's layout for 256 MB, which the next subsection has.
Three
things stood between the build and that point, none in the port's
code: PSL1GHT's `sprxlinker` runs after every link (the stub table's
import count is zero without it, so no system call resolves); RPCS3
itself crashes with more than two arguments after `--`, which the file
beside the program is for; and its `sys_fs_rmdir` throws for a path
that is not there, which newlib's `remove()` tries after a failed
unlink, so the console's newlib stops at the first. Its `unlink` on a
`/host_root` path fails too (it moves the file to the mount's root
first, which that mount has not), so the port's log rotation keeps its
old logs there; the game's data belongs under `/dev_hdd0` as on the
console, which a junction can give it.

### Game memory, packed (2026-11-13)

On the console game memory is one allocation of 48 MB: main RAM, then
the register pages, the scratchpad and the port's own region side by
side (`OPENRAC_PACKED_MEMORY`, `port/runtime/memory.cpp`). `G()` adds to
a game address the entry of a table that its top eight bits pick
(`openrac_guest_fold`, `guest.h`): 256 entries, one for each 16 MB of
game addresses. Main RAM's entries and those of its two uncached
aliases lead to the one copy of RAM, so the aliases work, which the
reservation gives only where the system can map pages twice. Every
entry with no region behind it leads into 256 MB of address space
reserved with nothing mapped (`sys_mmapper_allocate_address`), so a
stray access still stops the program at its address. Against the
reservation a memory access costs one more instruction and a load
that depends on the address; what that is on the PPU is unmeasured.
The 256 MB expanded arena has no room and is refused. Any host builds
this way with `-DOPENRAC_PACKED_MEMORY` in the compiler's flags, which
is how it was checked before the console: the big-endian GCC build
under QEMU, packed, passes the runtime's memory test and boots Veldin
to the same state at frame 1800 as the build with the reservation.

Under RPCS3 the headless program now goes through the boot: the
executable loaded, the movies skipped, both sound banks read from the
disc image, level 0's program relocated, "now playing level 0", and
Ratchet standing on Veldin at 132.09, 115.48, 31.43, where the Linux
builds have him. Two things beside memory stood in the way. The
variables of the arguments file were set in `main`, after code that
runs before it had read the environment, the one time the port reads
it: `common/env` reads the file itself now, before its first read.
And newlib's table of exit handlers stopped at 32, which a C++ program
fills with its statics' destructors, so the handler that writes the
end of a run was refused: the console's newlib grows the table.

That run stopped at frame 958, at the sound engine's first thread (the
reader of a music track), which the next subsection has.

### Threads on the console (2026-11-13)

`std::thread` threw "Enable multithreading to use std::thread".
libstdc++ reaches the thread library through weak references, which
pull nothing out of a static library, and takes the missing functions
to mean a program without threads: the console's programs link all of
`libpthread` (`ppu.cmake`). With that alone the program stopped before
`main`: libgcc's registration of the exception tables locks a mutex
from the start-up code, and pthreads-embedded creates a mutex on its
first lock with locks of its own that `pthread_init` makes, which
nothing had called, on a heap whose system calls PSL1GHT wires in a
constructor that had not run either. The toolchain's pthreads-embedded
is changed in two places (`pthread-emb-lv2.py`): it initialises itself
in a constructor that runs after PSL1GHT's and before every one of the
program's, and until then its mutex calls do nothing and succeed, the
program having one thread.

With it the headless run under RPCS3 goes the whole 1800 frames and
ends by itself, with the sound engine's reader threads started, and
its state at the last frame is that of the Linux builds to the last
digit: level 0, Veldin, the hero at 132.09, 115.48, 31.426617 turned
0.6627015. The headless game runs on the console's side of the
emulator; no real console has run it yet.

### The first picture (2026-11-13)

[rsx-probe](../../port/tools/ps3/rsx-probe/README.md) draws through
PSL1GHT's `librsx` under RPCS3: a cleared 1280x720 screen and one
triangle with a colour at each corner, read back from the RSX's memory
into a file. Two things it settles for the backend. The RSX's programs
need no Cg toolkit (NVIDIA's, proprietary, which PSL1GHT's `cgcomp`
loads to compile Cg): `cgcomp -a` assembles ARB vertex and fragment
program text by itself. And a console program can hand a script its
own picture, which is how the backend's pictures will be compared
with the other platforms'.

### The window on the console (2026-11-14)

The windowed program builds for the console (preset `ps3-window`) and
runs under RPCS3: the frontend, the debug interface, the sound engine
over SDL and the renderer, with the RSX behind the renderer's GL. It
draws nothing yet but the clear, and the port's own screenshots
(`OPENRAC_SHOT`) come out of it as PNGs. How it got there:

- **SDL3 itself, built for the console.** SDL reaches 45 files of the
  platform layer, the frontend and the debug interface; cutting it out
  of them would have forked the port. SDL 3.4 has no PS3 platform, and
  builds as a Unix without a desktop: pthreads, its dummy video and
  audio drivers, six places of its source changed
  (`tools/ps3/sdl3-lv2.py`) and nine answers its configure got wrong
  given to it (`cmake/Dependencies.cmake`; the toolchain file's checks
  compile without linking). Every other source of the windowed program
  compiled for the console unchanged.
- **GL carried out with the RSX** ([renderer/rsx](../../port/renderer/rsx/README.md)):
  the window opens the RSX for `GlApi::Rsx` and hands the layer's
  loader to the renderer, as it does the Vulkan layer's. Every GL
  function is answered; the clear, the flip and the read-back are
  carried out.
- **Byte order in C++.** The first frame stopped in the mods' menu
  code, which reads the game's numbers through `GREF`: in C that swaps
  on a big-endian host, in C++ it did not, and a pointer read backwards
  sent a copy to nowhere. `GREF` in C++ is now an object that swaps on
  reading and assigning (`guest.h`, `openrac_guest_ref`), which
  corrected all 202 uses at once; seven places that bound a `T&` to one
  were respelled to compile both ways.
- **Two faults of the console's C library**, found by a small SDL
  program before the game met them: a program that had printed stopped
  in `free` on its way out (PSL1GHT gives the heap back before newlib
  closes its streams; the console's are flushed and not closed), and
  SDL's delay returned at once (lv2's `select` does not wait; the
  console sleeps with `usleep`).

Still wrong in that run: the frontend's log repeats "camera usable
again after N refused frame(s)", so something the frontend reads of
the camera is not yet in the host's order. The renderer's own readers
of the game's packets are the rest of section 3's work.

### The game's first picture (2026-11-14)

The windowed program under RPCS3 shows the game's own first screen, the
message that no memory card is inserted: the game's display list walked
by the frontend, decoded by the direct renderer, drawn by the RSX layer
with one vertex and one fragment program, copied to the screen and read
back by the port's own screenshot (`OPENRAC_SHOT`). The 2D path is the
only renderer with RSX programs so far; the world behind it is not
drawn. What it took, most of it found by a test program made for the
purpose:

- **The layer's drawing** ([renderer/rsx](../../port/renderer/rsx/README.md)):
  textures, buffers, vertex arrays, framebuffers, state, draws, copies
  and the read-back, and `programs/direct.vp` and `direct.fp` for
  `shaders/direct.vert` and `direct.frag`. The GLSL branches on its
  uniforms; the fragment program is one sum whose weights the layer
  works out for each draw.
- **[tools/ps3/rsxgl-test](../../port/tools/ps3/rsxgl-test/README.md)**:
  the layer checked by its pictures, 55 checks in the seconds RPCS3
  takes to start a 16 MB program, where the game takes eight minutes a
  run. librsx's faults and the assembler's below showed in the game
  only as a black picture or a program that stopped.
- **Three faults of the toolchain**, which the headless runs had not
  shown:
  - newlib's own do-nothing locks were linked in place of libsysbase's:
    the allocator and every stream ran unlocked under every thread. A
    hash table's chain held the number 9 once two threads allocated.
    The console's libc is built without them (`newlib-lv2.py`).
  - librsx called libgcm's callback for a full segment of the command
    buffer from assembly that told the compiler nothing of what a call
    changes. GCC 13 wrote the next commands over the jump at the
    segment's end, and the RSX lost its place after 32 KB of commands
    (`psl1ght-lv2.py`).
  - librsx's `rsxSetVertexProgramConstants` counts single numbers and
    converts each to an integer. The layer uses the block call.
- **Two of `cgcomp -a`**, the assembler: a fragment instruction reads
  one interpolated input (it takes several and keeps the last), and an
  instruction the chip has none for is left out without a word.
- **Byte order in the readers of the game's memory**: the frontend's
  walk of the display list, the direct renderer's and the texture
  decoder's readers, and the game-state reader behind the camera
  (`game/rac1/game_state.cpp`) copied the console's bytes into host
  numbers. They read through `common/byte_order.h` now
  (`read_little_endian`, `copy_little_endian`), and the refused cameras
  of the first windowed run are gone. The world's readers
  (`frontend/world.cpp`) are still to do.

## 7. Next steps

The renderer backend, now that the game runs headless on the console.
What it has to carry, measured in the tree: the renderer speaks OpenGL
through 106 functions (`renderer/gl.h`), which the Vulkan build already
answers with a layer of its own (`renderer/vulkan`, GL carried out with
Vulkan); the classic picture is 23 GLSL 4.10 shaders, about 1,060
lines (`renderer/shaders`, `scene/shaders`); the enhanced renderer's 39
more, with compute, are beyond a GeForce 7 and stay off the console.
The proposal, in the order that gives a picture soonest:

1. **The window on the console**: done (section 6), with SDL built
   for the console instead of taken out. Left of it: the pad (SDL has
   no joystick driver there; PSL1GHT's `io/pad` behind SDL's virtual
   gamepad) and sound (the dummy device plays nothing).
2. **GL carried out with the RSX**, a layer beside the Vulkan one for
   the functions the classic renderer calls: what the 2D path calls is
   done (section 6). Left: indexed and instanced draws, stencil, depth
   copies, mip chains, uniform blocks and matrices, as the world's
   renderers come to need them.
3. **The shaders written again** as RSX programs, kept beside their
   GLSL and found by a line in it instead of compiled at run time: the
   direct renderer's pair is done, less its distance-field text. Its
   picture has not been compared with another platform's yet. Next the
   world's readers on big-endian, then terrain and ties, whose vertex
   shaders lean on integers and instancing that Shader Model 3 has not,
   so part of their work moves to the CPU.
4. Textures within 256 MB of video memory (section 2), and the
   extracted levels through the readers on big-endian
   (`--check-levels`).
