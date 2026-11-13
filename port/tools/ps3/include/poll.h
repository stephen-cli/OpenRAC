/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * <poll.h> for the PS3 (README.md): lv2 polls sockets alone, through its network library, and the
 * port waits on no descriptor there. SDL's Unix code compiles its wait on one (SDL_IOReady), which
 * nothing of the console's build calls: the wait fails. */
#pragma once

#include <errno.h>

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

static inline int poll(struct pollfd* fds, nfds_t count, int timeout) {
    (void)fds;
    (void)count;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}
