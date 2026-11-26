#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the OpenRAC contributors
"""pthreads-embedded made usable from a program's first instruction (toolchain.sh runs this on the
source ps3libraries unpacked, then builds and installs the library again). Idempotent.

    pthread-emb-lv2.py <pthread-emb tree>

The library wants pthread_init() called before anything else of it, and creates a mutex that was
initialised statically on its first lock, with locks of its own that pthread_init() makes and on the
heap. A program that links all of it (ppu.cmake) has libgcc lock such a mutex from the start-up
code, when it registers the exception tables: before any constructor, so before PSL1GHT has wired
the heap's system calls (its constructor of priority 104) and before anyone could call
pthread_init(). Two changes:

  - the library initialises itself in a constructor of priority 105, after PSL1GHT's and before
    every constructor of the program and of libstdc++;
  - until then the mutex calls do nothing and succeed: the program has one thread, which no lock
    has to keep out.

And the library's waits are made to sleep. A wait that pthread_cancel may end (a condition
variable's, so every std::condition_variable) and a join are loops in its PSL1GHT layer that ask
and yield without a pause: each waiting thread keeps one of the console's two hardware threads
busy, and a thread of lower priority than two waiting ones never runs (the game's sound mixer at
the exit: the program did not end). Two more changes, in platform/psl1ght/psl1ght_osal.c:

  - the wait on a semaphore sleeps on it, in slices of 10 ms between which it looks at the
    thread's cancel semaphore, and counts its timeout by the system's time;
  - the join sleeps 1 ms between its looks at the thread.

And a priority is turned over on its way to the system and back. A pthread's larger number is
the more urgent thread; lv2's is the less urgent one (0 first, 3071 last), and the layer handed
the number on as it was: SDL's time-critical threads (the sound device's, the game's mixer) were
the last to run. The default, 1001 as lv2 counts and what the system starts a program's first
thread with, is 2070 as pthreads count.
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


for name in ["lock", "unlock", "trylock", "timedlock"]:
    replace_once(
        tree / f"pthread_mutex_{name}.c",
        "  pthread_mutex_t mx;\n",
        "  pthread_mutex_t mx;\n"
        "\n"
        "  /* OpenRAC: before pthread_init() the program has one thread (pthread-emb-lv2.py). */\n"
        "  if (!pte_processInitialized)\n"
        "    {\n"
        "      return 0;\n"
        "    }\n",
    )

replace_once(
    tree / "pthread_init.c",
    "  return (pte_processInitialized);\n\n}\n",
    "  return (pte_processInitialized);\n\n}\n"
    "\n"
    "/* OpenRAC: the library initialises itself, after PSL1GHT has wired the heap's system calls (its\n"
    " * constructor of priority 104) and before the program's constructors (pthread-emb-lv2.py). */\n"
    "static void pte_initialise_at_start (void) __attribute__ ((constructor (105)));\n"
    "static void\n"
    "pte_initialise_at_start (void)\n"
    "{\n"
    "  pthread_init ();\n"
    "}\n",
)

osal = tree / "platform" / "psl1ght" / "psl1ght_osal.c"
YIELD = "                      sysThreadYield ();\n"
MARK = "/* OpenRAC: the waits sleep (pthread-emb-lv2.py). */\n"
source = osal.read_text(encoding="utf-8")
if MARK not in source:
    head, pend_name, pend = source.partition(
        "pte_osResult pte_osSemaphoreCancellablePend(pte_osSemaphoreHandle semHandle,"
    )
    if not pend_name or head.count(YIELD) != 1 or pend.count(YIELD) != 1:
        raise SystemExit(f"{osal}: the waiting loops are not as expected")
    # The join: a pause between its looks.
    head = head.replace(YIELD, "                      sysUsleep (1000);\n")
    # The wait on a semaphore: asleep on it for a slice, the timeout by the system's time.
    for old, new in [
        ("  start_time = clock();\n", "  start_time = (clock_t) sysGetSystemTime ();\n"),
        (
            "      status = sysSemTryWait(semHandle);\n",
            "      {\n"
            "        u64 slice = 10000;\n"
            "        if (timeoutEnabled)\n"
            "          {\n"
            "            u64 gone = (u64) sysGetSystemTime () - (u64) start_time;\n"
            "            slice = gone >= timeout ? 0 : (timeout - gone < slice ? timeout - gone : slice);\n"
            "          }\n"
            "        status = slice == 0 ? sysSemTryWait(semHandle) : sysSemWait(semHandle, slice);\n"
            "      }\n",
        ),
        (
            "      else if ((timeoutEnabled) && ((clock() - start_time) > timeout))\n",
            "      else if ((timeoutEnabled) && (((u64) sysGetSystemTime () - (u64) start_time) >= timeout))\n",
        ),
        (YIELD, "                      ;\n"),
    ]:
        if pend.count(old) != 1:
            raise SystemExit(f"{osal}: {pend.count(old)} matches for {old!r}")
        pend = pend.replace(old, new)
    source = head + pend_name + pend
    include = '#include "pte_osal.h"\n'
    if source.count(include) != 1:
        raise SystemExit(f"{osal}: no include to follow")
    source = source.replace(include, include + MARK + "#include <sys/systime.h>\n", 1)
    osal.write_text(source, encoding="utf-8", newline="\n")

replace_once(
    osal,
    "  sysThreadSetPriority(osThreadHandle, pThreadData->priority);\n",
    "  /* OpenRAC: lv2 counts priorities the other way (pthread-emb-lv2.py). */\n"
    "  sysThreadSetPriority(osThreadHandle, OS_MAX_PRIO - pThreadData->priority);\n",
)
replace_once(
    osal,
    "  sysThreadGetPriority (threadHandle, &priority);\n  return priority;\n",
    "  sysThreadGetPriority (threadHandle, &priority);\n"
    "  return OS_MAX_PRIO - priority;  /* OpenRAC: as pte_osThreadStart (pthread-emb-lv2.py) */\n",
)
replace_once(
    osal,
    "  sysThreadSetPriority (threadHandle, newPriority);\n",
    "  /* OpenRAC: as pte_osThreadStart (pthread-emb-lv2.py) */\n"
    "  sysThreadSetPriority (threadHandle, OS_MAX_PRIO - newPriority);\n",
)
replace_once(
    tree / "platform" / "psl1ght" / "psl1ght_osal.h",
    "#define OS_DEFAULT_PRIO 1001\n",
    "/* OpenRAC: lv2's 1001, as pthreads count (pthread-emb-lv2.py) */\n"
    "#define OS_DEFAULT_PRIO (OS_MAX_PRIO - 1001)\n",
)
