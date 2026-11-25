# rsxgl-test

The RSX layer ([renderer/rsx](../../renderer/rsx/README.md)) checked by
its pictures: a console program that calls the layer's GL as the renderer
does, draws rectangles of known colours with each of the layer's programs, reads
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
`TTY.log`, and in `/dev_hdd0/tmp/openrac-rsxgl-test.txt`, written line by
line: a console has no terminal, and a program that stops there has left
what it said before.

On a console with custom firmware and webMAN MOD (`PS3` its address),
without a hand on it:

```sh
ppu-strip build/ps3-window/openrac-rsxgl-test -o test.elf && sprxlinker test.elf
make_self_npdrm test.elf pkg/USRDIR/EBOOT.BIN UP0001-ORAT00001_00-0000000000000000
sfo --title "OpenRAC RSX layer test" --appid ORAT00001 -f $PS3DEV/bin/sfo.xml pkg/PARAM.SFO
pkg --contentid UP0001-ORAT00001_00-0000000000000000 pkg/ test.pkg && package_finalize test.pkg
curl -T test.pkg ftp://PS3/dev_hdd0/packages/test.pkg
curl http://PS3/install_ps3/dev_hdd0/packages/test.pkg   # then /pad.ps3?circle leaves its screen
curl "http://PS3/play.ps3?ORAT00001"
curl ftp://PS3/dev_hdd0/tmp/openrac-rsxgl-test.txt
```

The program takes about a minute there (its 300 frames of 100
full-screen draws) and returns to the XMB. An EBOOT replaced over FTP
does not start: the package is installed again. webMAN's pages carry the
console's identifiers; they are not for a log.

What it checks (124 checks, all passing on RPCS3 0.0.42, 2026-11-26; the
121 before the mip chain's pass on a console, a CECHA at 1920x1080):

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
| mip chain | `glGenerateMipmap`'s levels where the RSX reads them (a chain packed without the first level's pitch draws black) and the filter between levels reaching the RSX (without a chain the stripes are not averaged) |
| sky | an indexed draw; the two matrices multiplied in the right order; a vertex colour above one; the tint |
| mesh: two instances | a matrix and a tint for each instance from the instance buffer |
| mesh: the viewer's sun, fog | the normal through the model's rotation; the fog's arithmetic in the vertex program and its weight in the fragment program |
| mesh: cutout, in the solid and the translucent pass | the two tests on the texel's alpha; a material without a cutout drawing nothing in the translucent pass |
| mesh: 20000 draws a frame | more commands in a frame than the command buffer holds |
| moby: two joints' skin; no palette | the joints' constants read through the address register, two joints weighted, from the instance's place in the palette texture; an instance without a palette as stored |
| moby: MobyProc's lights; two sets cross-faded | the ambient, a light faced, a light leaking through from behind; the fade between two sets |
| moby: glow vertices | drawn as any vertex without a glow colour, left out of the solid pass with one |
| moby: glow pass | the glow faces in the glow colour and no others; the two draws split at the alpha reference; the alpha written, blended |
| moby: effect pass | a blended moby whole and lit, at the texel's alpha times its own; the split; an added one's colour times an alpha past 1 |
| moby: metal pass | the sphere map read by the normal as the camera sees it, the chip's 0x70 without the material's colour, the shine of the moby's distance as alpha |
| tie: a light slot's colour; a vertex without a slot; a colour written again; an instance without colours | a lit instance's colours read from the light texture across its rows and handed over as constants, by the vertex's slot; the stored colour and the viewer's sun kept elsewhere; the constants sent again when the texture is written |
| effect: through the camera; the fog line | the effect renderer's world positions; the strip's fog from the depth, its slope, offset and bounds |
| effect: textured; added; under the depth test; coordinates across a square; perspective; from far away; the game's smoke; the game's fire | the texel times the colour; the colour weighed by its alpha for an added strip; the reversed depth test; coordinates that differ from vertex to vertex, with and without perspective; coordinates that reach the texture without their perspective correction (a w of 16 puts them all on the first texel); a blended strip and an added sprite with the game's numbers |
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

And on its first day on a console: the first skinned draw stopped the
program at a load from RSX memory wider than its address's alignment
([renderer/rsx](../../renderer/rsx/README.md), "On a console").

What it did not find: `cgcomp -a` marks a `MUL` that reads an interpolated
input as not corrected for perspective, and the game's smoke and fire
were drawn from one texel. Its perspective check had a w between 0.75 and
1.25, which moves a coordinate too little to leave its texel; the check
from far away has a w of 16.
