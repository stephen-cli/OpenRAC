/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * <endian.h> for the PS3 (README.md): newlib has FreeBSD's <sys/endian.h>, with htole32 and the
 * rest, where glibc has this name; Luau's buffer library includes it on a big-endian host. */
#pragma once
#include <sys/endian.h>
