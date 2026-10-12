# The GPU on deko3d (Nintendo Switch)

Section 6 of [native.md](native.md): GX drawn by the console's own GPU library,
deko3d, with nothing in between - no Aurora renderer, no WebGPU, no Dawn, no
Tint, no NVK.

## The native layer (`src/platform/gpu`)

| File | What |
|---|---|
| `dk.h` | the layer: device, queue, buffers, textures, views, samplers, passes, copies, the screen |
| `dk_device_switch.cpp` | the device, the frame's command and staging rings, the descriptor sets |
| `dk_resources_switch.cpp` | buffers, textures, views, writes into them |
| `dk_frame_switch.cpp` | passes, copies, the screen (1920x1080 images, cropped to 720p in handheld) |
| `dk_shader_switch.cpp` | shaders: GLSL compiled by UAM on the console, loaded into code memory, kept for the run |
| `dk_gx_renderer_switch.cpp` | GX draws: the state's shaders, uniforms, converted vertices, pixel state and viewport, and its textures - decoded from guest memory, mips and palettes, cached by a sampled hash, with their samplers (`draw_gx`) |
| `gx_native_switch.cpp` | the renderer GX feeds: the FIFO buffered and run in batches, frames opened at the first draw and presented at the display copy, EFB copies kept on the GPU by guest address, per-second counts in boot.log |
| `aurora_off_switch.cpp` | what libdol still asked of Aurora - its start, events and shut-down, GX calls that only fed its copy of the state, its VI helpers and counters - done natively, so Aurora and Dawn are not linked |
| `dk_gx_state_switch.cpp` | GX's pixel state (`wiinx/format/gx/pixel.hpp`: blending, logic op, depth, culling, write masks) as deko3d state objects |

UAM is built for the console as Halo's port builds it (meson, bison, flex,
mako; a pinned commit with `uam.patch`), found at `WIINX_UAM_BUILD_DIR` -
beside this checkout in `haloce-nx/build/switch` unless pointed elsewhere.
`src/uam/uam_compile.cpp` is compiled with UAM's flags and merged with its
library into one object with every symbol renamed but `dol_dk_compile_glsl`
(`tools/wiinx-uam-object`): UAM is Mesa's GLSL compiler, as is the Mesa that
NVK brings, and linked plainly they collide. Without a UAM build the link
still succeeds and shaders do not compile.

Two console tests build alone with `tests/deko3d/build.sh`: `triangle_test.nro`
(the layer and UAM) and `gx_test.nro` (display lists through the command
processor and `draw_gx`, onto the screen).

It came from the draft in aurora-nx's `deko3d` branch and is compiled into every Switch build, linked against
`libdeko3d`, but nothing calls it yet: the game still draws through Aurora.

The files keep file-local helpers of the same names, so they are built outside
the runtime's unity batches (`cmake/runtime/Runtime.cmake`).

## Done, portable (`wiinx::gx`, checked on any machine)

| Header | What |
|---|---|
| `wiinx/format/gx/texture.hpp` | every GX texture format decoded to RGBA8, palettes, mips (`gx_texture_check`: texel by texel) |
| `wiinx/format/gx/tev.hpp` | the TEV as a GLSL fragment shader: sixteen stages in the hardware's 8-bit arithmetic, compares, swaps, konst colours, the alpha test |
| `wiinx/format/gx/vertex.hpp` | XF as a GLSL vertex shader: matrices, projection, the lit colour channels, texgens, dual texturing |
| `wiinx/format/gx/pixel.hpp` | GX's pixel engine state: blending, logic op, depth, culling, write masks |
| `wiinx/format/gx/vertex_stream.hpp` | GX's packed vertex records (every component and colour format) to the vertex shader's float layout |
| `wiinx/format/gx/command.hpp` | the command stream run against the state: CP vertex tables, XF and BP loads, indexed loads, called lists, draws with indexed attributes fetched (`gx_command_check`) |
| `wiinx/format/gx/state.hpp` | GX's state as the SDK's setters change it, and what a draw takes from it: both shader configurations, the pixel state, the uniform blocks' bytes; from the SDK's setters or the hardware's BP and XF registers (`gx_state_check`) |

`gx_tev_check` and `gx_vertex_check` compile what they generate with devkitPro's
`uam` - the compiler the console runs - wherever it is installed. Not yet:
indirect textures, fog, the z texture, bump texgens.

## How it replaces Aurora

The default on the Switch since 2026-10-12 (`WIINX_NATIVE_GX`, on). Aurora and
Dawn are not linked: Mario Kart Wii's NRO went from 139 MB to 115 MB.

1. **The game sets GX itself.** libdol's replacements of the SDK's GX setters
   existed to bridge into Aurora; natively the SDK's own setters write the
   hardware's registers through the gather pipe into `wiinx::gx::CommandProcessor`,
   so those natives are not registered (`#if !defined(WIINX_NATIVE_GX)` around
   each registration) and the game's own code runs, `GXInit` included. What
   stays native stands in for hardware: the FIFO and display-list plumbing,
   draw sync and the PE interrupts, and the copies. The CP, PE, PI, MI and SI
   registers the SDK reaches directly are register files (`src/cpu/memory.cpp`).
2. **The translator and the runtime agree.** A game's `recomp.yml` says which
   build it is (`runtime.defines`); the translator reads native sources as that
   build compiles them, so a function whose native drops out is translated, and
   `wiinx-build` builds the runtime the same way. With no list the default is
   `[WIINX_NATIVE_GX]`; `defines: []` is Aurora's build.
3. **Frames.** A frame opens at its first draw on the screen's image, cleared
   by the copy clear registers, and VI presents it at `GXCopyDisp`, paced to
   the retrace. `GXCopyTex` copies the EFB into an image kept for the guest
   address; a texture bound there samples it until the guest rewrites that RAM.
4. **What is left of Aurora** is its application layer's API, done in
   `aurora_off_switch.cpp`: HOME and quitting from libnx's applet loop, the
   controllers from libdol's native input. The settings overlay (ImGui on
   Aurora's renderer) is not drawn until it has a deko3d version.

## Lessons from deko3d

- **Free nothing the frame has recorded.** Draws recorded but not submitted
  still read what they bound. Textures, buffers and descriptor slots are
  retired with the frame and freed once its fence has passed (`retire`).
- **`dkCmdBufClear` does not drop a command buffer's memory**; it rewinds to
  the start of the piece it was last given. At each frame's end the buffers are
  cleared and given a fresh piece of the new frame's slice at once.
- **`dkCmdBufBeginCaptureCmds` clears the buffer it is given**: capture on a
  buffer of its own (`dk::capture()`), never on the frame's.
- **`dkCmdBufBindVtxBuffers` reserves five words and writes six** (deko3d's
  source as of 2026). It is captured and replayed, which reserves what it
  wrote.
- **Bound targets must agree in size.** The screen's images are 1920x1080
  whatever handheld crops them to; the depth buffer is the same size.
- **Many draws in one command list went wrong on the GPU** - a "GPU method
  error" on a data-like word, a different method each run, after a few hundred
  frames. Each draw submitted as its own list does not (`kSubmitEveryDraw`).
  The cause is not found yet; that is the workaround.
- **deko3d's debug build names the fault.** `-DWIINX_DEKO3D_DEBUG=ON` links
  `libdeko3dd`, whose messages reach boot.log.

## Where it stands

2026-10-12, Mario Kart Wii: boots, every draw accepted, stable through the
menus, about 20-26 fps there with ~220 draws a frame. The profile is 82% the
game's own translated code, 18% the runtime - the renderer included - so the
next speed is in the game's code: the GX setters now run as translated code
and feed the gather pipe a word at a time.

## What comes next

1. Speed: a cheaper gather-pipe path; native fast paths for the hottest GX
   setters straight into `State`; `nw4r::g3d` natives ([performance](performance.md)).
2. EFB copies in their own formats (depth, intensity) and copies the CPU reads.
3. The settings overlay drawn by deko3d.
4. Why large command lists fault, so draws can share lists again.
5. The compiled shaders kept on the card (nx-mod/sqlite-nx), keyed by the
   source's hash, so only a first run compiles them.
