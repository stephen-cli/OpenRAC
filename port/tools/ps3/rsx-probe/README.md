# rsx-probe

The first picture from the console: a program that sets the RSX up through
PSL1GHT's `librsx`, clears the screen, draws one triangle shaded by two
programs, reads the colour buffer back out of the RSX's memory and writes
it beside itself as `frame.ppm`. It is the ground the renderer's console
backend stands on (docs/port/PS3.md, section 7), and the way a script
gets a picture out of a console program.

| File | What |
|---|---|
| `main.cpp` | the program: video mode, two colour buffers and a depth buffer in RSX memory, the surface, the programs, the draw, the flip, the read-back |
| `triangle.vp`, `triangle.fp` | the vertex and fragment programs, in ARB assembly |
| `build.sh` | assembles the programs (`cgcomp -a`), embeds them, builds the ELF |
| `ppm2png.py` | `frame.ppm` as a PNG, with the standard library alone |

```sh
podman run --rm -v "$PWD:/src" openrac-ps3 sh /src/port/tools/ps3/rsx-probe/build.sh
rpcs3 --no-gui --config CONFIG port/build/rsx-probe/rsx-probe.elf
python port/tools/ps3/rsx-probe/ppm2png.py port/build/rsx-probe/frame.ppm frame.png
```

What it established (2026-11-13, RPCS3 0.0.42):

- **Programs without the Cg toolkit.** PSL1GHT's `cgcomp` compiles Cg
  through NVIDIA's `libCg`, which is proprietary and not in the image;
  with `-a` it assembles ARB vertex and fragment program text by itself.
  The triangle's two programs are 32 and 16 bytes of RSX code.
- **A picture a script can take.** The colour buffer is the program's own
  memory (`rsxMemalign`), so after the RSX has finished (a label written
  at the end of the command buffer) the program reads it as X8R8G8B8
  words. On the console that is the picture itself; under RPCS3 it needs
  a real renderer and `Write Color Buffers: true` in the config, which
  copies what the host's GPU drew back into the console's memory. The
  port's headless config (Null renderer) gives an empty buffer.
- 1280x720 was the video mode RPCS3 reported; the corner pixel read back
  as the clear colour and the centre as the three vertex colours mixed.
- The program does not pump the system's events and returns after one
  picture; RPCS3 with `Exit RPCS3 when process finishes: false` stays
  open after it and is stopped by the script that started it.
