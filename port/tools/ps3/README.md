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
   own. Nothing touches newlib's `configure.ac` or `Makefile.am`, so no
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
   it in C++.
3. **libsysbase**, the system call glue (`newlib-lv2/sysbase`,
   `make install`): newlib's `_open_r` and the rest over the
   `__syscalls` table that PSL1GHT's librt fills, `__getreent` in
   thread-local storage, `realpath`, and newlib's retargetable locks over
   lv2's lightweight mutexes (`retarget_lock.c`: newlib's named locks,
   created on first use under one meta lock). The sources are
   ps3toolchain's `libgloss/libsysbase` and `libc/sys/lv2` for newlib
   1.20, with the declaration macros that newlib dropped since given
   back by `ansi_compat.h`; GCC's link spec for the target names the
   library (`-lsysbase -lc -lrt -llv2`).
4. **PSL1GHT again**, on the new C library, and of ps3libraries zlib and
   pthreads-embedded only (ps3dev's stages 2 and 3, `PS3LIBRARIES_STAGES`):
   the other forty are old ports (libunrar, polarssl) that do not compile
   against newlib 4.6, and the port has no use for them.

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
