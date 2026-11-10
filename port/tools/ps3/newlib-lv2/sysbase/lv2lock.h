/* lv2's lightweight mutex as libc's lock: the structure lv2 fills (24 bytes, as PSL1GHT's
 * sys_lwmutex_t), its creation attributes, and the five calls lock.c routes through the __syscalls
 * table to PSL1GHT's librt. From ps3toolchain's libc/sys/lv2 (lock_internal.h, sys/lock.h). */
#ifndef OPENRAC_LV2LOCK_H
#define OPENRAC_LV2LOCK_H
#include <stdint.h>

#define SYS_LWMUTEX_ATTR_PROTOCOL 0x0002
#define SYS_LWMUTEX_ATTR_RECURSIVE 0x0010
/* lv2 writes the attribute on creation; a zero-filled mutex is one not created yet. */
#define SYS_LWMUTEX_UNINITIALIZED(plwm) (!((plwm)->attribute))

struct _sys_lwmutex {
    uint64_t lock_var;
    uint32_t attribute;
    uint32_t recursive_count;
    uint32_t sleep_queue;
    uint32_t _pad;
};

typedef struct _sys_lwmutex sys_lwmutex_t;

typedef struct _sys_lwmutex_attr {
    uint32_t attr_protocol;
    uint32_t attr_recursive;
    char name[8];
} sys_lwmutex_attr_t;

static const sys_lwmutex_attr_t __libc_lock_attributes = {
    SYS_LWMUTEX_ATTR_PROTOCOL, SYS_LWMUTEX_ATTR_RECURSIVE, ""
};

extern int32_t sys_lwmutex_create(sys_lwmutex_t* lwmutex, const sys_lwmutex_attr_t* attr);
extern int32_t sys_lwmutex_destroy(sys_lwmutex_t* lwmutex);
extern int32_t sys_lwmutex_lock(sys_lwmutex_t* lwmutex, uint64_t timeout);
extern int32_t sys_lwmutex_trylock(sys_lwmutex_t* lwmutex);
extern int32_t sys_lwmutex_unlock(sys_lwmutex_t* lwmutex);
#endif
