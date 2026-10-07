# Native on the Switch

The goal: on the Switch, a game is its translated code, libdol-nx and the
console's own libraries (libnx, deko3d) - nothing in between. No Aurora, no
Dawn, no SDL. The desktop builds keep Aurora; the Switch builds leave it, one
section at a time, each one proven by rebuilding a title and running it.

## Sections

Aurora is split into libraries, and libdol-nx links six of them. Each goes in
turn, smallest and most self-contained first.

| # | Section | Aurora | State |
|---|---|---|---|
| 1 | matrices | `aurora_mtx` | **gone**: nothing on the host called it; a game's matrices are its own code or libdol's natives (`accel/sdk/mtx`) |
| 2 | input | `aurora_pad`, `aurora_si`, Aurora's gamepad layer | next |
| 3 | main loop and events | `aurora_core` (window, events, the loop) | |
| 4 | video timing and present | `aurora_vi` | |
| 5 | settings overlay | `aurora_core` (ImGui) | |
| 6 | graphics | `aurora_gx`, Dawn | GX on deko3d |

## 2. Input

Aurora's gamepad layer on the Switch is already libnx HID underneath
(`gamepad_switch.cpp`); its PAD library is generic logic over that layer. So:

1. `src/platform/input/switch_gamepad.cpp` - libnx HID directly: eight players,
   the handheld pair as player one, rumble. Under libdol's own names, so it does
   not clash with Aurora's copy while `aurora_core` is still linked.
2. `src/platform/input/pad_sdk.cpp` - the PAD functions the games' natives and
   libdol's own code use: reading the pads (dead zones, clamping), button, axis
   and alternate mappings with their defaults, rumble, names. Carried over from
   Aurora's `lib/dolphin/pad/pad.cpp` (MIT) without the keyboard and mouse
   bindings, which the Switch has no use for. The mappings keep the same file
   format, so saved mappings carry over.
3. The callers move to it: `pad.cpp`, `input_bindings.cpp`, the settings
   overlay and the mapping wizard here, and libwii-nx's Wii Remote input.
4. `aurora::pad` and `aurora::si` are no longer linked (`cmake/runtime`).

Proven when a title rebuilt without them reads its controllers the same.

## 3-5. Main loop, video timing, overlay

Aurora's Switch window is `nwindowGetDefault()` already; what remains is its
event pump and main loop (`aurora_initialize`, focus, suspend, HOME, docking),
its VI timing, and the settings overlay drawn through ImGui. They become the
libnx applet loop, a present with vsync, and an overlay drawn natively.

## 6. Graphics

GX straight to deko3d, fed by libdol's own GX decoder (`src/platform/gx`):
vertex formats, TEV stages to GLSL compiled on the console by UAM, GX texture
decoding, EFB and XFB copies. Aurora's TEV shader generator and texture
decoders are the reference, adapted rather than linked. Shaders are kept on the
card in one SQLite database on nx-mod/sqlite-nx (a native libnx file layer)
rather than a file each. The native device layer is drafted in aurora-nx's
`deko3d` branch (`lib/deko3d`: device, memory rings, textures, passes, copies,
the screen) and moves here.

Graphics is where the time is: in a Mario Kart Wii race, Aurora building ~480
draws and the runtime walking display lists cost ~13 ms of a 77 ms frame on the
game's core (wii-nx/STATUS.md).

## Build notes

- A title's translated shards build at `-O2 -g0`: debug info is what costs a
  small machine its memory, and `-O0` ran the game's code several times slower.
  On a phone, build at `-j1`.
- Rebuild a title's NRO alone: `cmake --build <title>/build --target <title>_nro`;
  the default target also builds Dawn's demo, which does not build on the Switch.
