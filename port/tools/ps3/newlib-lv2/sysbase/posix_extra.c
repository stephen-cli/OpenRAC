/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * The POSIX calls libstdc++'s filesystem library links, which newlib 4.6 declares and lv2 has no
 * call for: a symbolic link is never made or read, a mode never changed, a volume's space never
 * asked. Each fails as newlib's own stubs do, with ENOSYS, except where lv2 has the plain call:
 * lstat is stat (lv2 follows links itself) and unlinkat at the working directory is unlink or
 * rmdir. pathconf knows the path's length alone. */
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscalls.h>
#include <unistd.h>

int lstat(const char* path, struct stat* buf) {
    return stat(path, buf);
}

int symlink(const char* target, const char* linkpath) {
    (void)target;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char* path, char* buf, size_t size) {
    (void)path;
    (void)buf;
    (void)size;
    errno = ENOSYS;
    return -1;
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int unlinkat(int dirfd, const char* path, int flags) {
    if (dirfd != AT_FDCWD && path[0] != '/') {
        errno = ENOSYS;  /* no directory-relative call on lv2 */
        return -1;
    }
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int statvfs(const char* path, struct statvfs* buf) {
    (void)path;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

long pathconf(const char* path, int name) {
    (void)path;
    if (name == _PC_PATH_MAX) {
        return 1024;  /* MAXPATHLEN, sys/unistd.h (newlib-lv2.py) */
    }
    errno = EINVAL;
    return -1;
}
