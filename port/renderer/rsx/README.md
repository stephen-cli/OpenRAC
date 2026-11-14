# renderer/rsx

The renderer's GL carried out with the PlayStation 3's RSX, through
PSL1GHT's `librsx`: library `openrac_renderer_rsx`, built for the console
alone ([cmake/Window.cmake](../../cmake/Window.cmake)). It is to the RSX
what [renderer/vulkan](../vulkan/README.md) is to Vulkan: the window opens
it for `GlApi::Rsx` (`platform/window.h`) and hands its `get_proc` to
`gl::load`, so the renderer calls it as it calls any OpenGL, by the
function pointers of `renderer/gl.h`.

| File | What |
|---|---|
| `rsxgl.h` | `open`, `close`, `present`, `get_proc`, the picture's size |
| `rsxgl.cpp` | the display (the console's video mode, two colour buffers and a depth buffer in RSX memory, the flip), GL's questions about the context, the table of every GL function `gl::load` asks for |
| `rsxgl_internal.h` | GL's objects and state as the layer keeps them |
| `rsxgl_objects.cpp` | textures and renderbuffers in RSX memory, buffers, vertex arrays, samplers, framebuffers, shaders, programs and their uniforms |
| `rsxgl_draw.cpp` | what reaches the RSX: the surface drawn into, GL's state, the programs and their numbers, draws, clears, copies, the read-back |
| `programs/` | the RSX's programs, in the assembly PSL1GHT's `cgcomp -a` takes (which needs no Cg toolkit), and `embed.cmake`, which makes arrays of bytes of the assembled ones (`rsx_programs.h` in the build directory) |

The layer is checked by its pictures in
[tools/ps3/rsxgl-test](../../tools/ps3/rsxgl-test/README.md), a console
program of 55 checks that RPCS3 starts in seconds: a change here is run
there before the game.

## Programs

Nothing is compiled on the console: `glCompileShader` and `glLinkProgram`
succeed at once, and a GL program is carried out by the RSX program made
ahead of time for it. The layer knows which by a line in the GLSL of both
stages (`rsx-program: direct` in `shaders/direct.vert` and `direct.frag`).
A program without one links too, and its draws are left out: the picture
is what the programs written so far draw.

| GL program | RSX program | Draws |
|---|---|---|
| `direct.vert`, `direct.frag` | `programs/direct.vp`, `direct.fp` | the 2D path: menus, the HUD, text, fades; and the layer's own copies between framebuffers |

`direct.frag` branches on its uniforms (the chip's four texture
functions, fog, the alpha test's six comparisons, the blend's source
weight). `direct.fp` has no branch: each choice is a sum whose weights the
layer works out from the same uniforms for every draw (`direct_constants`),
so one program serves them all. The weights reach the fragment program as
texture coordinates the vertex program copies from its constants. A vertex
program's constants travel in the command buffer with the draw; a fragment
program's are written into the program's own memory, which the RSX may
still be reading for an earlier draw.

Not in `direct.fp` yet: text drawn sharp from a distance field (a second
texture), and the texture packs' upscaled colours.

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
| textures: `glTexImage2D`, `glTexSubImage2D`, `glTexParameteri`, samplers | one level of A8R8G8B8 in RSX memory, rows in GL's order, GL's RGBA bytes turned to the RSX's order on the way in; a depth format is 24-bit depth with stencil; a float format is held as eight bits a channel; any other format has no memory and samples white |
| renderbuffers | as textures; one sample a pixel whatever was asked |
| buffers: `glBufferData`, `glBufferSubData` | RSX memory. A buffer a draw of this frame read is not written in place, since that draw may still be queued: it takes new memory (kept contents for `glBufferSubData`) and the old is left to the draw, without a wait. A texture's writes wait for the RSX first when a queued draw uses the texture |
| vertex arrays | float attributes and normalised bytes, at GL's locations |
| framebuffers: attachments, `glBindFramebuffer`, `glCheckFramebufferStatus` | a colour texture and a depth texture or renderbuffer as the RSX's surface; every framebuffer is complete |
| state: viewport, scissor, depth test, function, mask and clamp, colour mask, blend (function, equation, colour), cull face, front face | the RSX's, told again before a draw when any of it changed |
| `glClear`, `glClearColor`, `glClearDepth` | `rsxClearSurface`, within the scissor, of what the masks let be written |
| `glDrawArrays` | with a program the layer has for the RSX |
| `glBlitFramebuffer` | colour, from a framebuffer's texture: a textured rectangle drawn with the direct program, scaled and filtered as asked |
| `glCopyTexSubImage2D` | the RSX's own copy between two pieces of its memory (`rsxSetTransferImage`) |
| `glReadPixels` | `GL_RGBA` bytes or floats from the screen or a framebuffer's colour, read from memory once the RSX has caught up and been taken off that surface: the port's screenshots (`OPENRAC_SHOT`) work on the console |
| `glFinish` | waits for the RSX |

Answered and not carried out yet: indexed and instanced draws, stencil,
depth copies between framebuffers, mip chains (`glGenerateMipmap`: a mip
filter is linear within the one level), compressed textures, uniform
blocks and the matrix uniforms (no program uses them yet).

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

## Under RPCS3

What the emulator needs of the layer, none of it wrong on the console:

- A read-back needs a real renderer and `Write Color Buffers: true`, as
  the probe's ([tools/ps3/rsx-probe](../../tools/ps3/rsx-probe/README.md)).
  RPCS3 writes a surface back to the program's memory when it stops
  being the target, so `glReadPixels` points the RSX at the screen's
  other colour buffer before it reads.
- Onto an X8R8G8B8 surface it blended with a source alpha of one. The
  screen's surface is A8R8G8B8 (its alpha is never shown); the console
  itself was not asked.
