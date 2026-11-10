# The PS3's PPU with PSL1GHT (README.md): ps3toolchain's GCC for powerpc64-ps3-elf, newlib, and
# PSL1GHT's headers and libraries, as its ppu_rules compile a homebrew program. The toolchain is found
# through PS3DEV (/usr/local/ps3dev in the container). A bare-metal target: the compiler checks build a
# static library, as no program links without PSL1GHT's libraries.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)

if(NOT DEFINED ENV{PS3DEV})
  message(FATAL_ERROR "PS3DEV is not set (the ps3dev installation, /usr/local/ps3dev)")
endif()
set(PS3DEV "$ENV{PS3DEV}")
set(PSL1GHT "$ENV{PSL1GHT}")
if(NOT PSL1GHT)
  set(PSL1GHT "${PS3DEV}")
endif()

set(CMAKE_C_COMPILER "${PS3DEV}/ppu/bin/ppu-gcc")
set(CMAKE_CXX_COMPILER "${PS3DEV}/ppu/bin/ppu-g++")
set(CMAKE_ASM_COMPILER "${PS3DEV}/ppu/bin/ppu-gcc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ppu_rules' MACHDEP and CXXFLAGS_INIT, with the PSL1GHT headers.
set(ppu_flags "-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections")
set(ppu_flags "${ppu_flags} -I${PSL1GHT}/ppu/include -I${PSL1GHT}/ppu/include/simdmath")
set(CMAKE_C_FLAGS_INIT "${ppu_flags}")
# ppu_rules' CXXFLAGS_INIT. What this toolchain's C and C++ libraries lack for the port is in
# tools/ps3/README.md: the configuration is for measuring that, not yet for building the port.
set(CMAKE_CXX_FLAGS_INIT "${ppu_flags} -D_GLIBCXX11_USE_C99_STDIO")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${PSL1GHT}/ppu/lib -L${PS3DEV}/portlibs/ppu/lib -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/ppu" "${PSL1GHT}/ppu" "${PS3DEV}/portlibs/ppu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# PSL1GHT's threads are its own library, not found as a system's.
set(CMAKE_THREAD_LIBS_INIT "-lpthread")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_PTHREADS_INIT 1)
set(Threads_FOUND TRUE)

# What a PS3 program links beyond libc: PSL1GHT's lv2 syscalls, file system, modules and utilities.
set(OPENRAC_PS3_LIBRARIES -lsysfs -lsysutil -lsysmodule -lpthread -lrt -llv2 -lm)
set(OPENRAC_PS3 ON)
