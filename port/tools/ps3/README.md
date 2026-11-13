# The PS3 toolchain

The port built for the PlayStation 3 with the homebrew toolchain
(docs/port/PS3.md, section 7): ps3toolchain's GCC 13.2 for the Cell's
PPU, newlib, PSL1GHT and ps3libraries, in a container. The first target
is the runtime and the common library compiled against PSL1GHT; the
games, the renderer and the window come after (section 7's order).

## The container

```sh
podman build -t openrac-ps3 port/tools/ps3        # two to three hours: ps3dev, then the port's libraries
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
cmake --build build/ps3 --target openrac_common openrac_runtime
```

`ppu.cmake` is the CMake toolchain file: `ppu-gcc` and `ppu-g++` from
`$PS3DEV`, PSL1GHT's `ppu_rules` flags (`-mcpu=cell -mhard-float
-fmodulo-sched -ffunction-sections -fdata-sections`), its headers and
libraries, a bare-metal system (`Generic`) whose compiler checks build a
static library, and `sprxlinker` after every program's link, as
`ppu_rules` has it. `OPENRAC_PS3` is set for the port's CMake: no test
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

## Route 1: the toolchain's libraries, built for the port

`toolchain.sh`, run inside the openrac-ps3 container, builds over
ps3toolchain's prebuilt compiler:

1. **newlib 4.6**, devkitPro's fork at a pinned commit (it carries the
   PowerPC `fenv` and the C99 names in `std`), with the console's pieces
   put in by `newlib-lv2.py` from `newlib-lv2/`: the host configuration
   of `powerpc64-ps3-elf` (`configure.host`), the reentrancy structure
   from a function per thread and malloc's 16-byte alignment
   (`sys/config.h`), 64-bit file offsets (`machine/_types.h`), and the
   `dirent`, `utime` and `__syscalls` headers, with newlib's `libc/syscalls`
   wrappers (the plain `open`, `write` and the rest over the reentrant
   ones). newlib's own pthread types and POSIX thread feature macros are
   left out for the console (its pthread is pthreads-embedded's), and
   devkitPro's Wii headers in its PowerPC machine directory (`sys/lock.h`,
   `sys/stdio.h`, `sys/dirent.h`) are dropped for newlib's generic ones
   with retargetable locks. What PSL1GHT's headers and sources still take
   from newlib 1.20 is kept for the console: `struct stat` with `time_t`
   fields (its file system structure names fields `st_atime`, which 4.6's
   macros would rewrite), `fd_set`, `NFDBITS` and `howmany` in
   `sys/types.h`, `MAXPATHLEN` from `unistd.h`, the declaration macros
   `_DEFUN` and `_AND` in `_ansi.h`, and `sys/select.h` left to PSL1GHT's
   own. `openat` and `fdopendir` are declared for every target but the
   console, which has no directory-relative open: libstdc++'s configure
   decides `std::filesystem`'s ways by what the headers declare, and its
   directory iteration would otherwise call them and never link. newlib's
   `remove()` retries a failed unlink as rmdir whatever the failure; the
   console's stops at a path that is not there (RPCS3 throws on the rmdir
   of one, and the port removes its old logs by name without looking).
   Its table of exit handlers grows past the first 32 only where the
   system provides malloc; the console's grows it with newlib's own (a
   C++ program fills 32 with its statics' destructors, and the port's
   `atexit` for the end of a run was refused).
   Nothing touches newlib's `configure.ac` or `Makefile.am`, so no
   autotools run.
2. **GCC 13.2 again on it**, ps3toolchain's step with
   `--enable-threads=posix` (its bare `--enable-threads` leaves this
   target on the single model), newlib's retargetable locks, and
   newlib's own libm in place of mathfp; and one fix to the compiler
   (`gcc-13.2.0-savres.patch`): the target's libgcc has no out-of-line
   register save routines, yet the backend chose them for a function
   optimised for size and stopped with an internal error (newlib 4.6's
   vfprintf), so every save stays in line. pthreads-embedded's headers
   (`pthread.h`, `pte_types.h`, `sched.h`, `semaphore.h`, which
   ps3libraries installs into `ppu/include`) are copied in beside the
   in-tree newlib headers, so that libstdc++'s `gthr-posix.h` and libgcc
   are built against the pthread the console has. Those headers have no
   C++ linkage of their own (a C++ include got mangled names), so each is
   wrapped in `extern "C"` around the original, renamed `*.pte.h`; and
   pthreads-embedded's `pthread_t` is a struct (a pointer and a reuse
   count), which libstdc++ compares as a scalar in `std::thread::id`'s
   `==`, `<` and `<=>`, so the wrapper of `pthread.h` defines those for
   it in C++, inside its include guard (a program includes `<pthread.h>`
   itself, then libstdc++'s `<thread>` includes it again).
3. **libsysbase**, the system call glue (`newlib-lv2/sysbase`,
   `make install`): newlib's `_open_r` and the rest over the
   `__syscalls` table that PSL1GHT's librt fills, `__getreent` in
   thread-local storage, `realpath`, and newlib's retargetable locks over
   lv2's lightweight mutexes (`retarget_lock.c`: newlib's named locks,
   created on first use under one meta lock), and what libstdc++'s
   `std::filesystem` links that lv2 has not (`posix_extra.c`: `lstat` as
   `stat`, `unlinkat` over `unlink` and `rmdir` for a plain path,
   `pathconf`'s path length, and `symlink`, `readlink`, `fchmod` and
   `statvfs` answering `ENOSYS`; and `rename` removing a destination
   that exists, as POSIX has it, where lv2 refuses with `EEXIST`). The sources are ps3toolchain's
   `libgloss/libsysbase` and `libc/sys/lv2` for newlib 1.20, with the
   declaration macros that newlib dropped since given back by
   `ansi_compat.h`; GCC's link spec for the target names the library
   (`-lsysbase -lc -lrt -llv2`).
4. **PSL1GHT again**, on the new C library, and of ps3libraries zlib and
   pthreads-embedded only (ps3dev's stages 2 and 3, `PS3LIBRARIES_STAGES`):
   the other forty are old ports (libunrar, polarssl) that do not compile
   against newlib 4.6, and the port has no use for them.
   pthreads-embedded is then built again with two changes
   (`pthread-emb-lv2.py`, on the source ps3libraries left unpacked): it
   calls its own `pthread_init` in a constructor of priority 105, after
   PSL1GHT's that wires the heap's system calls (104) and before the
   program's; and until then `pthread_mutex_lock`, `_trylock`,
   `_timedlock` and `_unlock` do nothing and succeed. A program that
   links all of the library has libgcc lock a mutex from the start-up
   code (the registration of the exception tables), before any
   constructor: the library made that mutex on the spot, with locks and
   a heap it did not have yet, and the program stopped before `main`.

`Containerfile` runs the script as its second step, so `podman build`
gives the whole toolchain; while a step is being worked on, the script
runs by hand in a container of the first step's image and resumes (each
step is skipped when its result is there), the container started again
after a fix (`podman start -a`), and snapshotted with `podman commit`
when a probe needs its state. A change to `configure.host` needs
newlib's build directory under the GCC build removed, so that it
configures again; a change to a newlib header needs `make clean` there,
as GCC's build runs without dependency tracking.

**Result (2026-11-11):** `ppu-g++ -v` reports `Thread model: posix` and
newlib 4.6.0; a C++20 program with `std::thread`, `std::mutex`,
`std::condition_variable` and `std::format` links; and the port's
common library and runtime build and archive for the console with the
`ps3` preset (`libopenrac_common.a`, `libopenrac_runtime.a`), the first
of the port's code to compile for the PS3. The game programs, the mods
and the renderer come next (docs/port/PS3.md, section 7).

## The headless game program (2026-11-11)

```sh
cmake --preset ps3 -DOPENRAC_PORT_GAMES=ON   # hostgen needs clang and python3, which the image has
cmake --build build/ps3 --target openrac-rac1-pal
```

`build/ps3/openrac-rac1-pal` is the PAL game's translated C, the mods
(Luau) and `main.cpp` linked against PSL1GHT: a 108 MB ELF, most of it
the game's registration tables and the decompilation's data. What the
console needed of the port, each with its reason at the place:

- `include/` ([README](include/README.md)): headers newlib has under
  another name, on every unit's system include path.
- `_reent`, `_Bigint`, `__sbuf` and `__Long`: the game's own libc
  (`src/core/libc`, translated with the rest) defines them as the PS2's
  libc had them, and newlib's `<math.h>` defines the same names, so
  hostgen renames newlib's around a unit's includes (`lower.py`,
  `LIBC_NAMES_HEAD`).
- Luau's VM uses `LLONG_MIN` without `<climits>` (`cmake/Luau.cmake`);
  `CMAKE_DL_LIBS` is emptied, as CMake names `-ldl` for every GNU target
  (`port/CMakeLists.txt`); the console loads no native mod
  (`mod_library.cpp`) and has no signal for the stuck watch
  (`program/tools.cpp`).
- The console starts a program with no command line and no environment:
  both come from `/app_home/openrac.args`, a file beside the program.
  `common/env` reads it before its first read of a variable, which code
  that runs before `main` makes (`env.h`, `console_file`); `main.cpp`
  takes the command line from there (`program/options.h`).
- Game memory is packed into one allocation (`runtime/memory.cpp`,
  `OPENRAC_PACKED_MEMORY`; docs/port/PS3.md, section 6): the console
  has no 4 GB of address space to reserve.
- All of `libpthread` is linked (`ppu.cmake`, `--whole-archive`):
  libstdc++ reaches it through weak references, which pull nothing out
  of a static library, and `std::thread` then throws "Enable
  multithreading to use std::thread".
- `sprxlinker` after the link (`ppu.cmake`): the linker leaves the count
  of the system libraries' imports at zero in the stub table
  (`.lib.stub`), so the loader resolved none and the program's first
  system call, in librt's `sbrk_init`, jumped into its own stub's code
  (RPCS3: "access violation executing location 0x7c0802a6", the stub's
  first instruction read as an address). With it, 119 imports.

### Running under RPCS3

RPCS3 boots the ELF as it is (`File > Boot ELF`, or `rpcs3 --no-gui
--config FILE program.elf` from a script; `--headless` crashed at start
in the builds tried, 0.0.42). A config copy with `Enable /host_root/:
true` and the Null video and audio renderers runs it headless; the data
folder is then `/host_root/C:/.../port-data` and `/app_home` is the
ELF's folder, where `openrac.args` holds the command line. More than two
arguments after `--` crashed RPCS3 at start, hence the file. The first
boot compiles the program's PowerPC code with LLVM for minutes and caches
the result by the ELF's hash; the port's own log is in the data folder
as on the other platforms, and RPCS3's in its `log` folder. RPCS3's
`unlink` of a `/host_root` file fails with `ENOENT` on Windows (it moves
the file to the mount's root first, which that mount has not), so the
port's log rotation leaves its old logs there; a data folder under
`/dev_hdd0`, as on the console, has none of that.

## The RSX probe (2026-11-13)

[rsx-probe/](rsx-probe/README.md) is the first program here that draws:
a clear and one triangle through PSL1GHT's `librsx`, with programs
assembled by `cgcomp -a` (no Cg toolkit), and the picture read back from
the RSX's memory into a file. It runs under RPCS3 with a real renderer
and `Write Color Buffers: true`.
