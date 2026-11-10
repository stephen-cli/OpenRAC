/* newlib's retargetable locks (--enable-newlib-retargetable-locking, sys/lock.h) over lv2's
 * lightweight mutexes: the locks newlib names (malloc's, stdio's, atexit's and the rest, defined
 * here as sys/lock.h declares them) and the ones it makes at run time. A named lock is a zero-filled
 * structure until its first use, when it is created under one meta lock, as ps3toolchain's
 * libc/sys/lv2 did for its __libc_lock_t. Every lock is recursive, as those were. */
#include <stdlib.h>
#include <sys/lock.h>
#include "lv2lock.h"

struct __lock {
    struct _sys_lwmutex mutex;
};

struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct _sys_lwmutex meta_lock;

static void init_meta_lock(void) __attribute__((constructor(105)));

static void init_meta_lock(void) {
    if (sys_lwmutex_create(&meta_lock, &__libc_lock_attributes)) {
        abort();
    }
}

/* Creates a lock that is still zero-filled, once, whichever thread comes first. */
static void ensure_created(_LOCK_T lock) {
    if (!SYS_LWMUTEX_UNINITIALIZED(&lock->mutex)) {
        return;
    }
    if (sys_lwmutex_lock(&meta_lock, 0)) {
        abort();
    }
    if (SYS_LWMUTEX_UNINITIALIZED(&lock->mutex)
        && sys_lwmutex_create(&lock->mutex, &__libc_lock_attributes)) {
        abort();
    }
    if (sys_lwmutex_unlock(&meta_lock)) {
        abort();
    }
}

void __retarget_lock_init(_LOCK_T* lock) {
    *lock = calloc(1, sizeof **lock);
    if (*lock == NULL) {
        abort();
    }
    ensure_created(*lock);
}

void __retarget_lock_init_recursive(_LOCK_T* lock) {
    __retarget_lock_init(lock);
}

void __retarget_lock_close(_LOCK_T lock) {
    if (!SYS_LWMUTEX_UNINITIALIZED(&lock->mutex)) {
        sys_lwmutex_destroy(&lock->mutex);
    }
    free(lock);
}

void __retarget_lock_close_recursive(_LOCK_T lock) {
    __retarget_lock_close(lock);
}

void __retarget_lock_acquire(_LOCK_T lock) {
    ensure_created(lock);
    sys_lwmutex_lock(&lock->mutex, 0);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock) {
    __retarget_lock_acquire(lock);
}

int __retarget_lock_try_acquire(_LOCK_T lock) {
    ensure_created(lock);
    return sys_lwmutex_trylock(&lock->mutex) == 0;
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock) {
    return __retarget_lock_try_acquire(lock);
}

void __retarget_lock_release(_LOCK_T lock) {
    sys_lwmutex_unlock(&lock->mutex);
}

void __retarget_lock_release_recursive(_LOCK_T lock) {
    __retarget_lock_release(lock);
}
