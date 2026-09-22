# libwii-nx

The Wii as a library, for statically recompiled Wii games running natively on
Nintendo Switch.

It is the whole toolkit: the translator that turns a game's PowerPC code into
C++, the CPU runtime that code runs on, the console it expects, native versions
of the libraries it links, and the tools for discs, NANDs and builds. A game
project needs libwii-nx and nothing else. With it, a game is built from the
player's own disc:

```
your disc  →  libwii-nx translator  →  game code + libwii-nx runtime  →  NRO
```

The translated code calls in at the same function boundaries the game called
Nintendo's SDK at, so a fix here reaches every game that uses what was fixed.
Parts still being ported in are tracked in [docs/porting.md](docs/porting.md).

## Design

```
include/wiinx/     public headers - all a caller sees
  core/  cpu/  platform/  accel/
translator/        PowerPC → C++, run once per game
src/
  core/            types, typed guest access, the host interface, the registry
  cpu/             the guest CPU runtime: registers, memory, calls, threads
  platform/        the console itself - required by every game
    os  fs  gx  audio  input  system
  accel/           native versions of libraries games link - optional, for speed
    sdk  nw4r  egg  jsystem  rfl
  app/             the program: main loop, settings, the in-game overlay
data/              builds seen in games, and native signatures (hashes, never code)
docs/              how things work: builds, signatures, writing a native
tools/             discs, NANDs, DOLs, builds and signatures (see tools/README.md)
```

Every part is its own static library with its own README. Dependencies point one
way: `app` → `accel` → `platform` → `cpu` → `core`. Underneath, libwii-nx builds on
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
wiinx-scan game.dol  →  bindings  →  translator  →  direct native calls
```

`wiinx-scan` reads the library banners and matches each function's code
signature, then writes the game's `bindings.json`: which native, of which build,
answers each address. The translator bakes that in, so a bound call costs
nothing at runtime. A function nothing matches stays the game's own translated
code: an unknown build is slower, never wrong.

## Building a game

One command:

```sh
tools/wiinx-build mygame.iso        # or a game folder made earlier
```

extracts the disc, writes the project, scans the game for the natives libwii-nx
can bind, translates its code, builds it and leaves `<game>/build/<game>.nro`
for `sdmc:/wii-nx/games/<game>/`. It needs dawn-nx and aurora-nx checkouts
(`--dawn`, `--aurora`) and devkitPro's Switch toolchain.

Games are only ever built on the player's machine from their own disc; CI
builds the library and tools, never a game.

## Status

| Part                              | State                                              |
|-----------------------------------|----------------------------------------------------|
| [`core`](src/core/README.md)      | done: types, typed access, host, builds, registry  |
| [`platform`](src/platform/README.md) | ported: os, fs, gx, audio, input, net, system |
| [`accel/sdk`](src/accel/sdk/README.md) | THP decoder ported                             |
| [`accel/nw4r`](src/accel/nw4r/README.md) | lyt `Pane::CalculateMtx` ported, 2007 and 2008 builds |
| `accel/nw4r` g3d CalcWorld/CalcView | located in Mario Kart Wii, not written           |
| [`wiinx-scan`](tools/README.md)   | builds from banners (224 known) and binding by signature; call following next |
| [tools](tools/README.md)          | disc, DOL, NAND and game-project tools, all here |
| [translator](translator/README.md) | ported: same output as the original, byte for byte |
| `cpu`, `platform`, `app`          | ported: Mario Kart Wii builds from libwii-nx, layers checked by `wiinx-check-layers` |
| `tools/wiinx-build`               | written (disc → NRO through `cmake/game`); not yet run end to end |

## Working on it

[docs/](docs/README.md) explains how builds, signatures and natives work, and
the rules every part follows.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
cmake --build build
```

## License

GPL-3.0-or-later. The translator and runtime are built on
[WiiCompiled](https://github.com/patchzyy/wiicompiled) by patchzyy and contributors.
Behavior and layouts follow the decompilation projects doldecomp/ogws,
doldecomp/mkw and projectPiki/pikmin2 (all CC0); the THP IDCT is based in part
on the work of the Independent JPEG Group. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No game code or data is
included or distributed.
