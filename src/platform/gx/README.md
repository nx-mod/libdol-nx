# platform/gx

Graphics: the command stream a game writes, turned into draw calls.

A game does not call a graphics API. It writes commands into a FIFO in memory -
vertex data, register writes, display lists - and expects hardware to consume
them. This reads that stream and hands the result to
[aurora-nx](https://github.com/nx-mod/aurora-nx), which renders through WebGPU.

| File | Is |
|---|---|
| `gx_fifo.cpp`, `gx_cp_decode.h` | the command stream and its decoder |
| `gx_dl.cpp` | display lists, which are FIFOs a game keeps and replays |
| `gx_texture.cpp`, `gx_copy.cpp` | textures, and copies out of the embedded framebuffer |
| `gx_tev.cpp`, `gx_pixel.cpp`, `gx_lighting.cpp` | the fixed-function pipeline's state |
| `gx_transform.cpp`, `gx_vertex.cpp`, `gx_indirect.cpp` | matrices, vertex descriptions, indirect stages |
| `gx_frame.cpp`, `gx_init.cpp` | frames, and setting the hardware up |
| `gx_guest_write_hooks.cpp` | noticing when a game rewrites memory something is sampling |

The stream is the same on both consoles: Hollywood is Flipper at a higher clock.
