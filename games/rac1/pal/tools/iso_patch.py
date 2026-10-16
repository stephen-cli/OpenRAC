#!/usr/bin/env python3
"""
Write a bootable disc image: your own image with the rebuilt executable
(and optionally the editor's packed level sectors) written in place.

The PAL disc names only three files in its ISO 9660 directory; everything
else is addressed by absolute sector through the table the game reads at
startup (editor/disc.py). So a repack that keeps every size is a copy of the
image with sectors overwritten:

  - the boot executable, `SCES_509.16;1` in the root directory, replaced by
    tools/boot_elf.py's output (same size, by construction);
  - optionally the sectors listed in a `patches.tsv` written by
    `editor/extract.py pack` (first sector, sector count, file), the
    editor's edited levels, which it sizes to fit where the originals are.

Nothing in the directory or the sector table changes, so the result boots
wherever the original does. A larger executable or level needs a real
re-layout (wrench) and is refused here.

Usage:
  python tools/iso_patch.py [--elf build-sn/SCES_509.16]
                            [--patches ../../../assets/packed/rac1/patches.tsv]
                            [-o build-sn/SCES_509.16.iso]
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

SECTOR = 2048
IMAGE = Path("baserom/SCES_509.16.iso")
EXE_NAME = "SCES_509.16;1"


def root_directory(f) -> tuple[int, int]:
    """Extent and size of the root directory, from the primary volume descriptor."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise SystemExit("not an ISO 9660 image (no primary volume descriptor)")
    record = pvd[156:156 + 34]
    extent, = struct.unpack_from("<I", record, 2)
    size, = struct.unpack_from("<I", record, 10)
    return extent, size


def find_file(f, name: str) -> tuple[int, int]:
    """Extent (LBA) and data length of NAME in the root directory."""
    extent, size = root_directory(f)
    f.seek(extent * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:                      # padding to the next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        name_len = rec[32]
        ident = rec[33:33 + name_len].decode("ascii", "replace")
        if ident == name:
            return struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        pos += length
    raise SystemExit(f"{name} not found in the root directory")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--elf", type=Path, default=Path("build-sn/SCES_509.16"),
                    help="bootable executable from tools/boot_elf.py")
    ap.add_argument("--patches", type=Path, help="patches.tsv from `editor/extract.py pack`")
    ap.add_argument("-o", "--output", type=Path, default=Path("build-sn/SCES_509.16.iso"))
    args = ap.parse_args()

    if not IMAGE.is_file():
        print(f"missing {IMAGE} (python3 tools/openrac.py setup rac1/pal)", file=sys.stderr)
        return 1
    if not args.elf.is_file():
        print(f"missing {args.elf} (python tools/boot_elf.py)", file=sys.stderr)
        return 1
    if args.output.resolve() == IMAGE.resolve():
        print("refusing to overwrite the original image", file=sys.stderr)
        return 1

    writes: list[tuple[int, bytes, str]] = []
    with IMAGE.open("rb") as f:
        lba, size = find_file(f, EXE_NAME)
    elf = args.elf.read_bytes()
    if len(elf) != size:
        print(f"{args.elf} is {len(elf)} bytes, the disc's executable is {size}: a larger "
              f"executable needs a re-layout, not an in-place patch", file=sys.stderr)
        return 1
    writes.append((lba * SECTOR, elf, f"{EXE_NAME} (sector {lba}, {size} bytes)"))

    if args.patches:
        base = args.patches.parent
        for line in args.patches.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            first, count, name = line.split("\t")[:3]
            first, count = int(first), int(count)
            blob = (base / name).read_bytes()
            if len(blob) > count * SECTOR:
                print(f"{name}: {len(blob)} bytes do not fit in {count} sectors", file=sys.stderr)
                return 1
            blob = blob.ljust(count * SECTOR, b"\0")
            writes.append((first * SECTOR, blob, f"{name} (sectors {first}-{first + count - 1})"))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(f"copying {IMAGE} -> {args.output} ({IMAGE.stat().st_size} bytes)")
    shutil.copyfile(IMAGE, args.output)
    changed_total = 0
    with args.output.open("r+b") as out:
        for offset, blob, what in writes:
            out.seek(offset)
            old = out.read(len(blob))
            changed = sum(1 for a, b in zip(old, blob) if a != b)
            changed_total += changed
            out.seek(offset)
            out.write(blob)
            print(f"  wrote {what}: {changed} byte(s) changed")
    print(f"done: {changed_total} byte(s) differ from the original image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
