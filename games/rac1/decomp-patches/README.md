# OpenRAC's changes to rac1-decomp

Applied to [rac1-decomp](https://github.com/OpenRAC/rac1-decomp) (`games/rac1/decomp`) by `python3 tools/sources.py apply`; how to change them: [docs/SOURCES.md](../../../docs/SOURCES.md).

OpenRAC's own files in `files/`:

- `tools/boot_elf.py`: a bootable executable from the linked build, with extra loadable segments, for an
  edition that adds code to the game (the Executive Edition).
- `tools/iso_patch.py`: writes changed files into a copy of the disc image in place, or appended.

<!-- patches:begin -->
<!-- Written by tools/sources.py save; do not edit. -->
| Patch | What |
|---|---|
| [0001-space-transition-globals.patch](0001-space-transition-globals.patch) | Name current_vu_chain and space_load_in_progress directly under OPENRAC_HOSTGEN. |
| [0002-mark-interpolated-texture-uploads.patch](0002-mark-interpolated-texture-uploads.patch) | Mark interpolated texture uploads as generated. |
| [0003-keep-an-owned-item-0x24-in-the-hand-outside-the-race.patch](0003-keep-an-owned-item-0x24-in-the-hand-outside-the-race.patch) | apply_hero_item_slot_selection clears a held item 0x24 whenever the game mode is not the race mode (0x16), because the item exists only there and is never owned. |
| [0004-mount-a-linked-toolchain.patch](0004-mount-a-linked-toolchain.patch) | On Windows gcc323.py runs the C frontend in the build container with the decomp mounted at /work. |

Files under [files/](files) (copied as they are):

- [`tools/boot_elf.py`](files/tools/boot_elf.py)
- [`tools/iso_patch.py`](files/tools/iso_patch.py)
<!-- patches:end -->
