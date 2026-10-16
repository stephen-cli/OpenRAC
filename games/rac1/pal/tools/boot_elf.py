#!/usr/bin/env python3
"""
Write a bootable executable from the build: retail's container with the
rebuilt bytes laid over it.

build-sn/rac1.elf is the linked image the audits read (tools/check_image.py,
tools/check_layout.py). It is not what the console boots: its entry point is
the first .text symbol rather than crt0's _start, it is laid out as two
PT_LOAD segments, and the VU0 microcode at the front of retail's image
(0x100080-0x112380) is not linked (rac1.ld.sh). So the boot file is made the
other way round from check_image.py: start from baserom/SCES_509.16, and
copy every allocated PROGBITS section of rac1.elf that lies inside retail's
single loaded segment over the same addresses. Sections parked outside it
(.reginfo/.mdebug at 0x2400000) are left out.

With an unmodified tree the result differs from retail only inside the
decompiled near-misses, which is printed, so the output doubles as a check.

Usage: python tools/boot_elf.py [-o build-sn/SCES_509.16]
"""
import argparse
import hashlib
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

RETAIL = Path("baserom/SCES_509.16")
LINKED = Path("build-sn/rac1.elf")
SHF_ALLOC = 0x2


def add_segment(image: bytearray, addr: int, data: bytes) -> bytearray:
    """IMAGE with DATA appended as one more PT_LOAD at ADDR. Retail's program
    header table (one entry at 0x34) is followed by zeros up to the first
    segment at 0x1000, so the new entry goes right after it. The data goes
    after the file's end (the section headers), at an offset congruent to
    ADDR modulo the page size, as the ELF rules want."""
    import struct
    e_phoff, = struct.unpack_from("<I", image, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", image, 42)
    new_hdr = e_phoff + e_phnum * e_phentsize
    if any(image[new_hdr:new_hdr + e_phentsize]):
        raise SystemExit("no room for another program header")
    page = 0x1000
    offset = len(image)
    offset += (addr - offset) % page
    image = image + bytes(offset - len(image)) + data
    image += bytes((-len(image)) % 16)
    # PT_LOAD, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, PF_R|PF_W|PF_X, p_align
    struct.pack_into("<8I", image, new_hdr, 1, offset, addr, addr, len(data), len(data), 7, page)
    struct.pack_into("<H", image, 44, e_phnum + 1)
    return image


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", type=Path, default=Path("build-sn/SCES_509.16"))
    ap.add_argument("--segment", metavar="ADDR:FILE", action="append", default=[],
                    help="add a loadable segment: FILE's bytes at hex address ADDR (a second PT_LOAD "
                         "the console's loader places in RAM at boot; for code of your own outside "
                         "retail's image, e.g. a mod's resident code)")
    args = ap.parse_args()

    if not RETAIL.is_file():
        print(f"missing {RETAIL} (python3 tools/openrac.py setup rac1/pal)", file=sys.stderr)
        return 1
    if not LINKED.is_file():
        print(f"missing {LINKED} (bash tools/build_sn.sh)", file=sys.stderr)
        return 1

    retail = bytearray(RETAIL.read_bytes())
    with RETAIL.open("rb") as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        if len(loads) != 1:
            print(f"{RETAIL}: expected one PT_LOAD, found {len(loads)}", file=sys.stderr)
            return 1
        seg = loads[0]
        seg_vaddr, seg_off, seg_size = seg["p_vaddr"], seg["p_offset"], seg["p_filesz"]

    written = 0
    changed = 0
    skipped = []
    with LINKED.open("rb") as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if not sec["sh_flags"] & SHF_ALLOC or sec["sh_type"] != "SHT_PROGBITS" or sec["sh_size"] == 0:
                continue
            addr, size = sec["sh_addr"], sec["sh_size"]
            if addr < seg_vaddr or addr + size > seg_vaddr + seg_size:
                skipped.append(f"{sec.name} 0x{addr:X}+0x{size:X}")
                continue
            data = sec.data()
            off = seg_off + (addr - seg_vaddr)
            old = retail[off:off + size]
            changed += sum(1 for a, b in zip(old, data) if a != b)
            retail[off:off + size] = data
            written += size

    for spec in args.segment:
        addr_text, _, file_text = spec.partition(":")
        addr = int(addr_text, 16)
        data = Path(file_text).read_bytes()
        retail = add_segment(retail, addr, data)
        print(f"added segment: {len(data)} bytes at 0x{addr:08X} from {file_text}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(retail)
    print(f"wrote {args.output}: {len(retail)} bytes, {written} bytes from {LINKED}, "
          f"{changed} byte(s) changed against retail")
    if skipped:
        print("left out (outside retail's loaded segment): " + ", ".join(skipped))
    print(f"sha1 {hashlib.sha1(retail).hexdigest()}"
          + (" (identical to retail)" if changed == 0 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
