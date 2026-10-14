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

1. **The game sets GX itself.** The SDK's own setters write the hardware's
   registers through the gather pipe into `wiinx::gx::CommandProcessor`; the
   libdol replacements that bridged into Aurora are not registered
   (`#if !defined(WIINX_NATIVE_GX)`). What stays native stands in for hardware:
   the FIFO and display-list plumbing, draw sync, the PE interrupts, the
   copies. Natives that end a batch of state (`GXCallDisplayList`,
   `GXBeginDisplayList`, `GXDrawDone`, `GXFlush`, `GXDrawSync`) first run the
   game's `__GXSetDirtyState`, as the SDK does - skipping it left projections
   and viewports unsent.
2. **The translator and the runtime agree.** A game's `recomp.yml` names the
   build (`runtime.defines`); with no list the default is `[WIINX_NATIVE_GX]`,
   and `defines: []` is Aurora's build. The translated code and a game's own
   natives are compiled with the define too (`mkw_configure_translated_target`).
   Until 2026-10-13 they were not: the direct-call catalog
   (`native_cpu_calls.inc`) still sent calls to Aurora's GX bridge, and a
   game's own natives written for Aurora ran in the native build.
3. **A render thread.** The game thread only queues what it writes (FIFO
   bytes, list calls, copies, the frame's end). A thread pinned to core 2 runs
   the command processor, the state, vertex conversion and deko3d.
   `GXDrawDone` waits for it, as the hardware's draw done waits for the GPU,
   and at most one frame is in flight (`kThreaded`, `kDrawDoneWaits`).
4. **Draws merged.** Draws that share a state go to the GPU as one: quads,
   triangles, strips (winding kept) and fans become one indexed triangle list,
   lines and line strips one line list. Register writes that change nothing
   are skipped (`State::BpUnchanged`, `XfUnchanged`), so they do not split a
   batch. 32 draws a command list (`kDrawsPerList`).
5. **The EFB.** A 528-line target (Wii lines scaled to the screen), depth
   beside it. The display copy's source (`GXSetDispCopySrc`, 608x456 in Mario
   Kart Wii) is stretched over the screen as VI stretches the XFB.
   A display copy is kept as that XFB's image, and the screen shows it from the
   flip (`VIFlush`), not at the copy: a game may copy into one XFB more than
   once (Mega Man 9 copies a cleared EFB, then the frame scaled up) and a Wii
   shows only the last. Three copies with no flip present anyway. The next
   frame starts from a cleared EFB only when the copy asked for a clear.
   Lines and points take `GXSetLineWidth`/`GXSetPointSize` (BP 0x22, sixths
   of a pixel) scaled to the EFB; at one GPU pixel, a picture drawn from lines
   came out striped.
6. **EFB copies.** `GXCopyTex` runs a copy pass into an image kept for the
   guest address: box-filtered when halved, the copy filter's vertical taps,
   converted to the copy's format (I4/I8/IA4/IA8, RGB565, RGB5A3, RGBA8, the
   R/G/B/A/RG/GB colour formats, Z formats from depth). A copy from an EFB
   without alpha (`PE_CONTROL`'s format) is opaque, and a blend reading
   destination alpha then sees 1. A copy's clear covers its source rectangle
   only, and only the channels the update masks let through (Dolphin's
   `ClearScreen`). The clear flag is the argument's low bit, as the game's
   `GXCopyTex` (`rlwimi r3,r4,11,20,20`) puts it in the register.
7. **Shaders kept on the card.** Each compiled shader is appended to
   `cache/shaders.pack` (tag `WXSHPK01`; key = FNV-1a of source and stage), so
   a second run compiles only what it has not seen.
8. **What is left of Aurora** is its application layer, done in
   `aurora_off_switch.cpp`: HOME and quitting, the controllers. The settings
   overlay is not drawn yet; a frame-rate overlay is (`kOverlay`, top left:
   frames shown in the last second and the slowest of them).

## Diagnostics, off by default

Each costs real time, so each is a constant to turn on for one hunt:

| | where | does |
|---|---|---|
| `kProbePeriodic` | `dk_gx_renderer_switch.cpp` | every 120th frame: GPU counters after every draw (clipped, culled, depth-failed), state audit, overlay and model descriptions |
| probe focus | same | always on, 40 frames a run: frames that draw into an alpha-keeping EFB or start a render to a texture, each draw logged as `gx-mii` |
| `kSnapshots` | `gx_native_switch.cpp` | the EFB saved as `logs/snap_*.ppm` at set draws and at each new copy address |
| `kPcSampler` | `app/main.cpp` | the game thread's PC a thousand times a second into `logs/pcsamples.bin` |

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
- **A whole frame in one command list went wrong on the GPU** - a "GPU method
  error" on a data-like word. A list per draw did not, and neither do lists of
  32 draws (`kDrawsPerList`), which keep the kernel calls down. The cause is
  not found.
- **deko3d's debug build names the fault.** `-DWIINX_DEKO3D_DEBUG=ON` links
  `libdeko3dd`, whose messages reach boot.log.

## Where it stands

2026-10-13, Mario Kart Wii: menus, the flyover and races draw. Open:

- **Miis**: the face texture gets its eyebrows but not its eyes or mouth (the
  three-stage TEV draws in `RFLiCapture`); heads and icons draw nothing. It
  went with the copy-clear fix, which is right as hardware - the fix exposed a
  second bug that clearing everything had hidden.
- **The race minimap** is misplaced and its course outline missing.
- **Bump-alpha colour channels** are not implemented.
- **Speed**: a race runs about 12 fps; see [performance](performance.md).

## What comes next

1. The Mii draws, from the `gx-mii` trace.
2. Vertex decode on the GPU: the render thread converts every vertex to a
   128-byte record on the CPU; the game's own vertex bytes decoded in the
   vertex shader would cut that work and the upload.
3. The settings overlay drawn by deko3d.
