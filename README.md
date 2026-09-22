# libwii-nx

The Wii as a library, for statically recompiled Wii games running natively on
Nintendo Switch.

[wiicompiled-nx](https://github.com/nx-mod/wiicompiled-nx) translates a game's
PowerPC code to C++ and runs it on a guest CPU runtime. libwii-nx is everything
else the game expects of the console. Together they are all it takes to build a
game from the player's own disc:

```
translated game code   +   libwii-nx                  +   your own data
(wiicompiled-nx)           platform + accelerators        disc image, NAND
```

The translated code calls in at the same function boundaries the game called
Nintendo's SDK at, so a fix here reaches every game that uses what was fixed.

## Design

```
include/wiinx/     public headers - all a caller sees
  core/  platform/  accel/
src/
  core/            types, typed guest access, the host interface, the registry
  platform/        the console itself - required by every game
    os  fs  gx  audio  input  system
  accel/           native versions of libraries games link - optional, for speed
    sdk  nw4r  egg  jsystem  rfl
tools/
  wiinx-scan       finds library builds and functions in a game's DOL
```

Every part is its own static library with its own README. Dependencies point one
way: `accel` → `platform` → `core`. Underneath, libwii-nx builds on
[aurora-nx](https://github.com/nx-mod/aurora-nx) (GX to WebGPU),
[dawn-nx](https://github.com/nx-mod/dawn-nx) (WebGPU to Vulkan),
[nxvk](https://github.com/nx-mod/nxvk) (the Vulkan driver),
[sqlite-nx](https://github.com/nx-mod/sqlite-nx) and libnx. Those stay separate
libraries; a game project only sees libwii-nx.

### platform - the console

What a game's SDK talks to when it touches hardware.

| Module   | Is                                                              |
|----------|-----------------------------------------------------------------|
| `os`     | threads, alarms, interrupts, time, caches, mutexes              |
| `fs`     | disc (DVD), NAND saves, IOS/ES file access                      |
| `gx`     | graphics: the GX FIFO into Aurora, decoded on its own core      |
| `audio`  | AI/DSP/AX into Switch audio                                     |
| `input`  | GameCube pad, Wii Remote (KPAD/WPAD) from Joy-Con               |
| `system` | video timing (VI), settings (SC), IPC, power                    |

### accel - fast versions of linked libraries

A game runs without them, only slower. Every native gives the original's exact
results: gameplay depends on it (ghosts, online).

| Module    | Covers                                                      |
|-----------|-------------------------------------------------------------|
| `sdk`     | C library (memcpy, exact math), matrix math, GD, THP video  |
| `nw4r`    | lyt (layouts), g3d (models, animation), snd, ef, math       |
| `egg`     | Nintendo EAD's framework                                    |
| `jsystem` | the GameCube-era framework                                  |
| `rfl`     | Miis, Home Button menu                                      |

## Natives and versions

Games link different builds of the same library and name them in a startup
banner:

```
Wii Sports      NW4R - LYT  Jun  8 2007  (0x4199_60831)
Mario Kart Wii  NW4R - LYT  Mar  8 2008  (0x4201_127)
```

A native is registered under the original function's name and the library
build it matches, in one plain table per module:

```cpp
// src/accel/nw4r/lyt/pane.cpp
WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kLyt_2007_06, calculate_mtx<PaneLayout2007>),
WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kLyt_2008_03, calculate_mtx<PaneLayout2008>),
```

Every known build compiles in side by side. When two builds differ only in
struct layout, one implementation takes a layout table; when behavior changed,
that build gets its own function. Game structures are typed, big-endian fields
at each build's offsets:

```cpp
Guest<PaneLayout2008> pane{address};
u8 alpha = pane[PaneLayout2008::alpha];
```

## Binding a game

```
wiinx-scan game.dol  →  bindings.json  →  wiicompiled-nx  →  direct native calls
```

`wiinx-scan` reads the library banners and matches each function's code
signature, then writes the game's `bindings.json`: which native, of which build,
answers each address. The translator bakes that in, so a bound call costs
nothing at runtime. A function nothing matches stays the game's own translated
code: an unknown build is slower, never wrong.

## Building a game

Once complete, one command:

```
wii-nx build mygame.iso   →   mygame.nro + sdmc:/wii-nx/games/mygame/
```

extracts the disc, identifies the game, scans it, translates it, builds against
libwii-nx and packages the NRO. Games are only ever built on the player's
machine from their own disc; CI builds the library and tools, never a game.

## Status

| Part                              | State                                              |
|-----------------------------------|----------------------------------------------------|
| [`core`](src/core/README.md)      | done: types, typed access, host, builds, registry  |
| [`platform`](src/platform/README.md) | working in wiicompiled-nx's runtime, to port here |
| [`accel/sdk`](src/accel/sdk/README.md) | THP decoder ported                             |
| [`accel/nw4r`](src/accel/nw4r/README.md) | lyt `Pane::CalculateMtx` ported, 2007 and 2008 builds |
| `accel/nw4r` g3d CalcWorld/CalcView | located in Mario Kart Wii, not written           |
| `wiinx-scan`                      | not started                                        |
| one-command build                 | its steps exist as scripts in wii-nx               |

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
cmake --build build
```

## License

GPL-3.0-or-later, as wiicompiled-nx, which the platform layer comes from.
Behavior and layouts follow the decompilation projects doldecomp/ogws,
doldecomp/mkw and projectPiki/pikmin2 (all CC0); the THP IDCT is based in part
on the work of the Independent JPEG Group. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No game code or data is
included or distributed.
