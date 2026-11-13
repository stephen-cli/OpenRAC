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
