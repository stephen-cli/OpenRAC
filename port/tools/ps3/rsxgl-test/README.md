# rsxgl-test

The RSX layer ([renderer/rsx](../../renderer/rsx/README.md)) checked by
its pictures: a console program that calls the layer's GL as the renderer
does, draws rectangles of known colours with the direct program, reads
the pixels back and prints one line a check, `ok` or `FAIL` with what it
read and what GL would have drawn. It ends with status 0 when every check
passed. It is 16 MB, so RPCS3 starts it in seconds where the game takes
eight minutes of recompiling: the layer's faults are found here first.

Target `openrac-rsxgl-test` of the `ps3-window` preset
([cmake/Window.cmake](../../cmake/Window.cmake)):

```sh
cmake --build build/ps3-window --target openrac-rsxgl-test
rpcs3 --no-gui --config CONFIG build/ps3-window/openrac-rsxgl-test
```

`CONFIG` has a real renderer and `Write Color Buffers: true`, as the
[probe](../rsx-probe/README.md)'s. What the program printed is in RPCS3's
`TTY.log`.

What it checks (55 checks, all passing on RPCS3 0.0.42, 2026-11-14):

| Check | What it would catch |
|---|---|
| clear, scissored clear, scissored draw | the scissor's rectangle turned over on the screen |
| constant weight, no fog on a fogged draw | the fragment program's numbers reaching it (texture coordinates 2, 3, 5) and the fog attribute |
| vertex colour in one quarter | which way is up; the order of the colour's bytes |
| texture, texture modulated | texture coordinates, the texel rows' order, the bytes of an upload, the chip's 0x80 as one |
| alpha test and its failures | the six comparisons' arithmetic, the rounding, the second pass of the double draw |
| vertex alpha above and below the reference | the colour's alpha byte |
| blend, added and by the source's alpha | the blend state; the alpha the fragment program writes |
| framebuffer, blit to the screen, framebuffer as a texture, copied texture | a texture as the RSX's surface and back, the copy between framebuffers, `glCopyTexSubImage2D` |
| frame 300 | 300 frames of 100 draws: the command buffer's segments filling and wrapping, buffers taking new memory within a frame |

What it found on its first day, each of which the game had shown only as
a black picture or a stall:

- librsx's `rsxSetVertexProgramConstants` counts single numbers and
  converts each to an integer: the layer uses
  `rsxLoadVertexProgramParameterBlock`.
- A fragment program instruction reads one interpolated input; `cgcomp
  -a` takes several and keeps the last. It also leaves out an
  instruction the chip has none for (`SUB`) and says nothing.
- librsx called the command buffer's callback in a way GCC 13 compiled
  into commands written over the jump at a segment's end
  ([../psl1ght-lv2.py](../psl1ght-lv2.py)): the RSX lost its place after
  32 KB of commands.
- RPCS3 writes a surface back to memory when it stops being the target,
  so a read-back takes the RSX off it first.
- Onto an X8R8G8B8 surface RPCS3 blended with a source alpha of one: the
  screen's surface is A8R8G8B8.
