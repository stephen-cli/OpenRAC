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
| `rsxgl.cpp` | the display (the console's video mode, two colour buffers and a depth buffer in RSX memory, the flip), the table of every GL function `gl::load` asks for, and the ones carried out |

## What is carried out (2026-11-14)

Every function of `OPENRAC_GL_FUNCTIONS` and `OPENRAC_GL_DESKTOP_FUNCTIONS`
is answered, so `gl::load` finds nothing missing; the compute and
program-binary functions are not, and `gl::load` takes them as absent.
Most answers do nothing yet and return zero. Carried out:

| GL | On the RSX |
|---|---|
| `glGetString`, `glGetIntegerv`, `glGetFloatv` | the layer's name ("4.1 OpenRAC on RSX": the desktop's GL to `gl::api()`), no extensions, a 4096 texture limit, the viewport |
| `glGen*`, `glCreateShader`, `glCreateProgram` | names, counted up from 1 |
| `glGetShaderiv`, `glGetProgramiv`, `glCheckFramebufferStatus` | success: nothing is compiled at run time (the RSX's programs are made ahead of time), no framebuffer is refused |
| `glGetUniformLocation` | -1, which GL ignores a value set at |
| `glBindFramebuffer`, `glClearColor`, `glClear` | the screen's clear (`rsxClearSurface`), depth cleared to 0 for the renderer's reversed depth; an off-screen framebuffer has no memory yet and is left alone |
| `glReadPixels` | the screen as `GL_RGBA` bytes or floats, bottom row first, read from the colour buffer once the RSX has caught up: the port's screenshots (`OPENRAC_SHOT`) work on the console |

`present` flips: it waits for the RSX (a label written at the end of the
command buffer), queues the flip, waits for it, and makes the other
buffer the one drawn into.

So the windowed game runs on the console and its picture is the clear
colour. What comes next, in the order of
[docs/port/PS3.md](../../../docs/port/PS3.md), section 7: textures,
buffers and vertex layouts in RSX memory, the state, the draws, and the
renderer's shaders written again as RSX programs (vertex and fragment
programs in ARB assembly, assembled by `cgcomp -a`:
[tools/ps3/rsx-probe](../../tools/ps3/rsx-probe/README.md)).

Under RPCS3 the read-back needs a real renderer and `Write Color
Buffers: true`, as the probe's.
