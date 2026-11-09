# 64-bit big-endian PowerPC Linux (ELFv1), as Debian's powerpc64-linux-gnu, with GCC: the game's byte order on a
# big-endian host rests on GCC's scalar_storage_order (runtime/include/openrac/guest.h), which Clang lacks. The
# code is for the Cell's PPU (-mcpu=cell), as the PS3's would be; GCC 14's generic PowerPC target has no pattern
# for a byte-swapped volatile 64-bit load and stops with an internal error on the game's hardware-register reads.
# The binaries run under qemu-ppc64 (user mode).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc64)
set(triple powerpc64-linux-gnu)
set(CMAKE_C_COMPILER powerpc64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc64-linux-gnu-g++)
set(CMAKE_SYSROOT /)
set(CMAKE_FIND_ROOT_PATH /usr/${triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT "-mcpu=cell")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cell")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc64 -L /usr/${triple})
