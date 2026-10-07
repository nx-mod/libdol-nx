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
| 2 | input | `aurora_pad`, `aurora_si`, Aurora's gamepad layer | under way: native input in, Wii Remote on it; the PAD SDK next |
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
   overlay and the mapping wizard.
4. `aurora::pad` and `aurora::si` are no longer linked (`cmake/runtime`).

### libwii-nx's part

libwii-nx's `wii_remote_input.cpp` reaches real remotes through SDL3's HIDAPI
Wii driver on the desktop; on the Switch its own branch already read libnx, as a
Wii Remote + Classic Controller per pad, but with a HID reader of its own and no
motion. It now reads `dol::input` (one HID reader in the program; done), and its
own TODO's "an input interface in libdol first" is answered. Still to come:

- the interface carries motion as well (libnx's six-axis sensors: accelerometer
  and gyro), not only buttons, sticks and rumble
- WPAD/KPAD read it directly: a Joy-Con (the right one, or one held sideways) is
  the Wii Remote - buttons, its accelerometer, the pointer from its gyro with
  the stick as the fallback - and the left Joy-Con is the Nunchuk, with its own
  motion. That is also the motion libwii-nx's TODO lists as missing (Wii Sports)

The remote is filled in `WiiRemoteInput::ReadKpadSample`'s terms (WPAD button
bits, accelerometer in g with the KPAD frame's rest at y = -1, Nunchuk stick and
motion), so WPAD and KPAD themselves do not change; a Switch file fills the
sample from `dol::input` beside the desktop's SDL one.

| Wii | Joy-Con pair, handheld, Pro Controller | one right Joy-Con, sideways |
|---|---|---|
| A / B | A / ZR (B is a trigger on the remote) | - |
| 1 / 2 | Y / X | Y / A (the remote sideways: 1 and 2 are its face) |
| + / - / Home | + / - / right stick click | + / - / Home-side stick click |
| D-pad | left D-pad | the stick |
| remote motion | right half's accelerometer | its accelerometer, axes turned sideways |
| pointer | right half's gyro, right stick as the fallback | gyro |
| Nunchuk stick | left stick | - |
| Nunchuk C / Z | L / ZL | - |
| Nunchuk motion | left half's accelerometer | - |

The accelerometer's axes are the Joy-Con's turned into the remote's (held
upright, or sideways for the wheel games); the exact signs are measured on the
console, a remote on a table reading y = -1.

Real Wii Remotes over Bluetooth (libwii-nx's `wud`) need Bluetooth HID access
libnx does not give homebrew; a later question, not this section.

Proven when a title rebuilt without them reads its controllers the same, and
a Wii Remote game reads a Joy-Con as its remote.

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

## Others' wins to take

dorPXP/driftdroid (GPL-3.0, as libdol-nx is; credited where taken) runs Mario
Kart Wii on the Switch at ~30 fps in a stock 12-kart race and 60 in time trials,
up from ~18, and most of what did it is in the runtime, not the renderer
(driftdroid `runtime/`, commits of 2026-09-17 to 09-27):

- [ ] native AArch64 paired-single, `psq` and single-rounding helpers
      (`runtime/include/isa/ppc_isa_float.h`, `ppc_isa_quantized.h`): the
      translated code itself, bit-exact against the previous build; FPCR.FZ for
      the NI flush
- [ ] display-list caches (from chrissotraidis/kartpad `e3cb77f`, `b435655`):
      register-only classifications cached, a direct-mapped front for the scan
      cache, vertex layouts rehashed only where a writer marked them stale, XF
      loads batched in FIFO bursts (`runtime/src/hle/gx/gx_dl.cpp`)
- [ ] the build: functions ordered from race profiles, PGO generate/use (with
      their libgcov TLS and devkitA64 assembler workarounds), hidden visibility
- [ ] thread placement and priorities for the GX worker, the AX mixer and audio
      output
- [ ] audio: the mixer's own counters (`ax_mix.cpp`) to see its cost
- (their fewer-GX-worker-syncs work is Aurora's half; the native renderer
  replaces it rather than taking it)

These are independent of Aurora leaving, and apply to every game.

## Build notes

- A title's translated shards build at `-O2 -g0`: debug info is what costs a
  small machine its memory, and `-O0` ran the game's code several times slower.
  On a phone, build at `-j1`.
- Rebuild a title's NRO alone: `cmake --build <title>/build --target <title>_nro`;
  the default target also builds Dawn's demo, which does not build on the Switch.
