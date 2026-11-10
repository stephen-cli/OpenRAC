/* The reentrant system calls newlib 4.6 reaches that ps3toolchain's glue for newlib 1.20 did not
 * define: rmdir and mkdir through the __syscalls table (remove() takes rmdir), open64 as open (the
 * offsets are 64-bit already, machine/_types.h), and fcntl and getentropy, which lv2 has no call
 * for, failing with ENOSYS; and dirfd, which the 1.20 header made a macro and newlib 4.6's dirent.h
 * declares as a function. */
#include "config.h"
#include <errno.h>
#include <reent.h>
#include <sys/syscalls.h>

int _rmdir_r(struct _reent* r, const char* name) {
    if (__syscalls.rmdir_r) {
        return __syscalls.rmdir_r(r, name);
    }
    r->_errno = ENOSYS;
    return -1;
}

int _mkdir_r(struct _reent* r, const char* path, int mode) {
    if (__syscalls.mkdir_r) {
        return __syscalls.mkdir_r(r, path, (mode_t)mode);
    }
    r->_errno = ENOSYS;
    return -1;
}

int _open64_r(struct _reent* r, const char* file, int flags, int mode) {
    if (__syscalls.open_r) {
        return __syscalls.open_r(r, file, flags, mode);
    }
    r->_errno = ENOSYS;
    return -1;
}

int _fcntl_r(struct _reent* r, int fd, int cmd, int arg) {
    (void)fd;
    (void)cmd;
    (void)arg;
    r->_errno = ENOSYS;
    return -1;
}

int _getentropy_r(struct _reent* r, void* buf, size_t n) {
    (void)buf;
    (void)n;
    r->_errno = ENOSYS;
    return -1;
}

int dirfd(DIR* dirp) {
    return dirp->dd_fd;
}
