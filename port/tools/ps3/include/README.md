# Headers the console's C library has under other names

`ppu.cmake` puts this directory on the system include path of every unit
built for the PS3. A file here is a header that newlib, the console's C
library, provides under another name or not at all, so that a source which
expects glibc's compiles unchanged. Each says what it stands in for.

| File | Stands in for |
|---|---|
| `endian.h` | glibc's `<endian.h>`: newlib's is `<sys/endian.h>` (FreeBSD's) |
| `dlfcn.h` | POSIX's `<dlfcn.h>`: the console loads no shared object, so every load fails (SDL's Unix code looks for GTK with it) |
| `poll.h` | POSIX's `<poll.h>`: a wait that fails, for SDL's Unix code, which the console's build never calls |
