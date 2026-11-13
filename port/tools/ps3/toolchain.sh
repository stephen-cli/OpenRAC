#!/usr/bin/env bash
# The port's PS3 toolchain libraries (README.md, "Route 1"), built inside the openrac-ps3 container
# over ps3toolchain's prebuilt compiler: newlib 4.6 (devkitPro's fork) with the console's pieces
# (newlib-lv2.py), the PPU GCC 13.2 rebuilt on it with posix threads over pthreads-embedded (the
# pthread ps3libraries installs), the system call glue libsysbase built by hand (newlib-lv2/sysbase),
# then PSL1GHT and ps3libraries again on the new C library. Installs over $PS3DEV/ppu. Resumable:
# each step is skipped when its result is there. Takes one to two hours.
set -Eeuo pipefail

: "${PS3DEV:?PS3DEV is not set}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-/opt/openrac-toolchain}"
PROCS="${PROCS:-$(nproc)}"
TOOLCHAIN_REF="${TOOLCHAIN_REF:-de85d7d5d11d}"   # ps3toolchain master of 2026-10-11, the nightly's
NEWLIB_REF="${NEWLIB_REF:-57bd8e1ade3c}"         # devkitPro/newlib, branch devkitPro, 2026-04-11: 4.6.0
PSL1GHT_REF="${PSL1GHT_REF:-dd4e8c9be744e3273d665c393fa6d97a87402e2e}"
PS3LIBRARIES_REF="${PS3LIBRARIES_REF:-f671e1158e5db2520c172bd600d55d9fce3d15d0}"
GCC="gcc-13.2.0"
PTE_HEADERS="pthread.h pthread.pte.h pte_types.h sched.h sched.pte.h semaphore.h semaphore.pte.h"

# ---- pthreads-embedded's headers with C++ linkage ----
# pthread-emb installs pthread.h, pte_types.h, sched.h and semaphore.h without extern "C": C++ code
# that includes them (libstdc++'s gthr-posix.h, the port) gets mangled names. Each is wrapped, the
# original kept beside it as *.pte.h, the wrapper written afresh each run. pthread_t is a struct (a
# pointer and a reuse count), which libstdc++ compares as a scalar in std::thread::id's ==, < and
# <=>: the wrapper of pthread.h defines those for it in C++, inside its include guard (a program
# includes <pthread.h> itself, then libstdc++'s <thread> includes it again).
wrap_for_cxx() {
    local header="$1"
    local inner="${header%.h}.pte.h"
    if [ ! -f "$inner" ]; then
        mv "$header" "$inner"
    fi
    local guard
    guard="OPENRAC_PTE_$(basename "${header%.h}" | tr '[:lower:]' '[:upper:]')_H"
    cat > "$header" <<EOF
/* OpenRAC: C++ linkage for pthreads-embedded's header (port/tools/ps3/toolchain.sh). */
#ifndef $guard
#define $guard
#ifdef __cplusplus
extern "C" {
#endif
#include "$(basename "$inner")"
#ifdef __cplusplus
}
#endif
EOF
    if [ "$(basename "$header")" = pthread.h ]; then
        cat >> "$header" <<'EOF'
#ifdef __cplusplus
inline bool operator==(const pte_handle_t& a, const pte_handle_t& b) {
    return a.p == b.p && a.x == b.x;
}
inline bool operator!=(const pte_handle_t& a, const pte_handle_t& b) {
    return !(a == b);
}
inline bool operator<(const pte_handle_t& a, const pte_handle_t& b) {
    return a.p < b.p || (a.p == b.p && a.x < b.x);
}
#if __cplusplus >= 202002L
#include <compare>
#include <cstdint>
inline std::strong_ordering operator<=>(const pte_handle_t& a, const pte_handle_t& b) {
    if (const auto c = reinterpret_cast<std::uintptr_t>(a.p) <=> reinterpret_cast<std::uintptr_t>(b.p);
        c != 0) {
        return c;
    }
    return a.x <=> b.x;
}
#endif
#endif
EOF
    fi
    echo "#endif" >> "$header"
}

wrap_pte_headers() {
    for h in pthread.h sched.h semaphore.h; do
        wrap_for_cxx "$PS3DEV/ppu/include/$h"
    done
}

mkdir -p "$WORK"
cd "$WORK"

# ---- ps3toolchain: its download script, archives and the GCC patch ----
if [ ! -d ps3toolchain ]; then
    git clone --quiet https://github.com/ps3dev/ps3toolchain.git
    git -C ps3toolchain checkout --quiet "$TOOLCHAIN_REF"
fi

# ---- newlib 4.6 with the console's pieces ----
if [ ! -d newlib ]; then
    git clone --quiet --depth 1 --branch devkitPro https://github.com/devkitPro/newlib.git newlib
    case "$(git -C newlib rev-parse HEAD)" in
        "$NEWLIB_REF"*) ;;
        *) echo "newlib: the devkitPro branch moved past $NEWLIB_REF; pin the new head" >&2; exit 1 ;;
    esac
fi
python3 "$HERE/newlib-lv2.py" "$HERE/newlib-lv2" newlib   # idempotent: a change to the pieces lands

wrap_pte_headers

# ---- GCC on the new newlib ----
mkdir -p ps3toolchain/build
cd ps3toolchain/build
source ../utils/utils.sh
if [ ! -d "$GCC" ]; then
    ../download.sh "$GCC.tar.xz"
    ../config/get-config-scripts.sh
    extract "../archives/$GCC.tar.xz"
    patch -p1 -d "$GCC" < "../patches/$GCC-PS3.patch"
    cp ../archives/config.guess ../archives/config.sub "$GCC"
    (cd "$GCC" && ./contrib/download_prerequisites)
fi
# The port's own fix to the compiler (gcc-13.2.0-savres.patch): no out-of-line register saves on
# this target, whose libgcc has none.
if ! grep -q 'OpenRAC: no out-of-line' "$GCC/gcc/config/rs6000/rs6000-logue.cc"; then
    patch -p1 -d "$GCC" < "$HERE/gcc-13.2.0-savres.patch"
fi
# The in-tree C library is the new newlib; no libgloss (PSL1GHT has the start files, and the glue
# is built by hand below).
rm -f "$GCC/newlib" "$GCC/libgloss"
ln -s "$WORK/newlib/newlib" "$GCC/newlib"
# libstdc++ and libgcc are configured against the in-tree newlib headers: pthreads-embedded's go in
# beside them, in place of newlib's own declarations of the API it does not implement.
for h in $PTE_HEADERS; do
    cp "$PS3DEV/ppu/include/$h" "$WORK/newlib/newlib/libc/include/$h"
done
mkdir -p "$GCC/build-ppu-newlib46"
cd "$GCC/build-ppu-newlib46"
if [ ! -f Makefile ]; then
    # ps3toolchain's 002-gcc-newlib-PPU configuration, with posix threads, newlib's retargetable
    # locks (libsysbase's, over lv2's mutexes), and newlib's own libm in place of mathfp.
    CFLAGS="-Wno-int-conversion" CXXFLAGS="-Wno-int-conversion" ../configure \
        --prefix="$PS3DEV/ppu" --target="powerpc64-ps3-elf" \
        --disable-dependency-tracking \
        --disable-libcc1 \
        --disable-libstdcxx-pch \
        --disable-multilib \
        --disable-nls \
        --disable-shared \
        --disable-win32-registry \
        --enable-languages="c,c++" \
        --enable-long-double-128 \
        --enable-lto \
        --enable-threads=posix \
        --with-cpu="cell" \
        --with-newlib \
        --enable-newlib-multithread \
        --enable-newlib-retargetable-locking \
        --with-system-zlib
fi
make -j "$PROCS" all
make MULTIOSDIR=. install

# ---- the system call glue ----
make -C "$HERE/newlib-lv2/sysbase" BUILD="$WORK/sysbase" clean install

# ---- PSL1GHT and ps3libraries on the new C library ----
# Of ps3libraries, the two the port's toolchain needs: zlib (1) and pthreads-embedded (32), the
# pthread libstdc++ was built over. The other forty are old ports (libunrar, polarssl) that do not
# compile against newlib 4.6, and the port has no use for them.
(cd /opt/ps3dev && PSL1GHT_REF="$PSL1GHT_REF" PS3LIBRARIES_REF="$PS3LIBRARIES_REF"     PS3LIBRARIES_STAGES="1 32" ./build-all.sh 2 3)
# pthreads-embedded again with the port's changes (pthread-emb-lv2.py: usable from a program's first
# instruction), from the source ps3libraries left unpacked.
PTE=/opt/ps3dev/build/ps3libraries/build/pthread-emb-ps3
python3 "$HERE/pthread-emb-lv2.py" "$PTE"
make -C "$PTE/platform/psl1ght" install
# ps3libraries installed pthread-emb's headers again, unwrapped: wrapped once more, and the target
# include directory (where newlib's install put the in-tree copies) kept the same.
wrap_pte_headers
for h in $PTE_HEADERS; do
    cp "$PS3DEV/ppu/include/$h" "$PS3DEV/ppu/powerpc64-ps3-elf/include/$h"
done

ppu-g++ -v 2>&1 | grep 'Thread model'
grep -o '_NEWLIB_VERSION "[0-9.]*"' "$PS3DEV/ppu/powerpc64-ps3-elf/include/_newlib_version.h"
