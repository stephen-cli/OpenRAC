# The big-endian boot test

The port built for a big-endian machine and run headless to the first
level, beside the same build for x86-64, to measure what byte order breaks
before a PS3 port is written ([docs/port/PS3.md](../../../docs/port/PS3.md),
sections 3 and 6). The target is 64-bit big-endian PowerPC Linux, as the
Cell's PPU is a 64-bit PowerPC, run under QEMU's user mode; it needs no
console and no PS3 toolchain.

| File | |
|---|---|
| `Containerfile` | Debian with Clang (the x86-64 build, and hostgen's reader), the PowerPC cross GCC and binutils, and qemu-user |
| `ppc64be.cmake` | the CMake toolchain file: the PowerPC cross GCC, for the Cell's PPU (`-mcpu=cell`); Clang has no storage-order pragma, and lld no ELFv1 |
| `boot.sh` | one headless run of the game, straight into Veldin, with its state and reached functions written beside the data |

```sh
podman build --platform linux/amd64 -t openrac-be port/tools/bigendian
# in the container, with the repository at /src, a folder for the extracted game at /data and the disc image's
# folder at /iso:
#   podman run --rm -v "$PWD:/src" -v "$DATA:/data" -v "$ISOS:/iso:ro" -w /src/port openrac-be ...
CC=clang CXX=clang++ cmake --preset headless -B build/be-x86 -DOPENRAC_PORT_FAST_BUILD=OFF
cmake --build build/be-x86 --target openrac-rac1-pal openrac-extractor
cmake --preset headless -B build/be-ppc -DOPENRAC_PORT_FAST_BUILD=OFF \
    -DCMAKE_TOOLCHAIN_FILE=/src/port/tools/bigendian/ppc64be.cmake
cmake --build build/be-ppc --target openrac-rac1-pal -- -k 0
sh tools/bigendian/boot.sh be-x86 1800                                         # extracts the disc first
sh tools/bigendian/boot.sh be-ppc 1800 qemu-ppc64 -L /usr/powerpc64-linux-gnu
cmake --build build/be-ppc && (cd build/be-ppc && ctest -j4)                   # the tests, under QEMU
```

The build's `-k 0` keeps it going past errors so one build lists every
file that does not compile.

The game's own memory keeps the console's byte order on every host. On
the big-endian build the translated C and the hand-written C read and
write it through GCC's `scalar_storage_order` (`guest.h`: `GREF` and
`OPENRAC_GUEST_ORDER`), the runtime and the libraries through
`openrac_le32` and the `openrac_guest_*` accessors, and the asset readers
through `common/byte_order.h`, which swaps a record field by field. The
port's tests, run under QEMU with the toolchain file's emulator, are the
readers' check: they read synthetic data through the same readers.

The game is run without a window, so a scripted press (`OPENRAC_PRESS`)
gets it through the boot's memory card warning, as the window's does
(`game/common/program/game_host.cpp`). Both runs write
`/data/<build>-state.json` (the level and the hero's position at the end)
and `/data/<build>-reach.json` (the functions without C the run reached):
a big-endian run that matches the x86-64 one has reached the same level
with the hero at the same position.

Configuring and translating the game take minutes each when the
repository is a bind mount from Windows; the x86-64 run takes 30 seconds
for 1800 frames, the QEMU run several minutes.
