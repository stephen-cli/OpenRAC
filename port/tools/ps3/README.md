# The PS3 toolchain

The port built for the PlayStation 3 with the homebrew toolchain
(docs/port/PS3.md, section 7): ps3toolchain's GCC 13.2 for the Cell's
PPU, newlib, PSL1GHT and ps3libraries, in a container. The first target
is the runtime and the common library compiled against PSL1GHT; the
games, the renderer and the window come after (section 7's order).

## The container

```sh
podman build -t openrac-ps3 port/tools/ps3        # 30 to 40 minutes: PSL1GHT and ps3libraries from source
```

`Containerfile` takes ps3dev's layout at a pinned commit, with the
toolchain from ps3toolchain's prebuilt nightly (`TOOLCHAIN_RELEASE`,
2026-10-11: GCC 13.2.0, binutils 2.42, newlib 1.20 with threads) and
PSL1GHT and ps3libraries built from source at pinned commits. The
editions builder's image (rac-editions, `installer/ps3`) is the same
layout with ps3dev's default nightly, whose GCC 7.2 has no C++20. Ubuntu
24.04, as the prebuilt needs its glibc; `--platform linux/amd64`, as the
local Debian tags may be i386.

## Configuring and building

```sh
podman run --rm -it -v "$PWD:/src" -w /src/port openrac-ps3
cmake --preset ps3                                # tools/ps3/ppu.cmake, no games, no window
cmake --build build/ps3 --target openrac_runtime -- -k 0   # does not build yet: see below
```

`ppu.cmake` is the CMake toolchain file: `ppu-gcc` and `ppu-g++` from
`$PS3DEV`, PSL1GHT's `ppu_rules` flags (`-mcpu=cell -mhard-float
-fmodulo-sched -ffunction-sections -fdata-sections`), its headers and
libraries, a bare-metal system (`Generic`) whose compiler checks build a
static library. `OPENRAC_PS3` is set for the port's CMake: no test
programs (there is no host to run them on), and `OPENRAC_PS3_LIBRARIES`
names what a program links (`-lsysfs -lsysutil -lsysmodule -lpthread
-lrt -llv2 -lm`).

The compiler defines `__lv2ppu__` (and `__PPU__`), which is the port's
platform test for the console: PSL1GHT's newlib has no signals, no
unwinder, no dynamic loader, no `mmap` and no `/proc`, so the crash
reporter captures no backtrace, the fault handlers are not installed,
the process statistics are zero, and game memory (`runtime/memory.cpp`)
is main RAM alone until its layout for 256 MB is designed.

## What the toolchain lacks (2026-11-10)

The compiler is not the obstacle: `ppu-g++` is GCC 13.2.0, defines
`__lv2ppu__`, `__PPU__` and a big-endian `__BYTE_ORDER__`, and compiles
C++20. Its libraries are:

- **No threads.** GCC's thread model is `single`: libstdc++ was built
  without gthreads (`_GLIBCXX_HAS_GTHREADS` undefined in its
  `c++config.h`), so `std::thread` has no constructor that takes a
  function, and `std::mutex` and `std::condition_variable` do not exist.
  PSL1GHT has a pthread library (`libpthread.a`, `pthread.h`, every
  function `gthr-posix.h` needs), but it was not there when libstdc++
  was configured, before PSL1GHT in ps3toolchain's order. The port's
  common library and runtime use `std::thread`, `std::mutex` and
  `std::condition_variable` in ten files, the rest of the port in some
  twenty more.
- **newlib 1.20.0 (2012).** No `<fenv.h>` (the EE's rounding mode,
  `runtime/ee.cpp`), and its C99 names (`snprintf`, `strtoll`, `_Exit`,
  `trunc`, `fileno`) are hidden under `-std=c++20`'s strict mode and
  absent from `std::` either way, since libstdc++ was configured without
  C99 stdio and stdlib (`_GLIBCXX11_USE_C99_STDIO` and `_STDLIB`
  undefined). Defining them makes `<cstdio>` and `<cstdlib>` fail on the
  hidden names; `-U__STRICT_ANSI__` exposes them but breaks `<format>`'s
  static assertions. No `<execinfo.h>` (expected; the crash reporter
  does without, as on the console it must).

So with the toolchain as ps3toolchain ships it, nothing of the port's
C++ compiles: every unit includes `<format>` or `<string>`, which reach
the missing C99 names. Two routes, for the study (docs/port/PS3.md,
section 7):

1. **A toolchain of the port's own**, built from ps3toolchain's scripts
   (`LOCAL_TOOLCHAIN_BUILD=1`): a current newlib (4.x; ps3toolchain's
   13,000-line newlib patch is almost all `libgloss/libsysbase`, the
   devkitPro-style system call glue, which devkitPro keeps current for
   newer newlibs), and libstdc++ configured after PSL1GHT with
   `--enable-threads=posix` over its pthread. GCC itself stays
   ps3toolchain's 13.2.0 with its PS3 patch.
2. **The port's own layer**: a thread, mutex and condition variable over
   PSL1GHT's pthread or lv2, `<fenv.h>` over `mtfsf`, and the C99 names
   declared by hand. Every library the port brings (Luau, the ASTC and
   BC decoders) would need the same, and `std::format` and `std::string`
   would still not compile without the libstdc++ configuration, so this
   route is not enough on its own.
