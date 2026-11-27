// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// What the host says of the process (stats.h): its memory and its processor time. Each system has
// its own way; this file holds all three, and includes no header of the port's but stats.h
// (windows.h's macros clash with the port's names).

#include "common/stats.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(__lv2ppu__)
// The console: lv2 says how much of the process's share of main memory is left; its times it
// keeps to itself.
#include <malloc.h>

#include <ppu-lv2.h>
#else
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace openrac::stats {

#if defined(__lv2ppu__)
namespace {

struct UserMemory {
    std::uint32_t total;
    std::uint32_t available;
};

// sys_memory_get_user_memory_size, which PSL1GHT has no name for.
s32 user_memory(UserMemory* memory) {
    lv2syscall1(352, (u64) memory);
    return_to_user_prog(s32);
}

}  // namespace
#endif

ProcessMemory process_memory() {
    ProcessMemory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        m.resident = counters.WorkingSetSize;
        m.peak = counters.PeakWorkingSetSize;
        m.footprint = counters.PagefileUsage;
    }
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        m.resident = info.resident_size;
        m.footprint = info.phys_footprint;
        m.peak = info.resident_size_peak;
    }
#elif defined(__lv2ppu__)
    UserMemory memory{};
    if (user_memory(&memory) == 0) {
        m.resident = memory.total - memory.available;
        m.available = memory.available;
    }
    const struct mallinfo heap = mallinfo();
    m.heap = static_cast<std::uint64_t>(heap.arena);
    m.heap_free = static_cast<std::uint64_t>(heap.fordblks);
#else
    // /proc/self/statm: sizes in pages; the second is the resident set.
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long long total = 0, resident = 0;
        if (std::fscanf(f, "%llu %llu", &total, &resident) == 2) {
            m.resident = resident * static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
        }
        std::fclose(f);
    }
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        m.peak = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;  // kilobytes on Linux
    }
#endif
    return m;
}

double process_cpu_seconds() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1.0e-7;  // 100 ns units
#elif defined(__lv2ppu__)
    return 0.0;
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
           + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1.0e-6;
#endif
}

}  // namespace openrac::stats
