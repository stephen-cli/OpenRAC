/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * <dlfcn.h> for the PS3 (README.md): the console loads no shared object into a running program, so
 * every load fails here. SDL's Unix code includes the header to look for GTK at run time. */
#pragma once

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_NOLOAD 4
#define RTLD_DEFAULT ((void*)0)

static inline void* dlopen(const char* file, int mode) {
    (void)file;
    (void)mode;
    return 0;
}

static inline void* dlsym(void* handle, const char* name) {
    (void)handle;
    (void)name;
    return 0;
}

static inline int dlclose(void* handle) {
    (void)handle;
    return -1;
}

static inline char* dlerror(void) {
    return (char*)"the console loads no shared objects";
}
