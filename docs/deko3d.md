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
| `dk_gx_renderer_switch.cpp` | GX draws: the state's shaders, uniforms, converted vertices, pixel state and viewport, drawn (`draw_gx`); textures not yet bound |
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

## What comes next

GX is decoded by libdol already (`src/platform/gx`: display lists, vertex
formats, the CP and XF state). What is missing is the half that turns a frame's
GX state and draws into GPU work:

1. Pipelines: GLSL compiled on the console by UAM into DKSH, kept in memory
   by the source's hash for the run. Blend, depth, rasterizer and vertex-input
   state as deko3d state objects. Halo: Combat Evolved's deko3d port is the
   reference (nx-mod/haloce-nx, `port/switch/host/host_dk*.c`).
2. The TEV stages as GLSL. Aurora's WGSL generator (`lib/gx/shader.cpp`) is the
   reference for what each stage computes; it is adapted, not linked.
3. Textures: GX formats decoded (or, where the GPU has a match, uploaded as
   they are), TLUT palettes, the texture cache.
4. Draws: vertex and index data from libdol's display-list decoder into the
   frame's staging ring, one pass per EFB.
5. EFB and XFB copies, with their format conversions; then the present.
6. The settings overlay drawn without ImGui-on-WebGPU (section 5).

Once a title draws on it: the compiled shaders kept on the card in one SQLite
database (nx-mod/sqlite-nx) keyed by the source's hash - compiled once, never
again, and shippable as keys so a new install compiles in the background at
first start, as Halo's port does. Not before: the cache comes after the
renderer runs.

Each lands behind the Aurora renderer until a title draws its first frame on
it; then Aurora's `aurora_gx`, `aurora_vi` and Dawn leave the Switch build.
