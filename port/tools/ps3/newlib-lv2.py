"""The PS3 in a newlib 4.x tree (README.md): the host configuration of powerpc64-ps3-elf, the
console's reentrancy and malloc alignment in sys/config.h, newlib's own pthread types and POSIX
thread feature macros left out (the console's pthread is pthreads-embedded's), and its headers
(dirent, utime, the __syscalls table, 64-bit file offsets) copied in. Nothing here touches
newlib's configure.ac or Makefile.am, so no autotools run is needed. Idempotent.

    python3 newlib-lv2.py <newlib-lv2 directory> <newlib source tree>
"""

import re
import shutil
import sys
from pathlib import Path

pieces = Path(sys.argv[1])
tree = Path(sys.argv[2])


def insert_before(path, anchor, text):
    s = path.read_text(encoding="utf-8")
    if text in s:
        return
    if s.count(anchor) != 1:
        raise SystemExit(f"{path}: {s.count(anchor)} matches for the anchor {anchor!r}")
    path.write_text(s.replace(anchor, text + anchor), encoding="utf-8", newline="\n")


def replace_once(path, old, new):
    s = path.read_text(encoding="utf-8")
    if new in s:
        return
    if s.count(old) != 1:
        raise SystemExit(f"{path}: {s.count(old)} matches for {old!r}")
    path.write_text(s.replace(old, new), encoding="utf-8", newline="\n")


host = tree / "newlib" / "configure.host"
# The system: no newlib crt0 (PSL1GHT's lv2-crt0.o), no sys directory (the glue is libsysbase,
# built apart, ../newlib-lv2/sysbase).
insert_before(host, "  a29k-*-*)\n\tsys_dir=a29khif\n", "  *-ps3-*)\n\thave_crt0=\"no\"\n\t;;\n")
# The host's flags: 64-bit printf, the reentrancy structure from __getreent (libsysbase's, in
# thread-local storage), the reentrant system calls provided, rename and opendir provided, and
# libc/syscalls (the plain open, write and the rest over the reentrant ones). An earlier version
# of the block, from a tree prepared before, is taken out first: the first case wins in a shell.
host_text = host.read_text(encoding="utf-8")
host_text = re.sub(r"  powerpc64-ps3-elf\)\n(?:\t[^\n]*\n)*?\t;;\n", "", host_text)
host.write_text(host_text, encoding="utf-8", newline="\n")
insert_before(
    host,
    "  a29k-amd-udi)\n",
    "  powerpc64-ps3-elf)\n"
    "\tdefault_newlib_io_long_long=\"yes\"\n"
    "\tdefault_newlib_io_c99_formats=\"yes\"\n"
    "\tnewlib_cflags=\"${newlib_cflags} -D__DYNAMIC_REENT__ -DGETREENT_PROVIDED"
    " -DREENTRANT_SYSCALLS_PROVIDED\"\n"
    "\tnewlib_cflags=\"${newlib_cflags} -DHAVE_RENAME -DHAVE_OPENDIR"
    " -ffunction-sections -fdata-sections\"\n"
    "\tsyscall_dir=syscalls\n"
    "\t;;\n",
)

include = tree / "newlib" / "libc" / "include"
insert_before(
    include / "sys" / "config.h",
    "#ifdef __AMDGCN__\n",
    "#if defined(__lv2ppu__)\n"
    "/* The PS3: the reentrancy structure from a function (one per thread), and malloc aligned\n"
    "   for the Cell's vector types. */\n"
    "#define __DYNAMIC_REENT__\n"
    "#define MALLOC_ALIGNMENT 16\n"
    "#endif\n\n",
)

# devkitPro's fork declares the POSIX thread features for every target, and sys/types.h brings
# newlib's own pthread types in: not on the PS3, whose pthread is pthreads-embedded's, with types
# of its own (a struct for pthread_t, pointers for the rest).
features = include / "sys" / "features.h"
replace_once(
    features,
    "//#ifdef __DEVKITA64__\n#define _POSIX_MONOTONIC_CLOCK\t\t200112L\n",
    "#ifndef __lv2ppu__  /* the PS3's are pthreads-embedded's */\n"
    "#define _POSIX_MONOTONIC_CLOCK\t\t200112L\n",
)
replace_once(
    features,
    "#define _UNIX98_THREAD_MUTEX_ATTRIBUTES\t1\n//#endif\n",
    "#define _UNIX98_THREAD_MUTEX_ATTRIBUTES\t1\n#endif\n",
)
replace_once(
    include / "sys" / "_pthreadtypes.h",
    "#if defined(_POSIX_THREADS) || __POSIX_VISIBLE >= 199506\n",
    "#if (defined(_POSIX_THREADS) || __POSIX_VISIBLE >= 199506) && !defined(__lv2ppu__)\n",
)
# PSL1GHT shadows sys/select.h with its own (net/select.h), which includes time.h and sys/types.h
# itself; included from inside sys/types.h, as newlib does for BSD visibility, it reads time.h
# before the types it needs exist. A program that selects includes PSL1GHT's header itself.
replace_once(
    include / "sys" / "types.h",
    "#if __BSD_VISIBLE\n#include <machine/endian.h>\n#include <sys/select.h>\n",
    "#if __BSD_VISIBLE\n#include <machine/endian.h>\n#ifndef __lv2ppu__  /* PSL1GHT's own */\n"
    "#include <sys/select.h>\n#endif\n",
)
# What PSL1GHT's select header takes from sys/types.h, as newlib 1.20 defined them there (a mask
# of long, the PS3's set of 1024): the layout its programs and its network library were built with.
replace_once(
    include / "sys" / "types.h",
    "#if __BSD_VISIBLE\n#include <machine/endian.h>\n#ifndef __lv2ppu__  /* PSL1GHT's own */\n",
    "#ifdef __lv2ppu__\n"
    "/* For PSL1GHT's net/select.h, as newlib 1.20's sys/types.h had them. */\n"
    "#ifndef FD_SETSIZE\n#define FD_SETSIZE 1024\n#endif\n"
    "#ifndef NBBY\n#define NBBY 8\n#endif\n"
    "typedef long fd_mask;\n"
    "#define NFDBITS (sizeof (fd_mask) * NBBY)\n"
    "#ifndef howmany\n#define howmany(x, y) (((x) + ((y) - 1)) / (y))\n#endif\n"
    "#endif\n\n"
    "#if __BSD_VISIBLE\n#include <machine/endian.h>\n#ifndef __lv2ppu__  /* PSL1GHT's own */\n",
)
# And fd_set itself, which PSL1GHT's sys/socket.h names with no select header included: newlib
# 1.20's type and macros, which its sys/types.h defined.
replace_once(
    include / "sys" / "types.h",
    "#ifndef howmany\n#define howmany(x, y) (((x) + ((y) - 1)) / (y))\n#endif\n#endif\n",
    "#ifndef howmany\n#define howmany(x, y) (((x) + ((y) - 1)) / (y))\n#endif\n"
    "typedef struct _types_fd_set {\n"
    "\tfd_mask fds_bits[howmany(FD_SETSIZE, NFDBITS)];\n"
    "} _types_fd_set;\n"
    "#define fd_set _types_fd_set\n"
    "#define FD_SET(n, p) ((p)->fds_bits[(n) / NFDBITS] |= (1L << ((n) % NFDBITS)))\n"
    "#define FD_CLR(n, p) ((p)->fds_bits[(n) / NFDBITS] &= ~(1L << ((n) % NFDBITS)))\n"
    "#define FD_ISSET(n, p) ((p)->fds_bits[(n) / NFDBITS] & (1L << ((n) % NFDBITS)))\n"
    "#define FD_ZERO(p) \\\n"
    "\t(__extension__ (void)({ \\\n"
    "\t\tsize_t __i; \\\n"
    "\t\tchar *__tmp = (char *)(p); \\\n"
    "\t\tfor (__i = 0; __i < sizeof(*(p)); ++__i) \\\n"
    "\t\t\t*__tmp++ = 0; \\\n"
    "\t}))\n"
    "#endif\n",
)
replace_once(
    include / "sys" / "time.h",
    "#if __BSD_VISIBLE || __POSIX_VISIBLE >= 200112 || __XSI_VISIBLE\n#include <sys/select.h>\n#endif\n",
    "#if (__BSD_VISIBLE || __POSIX_VISIBLE >= 200112 || __XSI_VISIBLE) && !defined(__lv2ppu__)\n"
    "#include <sys/select.h>  /* PSL1GHT's own on the PS3 */\n#endif\n",
)
# The declaration macros newlib's _ansi.h had until 2.x (_DEFUN, _AND, _EXFUN and the rest), which
# PSL1GHT's librt and the glue (newlib-lv2/sysbase) still write; newlib 4.6 dropped them.
replace_once(
    include / "_ansi.h",
    "#endif /* _ANSIDECL_H_ */\n",
    "#ifdef __lv2ppu__\n"
    "/* The declaration macros of newlib 1.x, which PSL1GHT and the PS3's libsysbase still write. */\n"
    "#define _PTR void *\n"
    "#define _AND ,\n"
    "#define _NOARGS void\n"
    "#define _CONST const\n"
    "#define _VOLATILE volatile\n"
    "#define _SIGNED signed\n"
    "#define _DOTS , ...\n"
    "#define _VOID void\n"
    "#define _EXFUN(name, proto) name proto\n"
    "#define _EXFUN_NOTHROW(name, proto) name proto _NOTHROW\n"
    "#define _DEFUN(name, arglist, args) name(args)\n"
    "#define _DEFUN_VOID(name) name(_NOARGS)\n"
    "#define _CAST_VOID (void)\n"
    "#define _PARAMS(paramlist) paramlist\n"
    "#endif\n"
    "#endif /* _ANSIDECL_H_ */\n",
)
# struct stat as newlib 1.20 laid it out for the PS3 (time_t fields with spares, no st_atim
# timespecs): PSL1GHT's librt fills it by those names, and its own file system structure
# (lv2/sysfs.h) names fields st_atime, which newlib 4.6's macros would rewrite.
replace_once(
    include / "sys" / "stat.h",
    "#elif defined(__mips__) && !defined(__rtems__)\n  time_t\tst_atime;\n",
    "#elif (defined(__mips__) && !defined(__rtems__)) || defined(__lv2ppu__)\n  time_t\tst_atime;\n",
)
replace_once(
    include / "sys" / "stat.h",
    "#if !((defined(__svr4__) && !defined(__PPC__) && !defined(__sun__)) || \\\n"
    "    (defined(__mips__) && !defined(__rtems__)))\n",
    "#if !((defined(__svr4__) && !defined(__PPC__) && !defined(__sun__)) || \\\n"
    "    (defined(__mips__) && !defined(__rtems__)) || defined(__lv2ppu__))\n",
)
# PSL1GHT's headers (lv2/sysfs.h) take MAXPATHLEN from unistd.h, where newlib 1.20 gave it; newlib
# 4.6 keeps it in sys/param.h, behind the BSD visibility the port's strict C++ does not have.
replace_once(
    include / "sys" / "unistd.h",
    "#include <sys/_types.h>\n#include <stddef.h>\n",
    "#include <sys/_types.h>\n#include <stddef.h>\n"
    "#if defined(__lv2ppu__) && !defined(MAXPATHLEN)\n#define MAXPATHLEN 1024  /* as newlib 1.20 had it */\n"
    "#endif\n",
)
replace_once(
    include / "sys" / "signal.h",
    "#if __POSIX_VISIBLE >= 199506\nint pthread_kill (pthread_t, int);\n",
    "#if __POSIX_VISIBLE >= 199506 && !defined(__lv2ppu__)  /* pthreads-embedded declares it */\n"
    "int pthread_kill (pthread_t, int);\n",
)

for name in ["sys/dirent.h", "sys/utime.h", "sys/syscalls.h"]:
    shutil.copyfile(pieces / "include" / name, include / name)

# devkitPro's PowerPC machine directory carries the Wii's own sys/lock.h (32-bit lock handles and
# condition variables, implemented by its platform libraries), sys/stdio.h (stream locks through
# __flockfile) and sys/dirent.h (its device tables), which newlib installs over the generic ones.
# The PS3 keeps newlib's generic headers, whose retargetable locks libsysbase implements over lv2's
# mutexes (newlib-lv2/sysbase/retarget_lock.c), and its own dirent. The directory's machine/_types.h
# (64-bit file offsets) and the PowerPC fenv stay.
machine = tree / "newlib" / "libc" / "machine" / "powerpc"
for name in ["sys/lock.h", "sys/stdio.h", "sys/dirent.h"]:
    if (machine / name).exists():
        (machine / name).unlink()
print("newlib tree prepared for the PS3")
