#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
"""SDL3's source made to compile for the console (the port's CMake runs this on the SDL it fetched
when the target is the PS3). Idempotent.

    sdl3-lv2.py <SDL tree>

SDL has no PS3 platform. The port builds it as a Unix without a desktop (pthreads, the dummy video
and audio drivers; cmake/Dependencies.cmake has the options), which its sources take but for five
places that assume more of the system than lv2 and pthreads-embedded give:

  - a thread's ID is the pthread_t cast to an integer (in two functions), and pthreads-embedded's
    pthread_t is a structure (a pointer and a reuse count): the pointer is the ID;
  - a new thread masks the asynchronous signals with pthread_sigmask, which the console, without
    signals, has not;
  - a delay is nanosleep, or without it a select on no descriptor, which lv2's select (of its
    network library) returns from at once: the console sleeps with usleep;
  - struct tm has no tm_gmtoff in newlib, which SDL reads for the offset from UTC: newlib has no time
    zone on the console, where local time is UTC and the offset zero;
  - the dynamic API (SDL calling itself through a table another SDL can replace) is for shared
    libraries, and its switch is in a header that refuses a command-line definition.
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def replace_once(path, old, new):
    s = path.read_text(encoding="utf-8")
    if new in s:
        return
    if s.count(old) != 1:
        raise SystemExit(f"{path}: {s.count(old)} matches for {old!r}")
    path.write_text(s.replace(old, new), encoding="utf-8", newline="\n")


replace_once(
    tree / "src" / "thread" / "pthread" / "SDL_systhread.c",
    "    return (SDL_ThreadID)pthread_self();\n",
    "#ifdef __lv2ppu__  /* OpenRAC: pthreads-embedded's pthread_t is a structure (sdl3-lv2.py) */\n"
    "    return (SDL_ThreadID)(uintptr_t)pthread_self().p;\n"
    "#else\n"
    "    return (SDL_ThreadID)pthread_self();\n"
    "#endif\n",
)

replace_once(
    tree / "src" / "thread" / "pthread" / "SDL_systhread.c",
    "    thread->threadid = (SDL_ThreadID) thread->handle;  // the SDL thread ID is just the pthread_t.\n",
    "#ifdef __lv2ppu__  /* OpenRAC: as SDL_GetCurrentThreadID below (sdl3-lv2.py) */\n"
    "    thread->threadid = (SDL_ThreadID)(uintptr_t)thread->handle.p;\n"
    "#else\n"
    "    thread->threadid = (SDL_ThreadID) thread->handle;  // the SDL thread ID is just the pthread_t.\n"
    "#endif\n",
)

replace_once(
    tree / "src" / "thread" / "pthread" / "SDL_systhread.c",
    "#ifdef HAVE_SIGNAL_H\n    // Mask asynchronous signals for this thread\n",
    "#if defined(HAVE_SIGNAL_H) && !defined(__lv2ppu__)  // OpenRAC: no signals there (sdl3-lv2.py)\n"
    "    // Mask asynchronous signals for this thread\n",
)

replace_once(
    tree / "src" / "timer" / "unix" / "SDL_systimer.c",
    "        was_error = select(0, NULL, NULL, NULL, &tv);\n",
    "#ifdef __lv2ppu__  /* OpenRAC: lv2's select does not wait (sdl3-lv2.py) */\n"
    "        was_error = usleep((useconds_t)(tv.tv_sec * 1000000 + tv.tv_usec));\n"
    "#else\n"
    "        was_error = select(0, NULL, NULL, NULL, &tv);\n"
    "#endif\n",
)

replace_once(
    tree / "src" / "time" / "unix" / "SDL_systime.c",
    "#if (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))\n",
    "#ifdef __lv2ppu__  /* OpenRAC: newlib's local time is UTC on the console (sdl3-lv2.py) */\n"
    "        dt->utc_offset = 0;\n"
    "#elif (_POSIX_VERSION >= 202405L) || (!defined(sun) && !defined(__sun))\n",
)

replace_once(
    tree / "src" / "dynapi" / "SDL_dynapi.h",
    "#if defined(SDL_PLATFORM_PRIVATE) // probably not useful on private platforms.\n",
    "#if defined(__lv2ppu__) // OpenRAC: the console links SDL statically (sdl3-lv2.py)\n"
    "#define SDL_DYNAMIC_API 0\n"
    "#elif defined(SDL_PLATFORM_PRIVATE) // probably not useful on private platforms.\n",
)
