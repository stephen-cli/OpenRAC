// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// Game memory. The whole 32-bit address space is reserved so that any game
// address is base + address, with no check and no branch; only the regions
// in guest.h's map are backed by pages. Main RAM is one shared object mapped
// three times (cached, uncached, uncached-accelerated), so a write through
// one alias is read through the others, as on the console.
//
// Packed (OPENRAC_PACKED_MEMORY, the PS3), there is no reservation: the regions
// sit side by side in one allocation, main RAM first, and guest.h's fold table
// takes each game address to its place there.

#include "openrac/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "common/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__lv2ppu__)
#include <sys/memory.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#endif

extern "C" {
std::uint8_t* openrac_guest_base = nullptr;
std::uint32_t openrac_guest_ram_size = GUEST_RAM_SIZE;
#if defined(OPENRAC_PACKED_MEMORY)
std::uintptr_t openrac_guest_fold[1u << (32 - OPENRAC_GUEST_FOLD_SHIFT)];
#endif
}

namespace openrac::runtime {
namespace {

constexpr std::uint64_t kReservation = 0x100000000ull;  // 4 GB: every gaddr

struct Region {
    gaddr address;
    std::uint32_t size;
};

// Everything but main RAM and its aliases, which are mapped separately.
constexpr Region kPlainRegions[] = {
    {GUEST_EE_REGS, GUEST_EE_REGS_SIZE},
    {GUEST_VU_MEM, GUEST_VU_MEM_SIZE},
    {GUEST_GS_REGS, GUEST_GS_REGS_SIZE},
    {GUEST_SCRATCHPAD, GUEST_SCRATCHPAD_SIZE},
    {GUEST_PORT, GUEST_PORT_SIZE},
};

constexpr gaddr kRamViews[] = {GUEST_RAM, GUEST_RAM_UNCACHED, GUEST_RAM_UNCACHED_ACCEL};

Memory* g_memory = nullptr;

std::uint32_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;  // views and placeholders split at 64 KB
#elif defined(__lv2ppu__)
    return 0x10000;  // lv2's smallest page
#else
    return static_cast<std::uint32_t>(sysconf(_SC_PAGESIZE));
#endif
}

std::uint32_t round_up(std::uint32_t size, std::uint32_t page) {
    return (size + page - 1) / page * page;
}

// The host's bytes at a game address: G() for this file's arithmetic.
std::uint8_t* host(gaddr address) {
    return static_cast<std::uint8_t*>(G(address));
}

}  // namespace

#if defined(OPENRAC_PACKED_MEMORY)

namespace {

constexpr std::uint32_t kFoldStep = 1u << OPENRAC_GUEST_FOLD_SHIFT;  // game addresses per entry

// An entry of the fold table leads to one place, so a region starts where an entry does and no two
// share one. A region shorter than its entry's reach leaves the rest of it leading into whatever
// follows in the allocation: Memory::mapped() knows the region's end, a stray access there does not
// stop the program.
constexpr bool regions_fold() {
    for (const Region& r : kPlainRegions) {
        if (r.address % kFoldStep != 0) {
            return false;
        }
    }
    for (gaddr view : kRamViews) {
        if (view % kFoldStep != 0) {
            return false;
        }
    }
    return true;
}

static_assert(regions_fold(), "every region of game memory starts on a fold step");

// The allocation, main RAM at its start, and where each plain region starts in it.
std::uint8_t* g_packed = nullptr;
std::uint32_t g_packed_offset[std::size(kPlainRegions)];

// A fold step of address space with nothing behind it, where the entries without a region lead.
std::uint8_t* reserve_nothing() {
#if defined(__lv2ppu__)
    // lv2 hands address space out 256 MB at a time and maps memory into it only when asked.
    sys_mem_addr_t at = 0;
    const s32 status = sysMMapperAllocateAddress(
        0x10000000, SYS_MEMORY_PAGE_SIZE_1M | SYS_MEMORY_ACCESS_RIGHT_ANY, 0x10000000, &at
    );
    return status == 0 ? reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(at)) : nullptr;
#elif defined(_WIN32)
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kFoldStep, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* at =
        mmap(nullptr, kFoldStep, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return at == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(at);
#endif
}

// The game address of a host address in the allocation (an alias of RAM has RAM's own), or false.
bool unfold(const std::uint8_t* p, std::uint32_t ram_size, gaddr* address) {
    if (g_packed == nullptr || p < g_packed) {
        return false;
    }
    const std::uintptr_t offset = static_cast<std::uintptr_t>(p - g_packed);
    if (offset < ram_size) {
        *address = GUEST_RAM + static_cast<gaddr>(offset);
        return true;
    }
    for (std::size_t i = 0; i < std::size(kPlainRegions); ++i) {
        if (offset >= g_packed_offset[i] && offset - g_packed_offset[i] < kPlainRegions[i].size) {
            *address = kPlainRegions[i].address + static_cast<gaddr>(offset - g_packed_offset[i]);
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" gaddr openrac_guest_address(const void* host) {
    gaddr address = 0;
    if (!unfold(static_cast<const std::uint8_t*>(host), openrac_guest_ram_size, &address)) {
        log::fatal("a host address outside game memory was asked for its game address");
    }
    return address;
}

void Memory::map_all() {
    if (ram_size_ != GUEST_RAM_SIZE) {
        log::fatal("packed game memory has no room for the expanded arena");
    }
    const std::uint32_t page = page_size();
    std::uint32_t size = round_up(ram_size_, page);
    for (std::size_t i = 0; i < std::size(kPlainRegions); ++i) {
        g_packed_offset[i] = size;
        size += round_up(kPlainRegions[i].size, page);
    }
    // newlib declares aligned_alloc in C, not in std (libstdc++ was built without it).
    g_packed = static_cast<std::uint8_t*>(::aligned_alloc(page, size));
    if (g_packed == nullptr) {
        log::fatalf("could not allocate {} MB of game memory", size >> 20);
    }
    const std::uint8_t* nothing = reserve_nothing();
    if (nothing == nullptr) {
        log::fatal("could not reserve the address space that unmapped game addresses lead to");
    }
    for (std::size_t entry = 0; entry < std::size(openrac_guest_fold); ++entry) {
        openrac_guest_fold[entry] =
            reinterpret_cast<std::uintptr_t>(nothing) - entry * std::uintptr_t{kFoldStep};
    }
    auto lead = [](gaddr address, std::uint32_t length, const std::uint8_t* to) {
        for (std::uint32_t done = 0; done < length; done += kFoldStep) {
            openrac_guest_fold[(address + done) >> OPENRAC_GUEST_FOLD_SHIFT] =
                reinterpret_cast<std::uintptr_t>(to) - address;
        }
    };
    for (gaddr view : kRamViews) {
        lead(view, ram_size_, g_packed);
    }
    for (std::size_t i = 0; i < std::size(kPlainRegions); ++i) {
        lead(kPlainRegions[i].address, kPlainRegions[i].size, g_packed + g_packed_offset[i]);
    }
    base_ = g_packed;
    aliases_ = true;
}

#elif defined(_WIN32)

namespace {

// VirtualAlloc2 and MapViewOfFile3 (Windows 10 1803) are looked up at run
// time so the port still starts, without the aliases, on older systems.
using VirtualAlloc2Fn =
    PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(
    HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG
);

bool map_with_placeholders(std::uint8_t** base_out, std::uint32_t ram_size) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase == nullptr) {
        return false;
    }
    auto virtual_alloc2 =
        reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
    auto map_view3 =
        reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
    if (virtual_alloc2 == nullptr || map_view3 == nullptr) {
        return false;
    }
    auto* base = static_cast<std::uint8_t*>(virtual_alloc2(
        nullptr,
        nullptr,
        kReservation,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS,
        nullptr,
        0
    ));
    if (base == nullptr) {
        return false;
    }
    const std::uint32_t page = page_size();
    // Split the placeholder so that each region is a placeholder of its own.
    auto carve = [&](gaddr address, std::uint32_t size) {
        return VirtualFree(base + address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
    };
    HANDLE ram =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ram_size, nullptr);
    if (ram == nullptr) {
        return false;
    }
    for (gaddr view : kRamViews) {
        if (!carve(view, ram_size)
            || map_view3(
                   ram,
                   GetCurrentProcess(),
                   base + view,
                   0,
                   ram_size,
                   MEM_REPLACE_PLACEHOLDER,
                   PAGE_READWRITE,
                   nullptr,
                   0
               ) == nullptr) {
            return false;
        }
    }
    for (const Region& r : kPlainRegions) {
        const std::uint32_t size = round_up(r.size, page);
        if (!carve(r.address, size)
            || virtual_alloc2(
                   GetCurrentProcess(),
                   base + r.address,
                   size,
                   MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                   PAGE_READWRITE,
                   nullptr,
                   0
               ) == nullptr) {
            return false;
        }
    }
    *base_out = base;
    return true;
}

}  // namespace

void Memory::map_all() {
    if (map_with_placeholders(&base_, ram_size_)) {
        aliases_ = true;
        return;
    }
    // Without placeholders: no aliases, every region committed on its own.
    base_ =
        static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kReservation, MEM_RESERVE, PAGE_NOACCESS));
    if (base_ == nullptr) {
        log::fatal("could not reserve 4 GB of address space for game memory");
    }
    const std::uint32_t page = page_size();
    if (VirtualAlloc(base_ + GUEST_RAM, ram_size_, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        log::fatal("could not commit game RAM");
    }
    for (const Region& r : kPlainRegions) {
        if (VirtualAlloc(base_ + r.address, round_up(r.size, page), MEM_COMMIT, PAGE_READWRITE)
            == nullptr) {
            log::fatalf("could not commit game memory at {:#x}", r.address);
        }
    }
    aliases_ = false;
    log::warn("this Windows has no memory placeholders (before 10 1803): uncached RAM addresses "
              "are not mapped");
}

#else  // POSIX

namespace {

int ram_object() {
#if defined(__ANDROID__) && __ANDROID_API__ < 30
    // bionic declares memfd_create from API 30; the kernel call is older (Linux 3.17).
    return static_cast<int>(syscall(__NR_memfd_create, "openrac-ram", 0));
#elif defined(__linux__)
    return memfd_create("openrac-ram", 0);
#else
    char name[64];
    std::snprintf(name, sizeof name, "/openrac-ram-%d", static_cast<int>(getpid()));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
    }
    return fd;
#endif
}

bool map_fixed(std::uint8_t* at, std::size_t size, int fd) {
    const int flags =
        fd >= 0 ? (MAP_SHARED | MAP_FIXED) : (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    void* p = mmap(at, size, PROT_READ | PROT_WRITE, flags, fd, 0);
    return p == at;
}

}  // namespace

void Memory::map_all() {
    void* reserved =
        mmap(nullptr, kReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED) {
        log::fatal("could not reserve 4 GB of address space for game memory");
    }
    base_ = static_cast<std::uint8_t*>(reserved);

    aliases_ = false;
    int fd = ram_object();
    if (fd >= 0 && ftruncate(fd, ram_size_) == 0) {
        aliases_ = true;
        for (gaddr view : kRamViews) {
            if (!map_fixed(base_ + view, ram_size_, fd)) {
                aliases_ = false;
                break;
            }
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (!aliases_) {
        if (!map_fixed(base_ + GUEST_RAM, ram_size_, -1)) {
            log::fatal("could not map game RAM");
        }
        log::warn("main RAM could not be aliased: uncached RAM addresses are not mapped");
    }
    const std::uint32_t page = page_size();
    for (const Region& r : kPlainRegions) {
        if (!map_fixed(base_ + r.address, round_up(r.size, page), -1)) {
            log::fatalf("could not map game memory at {:#x}", r.address);
        }
    }
}

#endif

Memory& Memory::create(std::uint32_t ram_size) {
    if (ram_size != GUEST_RAM_SIZE && ram_size != GUEST_RAM_EXPANDED_SIZE) {
        log::fatalf("game RAM size {:#x} is outside the supported range", ram_size);
    }
    if (g_memory != nullptr) {
        if (g_memory->ram_size_ != ram_size) {
            log::fatal("game memory was already created with a different RAM size");
        }
        return *g_memory;
    }
    g_memory = new Memory();
    g_memory->ram_size_ = ram_size;
    g_memory->map_all();
    openrac_guest_base = g_memory->base_;
    openrac_guest_ram_size = ram_size;
    g_memory->clear();
    return *g_memory;
}

Memory& Memory::get() {
    if (g_memory == nullptr) {
        log::fatal("game memory used before Memory::create");
    }
    return *g_memory;
}

bool Memory::mapped(gaddr address, std::size_t size) const {
    const std::uint64_t begin = address;
    const std::uint64_t end = begin + size;
    auto inside = [&](gaddr at, std::uint32_t length) {
        return begin >= at && end <= static_cast<std::uint64_t>(at) + length;
    };
    if (inside(GUEST_RAM, ram_size_)) {
        return true;
    }
    if (aliases_
        && (inside(GUEST_RAM_UNCACHED, ram_size_) || inside(GUEST_RAM_UNCACHED_ACCEL, ram_size_))) {
        return true;
    }
    for (const Region& r : kPlainRegions) {
        if (inside(r.address, r.size)) {
            return true;
        }
    }
    return false;
}

std::span<std::uint8_t> Memory::bytes(gaddr address, std::size_t size) {
    if (!mapped(address, size)) {
        log::fatalf("game memory {:#010x}+{:#x} is not mapped", address, size);
    }
    return {host(address), size};
}

void Memory::clear() {
    std::memset(host(GUEST_RAM), 0, ram_size_);
    for (const Region& r : kPlainRegions) {
        std::memset(host(r.address), 0, r.size);
    }
    openrac_guest_sp = GUEST_STACK_TOP;
}

std::size_t Memory::snapshot_size() {
    std::size_t size = openrac_guest_ram_size;
    for (const Region& r : kPlainRegions) {
        size += r.size;
    }
    return size;
}

bool Memory::save_snapshot(std::span<std::uint8_t> out) const {
    if (out.size() != snapshot_size()) {
        return false;
    }
    std::size_t at = 0;
    std::memcpy(out.data() + at, host(GUEST_RAM), ram_size_);
    at += ram_size_;
    for (const Region& r : kPlainRegions) {
        std::memcpy(out.data() + at, host(r.address), r.size);
        at += r.size;
    }
    return true;
}

bool Memory::restore_snapshot(
    std::span<const std::uint8_t> in, const openrac_guest_allocation_state& allocations
) {
    try {
        if (in.size() != snapshot_size()) {
            return false;
        }
        openrac_guest_allocation_state current{};
        openrac_guest_get_allocation_state(&current);
        if (allocations.strings_end < GUEST_STRINGS
            || allocations.strings_end > GUEST_STRINGS + GUEST_STRINGS_SIZE
            || current.strings_end > GUEST_STRINGS + GUEST_STRINGS_SIZE
            || allocations.statics_end < GUEST_STATICS
            || current.statics_end > GUEST_STATICS + GUEST_STATICS_SIZE
            || allocations.statics_end > allocations.static_sites_begin
            || current.statics_end > current.static_sites_begin
            || current.static_sites_begin < GUEST_STATICS
            || current.static_sites_begin > GUEST_STATICS + GUEST_STATICS_SIZE
            || allocations.static_sites_begin > GUEST_STATICS + GUEST_STATICS_SIZE) {
            return false;
        }
        const std::size_t strings_tail_size = allocations.strings_end <= current.strings_end
                                                  ? current.strings_end - allocations.strings_end
                                                  : 0;
        const std::size_t statics_tail_size = allocations.statics_end <= current.statics_end
                                                  ? current.statics_end - allocations.statics_end
                                                  : 0;
        const std::size_t static_sites_tail_size =
            current.static_sites_begin <= allocations.static_sites_begin
                ? allocations.static_sites_begin - current.static_sites_begin
                : 0;
        std::vector<std::uint8_t> strings_tail(strings_tail_size);
        std::vector<std::uint8_t> statics_tail(statics_tail_size);
        std::vector<std::uint8_t> static_sites_tail(static_sites_tail_size);
        if (!strings_tail.empty()) {
            std::memcpy(strings_tail.data(), host(allocations.strings_end), strings_tail.size());
        }
        if (!statics_tail.empty()) {
            std::memcpy(statics_tail.data(), host(allocations.statics_end), statics_tail.size());
        }
        if (!static_sites_tail.empty()) {
            std::memcpy(
                static_sites_tail.data(), host(current.static_sites_begin), static_sites_tail.size()
            );
        }
        if (allocations.strings_end > current.strings_end) {
            std::memset(
                host(current.strings_end), 0, allocations.strings_end - current.strings_end
            );
        }
        if (allocations.statics_end > current.statics_end) {
            std::memset(
                host(current.statics_end), 0, allocations.statics_end - current.statics_end
            );
        }
        if (allocations.static_sites_begin < current.static_sites_begin) {
            std::memset(
                host(allocations.static_sites_begin),
                0,
                current.static_sites_begin - allocations.static_sites_begin
            );
        }
        std::size_t at = 0;
        std::memcpy(host(GUEST_RAM), in.data() + at, ram_size_);
        at += ram_size_;
        for (const Region& r : kPlainRegions) {
            std::memcpy(host(r.address), in.data() + at, r.size);
            at += r.size;
        }
        if (!strings_tail.empty()) {
            std::memcpy(host(allocations.strings_end), strings_tail.data(), strings_tail.size());
        }
        if (!statics_tail.empty()) {
            std::memcpy(host(allocations.statics_end), statics_tail.data(), statics_tail.size());
        }
        if (!static_sites_tail.empty()) {
            std::memcpy(
                host(current.static_sites_begin), static_sites_tail.data(), static_sites_tail.size()
            );
        }
        if (allocations.static_sites_begin > current.static_sites_begin) {
            std::memset(
                host(current.static_sites_begin),
                0,
                allocations.static_sites_begin - current.static_sites_begin
            );
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool Memory::contains(const void* host, gaddr* address) const {
    auto* p = static_cast<const std::uint8_t*>(host);
#if defined(OPENRAC_PACKED_MEMORY)
    return unfold(p, ram_size_, address);
#else
    if (p < base_ || p >= base_ + kReservation) {
        return false;
    }
    *address = static_cast<gaddr>(p - base_);
    return true;
#endif
}

}  // namespace openrac::runtime
