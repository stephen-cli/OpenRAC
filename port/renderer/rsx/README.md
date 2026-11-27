# renderer/rsx

The renderer's GL carried out with the PlayStation 3's RSX, through
PSL1GHT's `librsx`: library `openrac_renderer_rsx`, built for the console
alone ([cmake/Window.cmake](../../cmake/Window.cmake)). It is to the RSX
what [renderer/vulkan](../vulkan/README.md) is to Vulkan: the window opens
it for `GlApi::Rsx` (`platform/window.h`) and hands its `get_proc` to
`gl::load`, so the renderer calls it as it calls any OpenGL, by the
function pointers of `renderer/gl/gl.h`.

| File | What |
|---|---|
| `rsxgl.h` | `open`, `close`, `present`, `get_proc`, the picture's size |
| `rsxgl.cpp` | the display (the console's video mode, two colour buffers and a depth buffer in RSX memory, the flip), GL's questions about the context, the RSX's state that GL has no say in (alpha test, stencil, logic op, antialiasing control: off, said once at the start), the table of every GL function `gl::load` asks for |
| `rsxgl_internal.h` | GL's objects and state as the layer keeps them |
| `rsxgl_objects.cpp` | RSX memory and the only functions that read and write it, textures and renderbuffers in it, buffers, vertex arrays, samplers, framebuffers, shaders, programs and their uniforms |
| `rsxgl_draw.cpp` | what reaches the RSX: the surface drawn into, GL's state, the programs and their numbers, draws, clears, copies, the read-back |
| `programs/` | the RSX's programs, in the assembly PSL1GHT's `cgcomp -a` takes (which needs no Cg toolkit), and `embed.cmake`, which makes arrays of bytes of the assembled ones (`rsx_programs.h` in the build directory) |

The layer is checked by its pictures in
[tools/ps3/rsxgl-test](../../tools/ps3/rsxgl-test/README.md), a console
program of 124 checks that RPCS3 starts in seconds: a change here is run
there before the game.

## Programs

Nothing is compiled on the console: `glCompileShader` and `glLinkProgram`
succeed at once, and a GL program is carried out by the RSX program made
ahead of time for it. The layer knows which by a line in the GLSL of both
stages (`rsx-program: direct` in `shaders/direct.vert` and `direct.frag`;
a pair may name a program in each, as `effect.vert` with `direct.frag`).
A program without one links too, and its draws are left out: the picture
is what the programs written so far draw. The log says once why draws
are left out (no RSX program, a pass the RSX program has not yet, a
vertex array or indices the RSX cannot read).

| GL program | RSX program | Draws |
|---|---|---|
| `renderer/shaders/direct.vert`, `direct.frag` | `programs/direct.vp`, `direct.fp` | the 2D path: menus, the HUD, text, fades; and the layer's own copies between framebuffers |
| `viewer/shaders/sky.vert`, `sky.frag` | `programs/sky.vp`, `sky.fp` | the sky's shells |
| `viewer/shaders/mesh.vert`, `mesh.frag` | `programs/mesh.vp`, `mesh.fp` | the level as stored: terrain, ties, shrubs, in the solid and the translucent pass |
| the same, where its `skinning` or a pass of the mobys is set | `programs/moby.vp`, `mesh.fp` | the live mobys, skinned and lit as MobyProc lights them: their solid faces, glow faces, the blended ones, the metal packets |
| `renderer/shaders/effect.vert`, `direct.frag` | `programs/effect.vp`, `direct.fp` | the effect renderer's strips and quads: smoke, fire, glows, particles |

A program's uniforms are kept by name in slots of four numbers (a matrix
four slots, an array a slot an element; `rsxgl_objects.cpp` has each
kind's names), and a draw makes the RSX program's constants of them.

`direct.frag` branches on its uniforms (the chip's four texture
functions, fog, the alpha test's six comparisons, the blend's source
weight). `direct.fp` has no branch: each choice is a sum whose weights the
layer works out from the same uniforms for every draw (`direct_constants`),
so one program serves them all. The weights reach the fragment program as
texture coordinates the vertex program copies from its constants. A vertex
program's constants travel in the command buffer with the draw; a fragment
program's are written into the program's own memory, which the RSX may
still be reading for an earlier draw.

A fragment program reads an interpolated input with `MOV` into a register
and with nothing else. `cgcomp` marks a `MUL`, a `DP2`, a `DP3`, a `DP4`, a
`DIV` or an `NRM` that reads one as not corrected for perspective (bit 31
of the instruction's last word), and the chip, as RPCS3, then hands that
instruction the input divided by the pixel's w: right where w is 1, the
2D path, and wrong for everything drawn in the world.
A compiled program's instructions and that bit can be read from its
`.fpo`: a 36-byte header (`rsxFragmentProgram`), then four words an
instruction, each with its halves exchanged.

Not in `direct.fp` yet: text drawn sharp from a distance field (a second
texture), and the texture packs' upscaled colours.

`mesh.fp` is made the same way (the cutout's two passes, the fog and the
alpha written are sums), and `mesh.vp` does what `mesh.vert` does for a
vertex drawn as stored. The GLSL reads a matrix and a tint for each
instance; the RSX has no instances, so an instanced draw is a draw for
each, with the constants the instance changes (nine: the matrix times the
view's, the depth's row, the tint, the normal's rotation) worked out by
the layer from the instance buffer and sent before it. The viewer's sun
is worked out at the vertex, not at the pixel as the GLSL has it.

A lit tie or shrub takes its colours from the light texture in the GLSL,
a texel for each of the instance's light slots. The RSX's vertex programs
read no such texture, so the layer reads the instance's 64 texels where
the texture is in RSX memory and sends them as constants from `c[32]`,
which `mesh.vp` reads through the address register by the vertex's slot.
They are sent once for an instance's draws and again when the texture is
written. `moby.vp` has its joints in the same constants, and the load of
the one ends what the layer knows of the other. A vertex with a slot gets
no viewer's sun.

`effect.vp` is `effect.vert`: world positions through the camera, the
strip's fog line, and `direct.fp`'s numbers handed on as `direct.vp`
hands them.

`moby.vp` is the same GLSL for a live moby. The GLSL reads a moby's joint
matrices from a float texture and its lights from uniform arrays, by
numbers of the instance. An instance is a draw here, so the layer hands
both over as constants: the palette texture (RGBA32F) is kept in main
memory and never reaches the RSX, a draw sends its instance's joints as
three rows each from `c[32]`, and the vertex program reads them through
the address register (`c[A0.x + 32]`). The RSX has 468 constants: 145
joints, where Ratchet has 111, the most of the three levels looked at; a
moby with more is left out and the log says so. How many joints a draw
needs is read from its vertices once and kept until their buffers are
written. The two directional lights (two sets cross-faded), the point
light, the ambient and the glow colour are worked out for the instance
on the CPU. The joints' bytes reach the program as parts of 255.

The mobys' passes (`scene/moby_passes.h`) are the same pair with other
constants. A glow packet's faces are the glow pass's and are left out of
the solid pass, the others the reverse. The glow pass and the blended
mobys are drawn in two draws split at an alpha reference, as the GLSL
splits them; an added moby's colour is weighed by its alpha in the
fragment program, where the chip's alpha goes to about 2. The metal
packets take their texture coordinates from the normal as the camera sees
it, the chip's 0x70 as their colour and the shine their moby's distance
gives as their alpha, which the layer works out for the instance.

Not in the mesh programs yet: the terrain's point lights, the cut at the
picture's sides of a moby of the 2D layer.

Writing a program for `cgcomp -a`: registers are `R0` up, constants
`c[n]`, and the chip's limits are the author's to keep, since the
assembler does not report them. A fragment instruction reads one
interpolated input (it takes several and keeps the last: each input is
moved to a register first). An instruction the chip has none for is left
out without a word (`SUB`: an `ADD` of the negated operand). `R0` is the
colour written.

## What is carried out (2026-11-14)

Every function of `OPENRAC_GL_FUNCTIONS` and `OPENRAC_GL_DESKTOP_FUNCTIONS`
is answered, so `gl::load` finds nothing missing; the compute and
program-binary functions are not, and `gl::load` takes them as absent.
Carried out:

| GL | On the RSX |
|---|---|
| `glGetString`, `glGetIntegerv`, `glGetFloatv` | the layer's name ("4.1 OpenRAC on RSX": the desktop's GL to `gl::api()`), no extensions, a 4096 texture limit, one sample a pixel, the viewport |
| textures: `glTexImage2D`, `glTexSubImage2D`, `glTexParameteri`, samplers | A8R8G8B8 in RSX memory, one level or a chain (below), rows in GL's order, GL's RGBA bytes turned to the RSX's order on the way in; a depth format is 24-bit depth with stencil; a float format is held as eight bits a channel; any other format has no memory and samples white |
| renderbuffers | as textures; one sample a pixel whatever was asked |
| buffers: `glBufferData`, `glBufferSubData` | RSX memory, vertices and indices alike (the element buffer is the vertex array's). A buffer a draw of this frame read is not written in place, since that draw may still be queued: it takes new memory (kept contents for `glBufferSubData`) and the old is left to the draw, without a wait. A texture's writes wait for the RSX first when a queued draw uses the texture |
| vertex arrays | float attributes and normalised bytes, at GL's locations; of a vertex array's attributes the RSX is told those the bound program reads; an attribute with a divisor is one value an instance, read by the layer |
| uniforms: `glUniform1i` to `4f`, `glUniform4fv`, `glUniformMatrix4fv` | kept with the GL program, the RSX program's constants at a draw |
| framebuffers: attachments, `glBindFramebuffer`, `glCheckFramebufferStatus` | a colour texture and a depth texture or renderbuffer as the RSX's surface; every framebuffer is complete |
| state: viewport, scissor, depth test, function, mask and clamp, colour mask, blend (function, equation, colour), cull face, front face | the RSX's, told again before a draw when any of it changed |
| `glClear`, `glClearColor`, `glClearDepth` | `rsxClearSurface`, within the scissor, of what the masks let be written |
| `glDrawArrays`, `glDrawElements` and their instanced forms | with a program the layer has for the RSX; indices of 16 and 32 bits; an instance is a draw |
| `glBlitFramebuffer` | colour, from a framebuffer's texture: a textured rectangle drawn with the direct program, scaled and filtered as asked |
| `glCopyTexSubImage2D` | the RSX's own copy between two pieces of its memory (`rsxSetTransferImage`) |
| `glReadPixels` | `GL_RGBA` bytes or floats from the screen or a framebuffer's colour, read from memory once the RSX has caught up and been taken off that surface: the port's screenshots (`OPENRAC_SHOT`) work on the console |
| `glFinish` | waits for the RSX |

Answered and not carried out yet: stencil, depth copies between
framebuffers, float textures, compressed textures, uniform blocks.

## Mip chains

`glGenerateMipmap` makes a texture's chain on the CPU, each texel the
mean of the four above it, down to one texel. The levels follow one
another in one piece of RSX memory, every level with the first's pitch:
the RSX's layout of a texture that is not swizzled, so a chain takes
about twice its first level where a tight one takes a third more
(Veldin: 31 MiB of first levels). A texture with a chain takes GL's
filters between levels as the RSX's own, and the RSX chooses the level
from the texture coordinates' change across a pixel, as a desktop GPU
does for the same GL calls; a texture of one level takes such a filter
as the filter within the level. Levels are not uploaded one by one
(`glTexImage2D` of a level other than the first does nothing), and no
anisotropic filter is asked for. Seen under RPCS3 (2026-11-26); on a
console not yet.

## Tiled surfaces

What the RSX draws into is a tiled region of its memory: rows of one of
the RSX's own pitches (`gcmGetTiledPitchSize`), from a multiple of 64 KB
and 64 KB whole, bound with `gcmSetTileInfo` and `gcmBindTile` (no
compression). The screen's buffers are laid out so, and so is a
renderbuffer and a texture given no texels, which is a framebuffer's;
a surface's region is bound when the surface is set, and the RSX's 15
regions go to the surfaces last drawn into. The bytes are where they
would be without a region, so a surface is read back and sampled as any
other image.

Measured on a console (2026-11-27, 20 layers of 1440 by 1080 with a
fragment program of three instructions): a blended, depth-tested pixel
takes 3.07 ms a million in memory the RSX was told nothing of and 1.03
in a region; a plain one 0.94 and 0.60. With the layer's long fragment
programs the program is the limit either way (2.27 and 2.25), which is
why the regions alone change little in the game. The game's pictures on
the console are the same byte for byte with and without them.

## Which way up

A texture's and an off-screen framebuffer's rows are in GL's order in
memory, the first row y = 0, so what is drawn into a texture is sampled as
GL samples it and no texture coordinate is turned. The screen's rows run
downwards. The RSX's window coordinates count rows of memory, so the
viewport and the scissor turn y over on the screen and nowhere else, and
with them the winding that faces front.

## Waiting for the RSX

The RSX runs behind the program: a draw is queued and reads its vertices
and textures later. `present` waits for it (a label written at the end of
the command buffer), queues the flip, waits for that, and makes the other
colour buffer the one drawn into. Memory GL gave up during the frame
(`glDeleteTextures`, a buffer's old store, a copy's vertices) is given
back to the RSX's allocator then, when nothing queued reads it. A wait
that lasts five seconds says so once in the log, with where the RSX is
reading its commands: a queue it has stopped reading is otherwise a
program that hangs in silence.

Every 120 frames the verbose log has a line of what the layer did
(`rsxgl: 120 frames: 904 draws, 0 left out, 120 copies`): draws left out
are draws of a program the layer has no RSX program for, and no draws at
all is a renderer that was given nothing to draw.

## Against the desktop's picture

The port's own OpenGL renderer, built for x86 and run under Mesa's
software rasteriser with the console run's arguments, writes the same
frames (`OPENRAC_SHOT`), and the two are compared pixel for pixel. On
Veldin (2026-11-25), frames of the opening without effects in view differ
in 0.2% of their pixels by more than 16 of 255 in a channel. Smoke and
fire are drawn since; their frames have not been measured again (the
reference is of the tree before the last merge of `master`).
Also missing: the shadows and the haze, whose programs the layer has not.

## On a console

The layer's test passes on a console (a CECHA, 2026-11-25) and the game
draws there what it draws under RPCS3. What the console asks that the
emulator does not:

- **RSX memory is read and written in its own alignment.** A load of
  it wider than its address's alignment stops the program: an 8-byte
  load at an address that is a multiple of 4 and not of 8, which GCC
  makes of a 16-byte `memcpy` and may make of two neighbouring fields.
  Loads of a word at a word's place and of bytes are fine. So nothing in the
  layer takes a pointer into RSX memory and reads or writes through
  it: `read_memory`, `write_memory`, `fill_memory`, `read_word` and
  `write_word` (`rsxgl_internal.h`) move a word at a time where the
  RSX's side is a word's place and a byte at a time before and after,
  through volatile pointers, which the compiler does not join.
- A program that does not end by itself is ended from outside only by
  restarting the console: the layer does not yet hear the XMB's
  request to quit.

## Under RPCS3

What the emulator needs of the layer, none of it wrong on the console:

- `Shader Mode: Shader Recompiler`. With the asynchronous one a draw is
  left out while its pipeline is compiled, and a check that draws once
  reads the picture without it.
- The first draw into a framebuffer that has never been drawn into is
  lost; the same draw after it is not. Seen in the layer's test, not
  explained.
- A read-back needs a real renderer and `Write Color Buffers: true`, as
  the probe's ([tools/ps3/rsx-probe](../../tools/ps3/rsx-probe/README.md)).
  RPCS3 writes a surface back to the program's memory when it stops
  being the target, so `glReadPixels` points the RSX at the screen's
  other colour buffer before it reads.
- Onto an X8R8G8B8 surface it blended with a source alpha of one. The
  screen's surface is A8R8G8B8 (its alpha is never shown); the console
  itself was not asked.
