# libdol-nx

The machine the GameCube and the Wii both are, as a library, for statically
recompiled games running natively on Nintendo Switch.

It is the whole toolkit: the translator that turns a game's PowerPC code into
C++, the CPU runtime that code runs on, the console it expects, native versions
of the libraries it links, the Wii's and the GameCube's file formats, and the
tools for discs, NANDs and builds. A game project needs this and the library for
its console, and nothing else. With it, a game is built from the player's own
disc:

```
your disc  →  translator  →  game code + runtime  →  NRO
```

The translated code calls in at the same function boundaries the game called
Nintendo's SDK at, so a fix here reaches every game that uses what was fixed.

Two consoles, one library, because they are one machine: the CPU core is the
same, the graphics pipeline is the same, the sound hardware is the same, and of
the libraries Nintendo shipped in the two SDKs, sixteen appear in both under the
same names. What differs is peripherals and system software, and that lives in
[libwii-nx](https://github.com/nx-mod/libwii-nx) and
[libgc-nx](https://github.com/nx-mod/libgc-nx).

## Design

Six sections. Each is a folder of public headers under `include/wiinx/`, a
folder of code under `src/`, a static library, and a README beside it.

```
include/wiinx/     public headers - all a caller sees
  core/  format/  cpu/  platform/  accel/  app/
src/
  core/       types, typed guest access, the host interface, the registry
  format/     the consoles' file formats, as bytes: disc  nand  archive  media
  cpu/        the guest CPU runtime: registers, memory, calls, threads
  platform/   the console itself - os  fs  gx  audio  input  system  net
  accel/      native versions of the libraries games link - sdk  nw4r  egg  jsystem  rfl  ogc
  app/        the program: main loop, settings, the in-game overlay
translator/   PowerPC → C++, run once per game
data/         builds seen in games, and native signatures (hashes, never code)
docs/         how things work: builds, signatures, writing a native, homebrew
tools/        discs, NANDs, DOLs, builds and signatures (see tools/README.md)
tests/        checks that run wherever the building happens (ctest)
```

Dependencies point one way: `app` → `accel` → `platform` → `cpu` → `core`, with
`format` beside them depending on nothing.

`core`, `format` and `accel` need only the standard library, so they build on
any machine and their checks run there. `cpu`, `platform` and `app` are the
running console and need Aurora and a game's translated code.

Underneath, this builds on [aurora-nx](https://github.com/nx-mod/aurora-nx)
(GX to WebGPU), [dawn-nx](https://github.com/nx-mod/dawn-nx) (WebGPU to Vulkan),
[nxvk](https://github.com/nx-mod/nxvk) (the Vulkan driver),
[sqlite-nx](https://github.com/nx-mod/sqlite-nx) and libnx. Those stay separate
libraries; a game project sees this one and its console's.

### format - the consoles' own files

Bytes in a buffer: no host, no runtime, no guest memory. The runtime uses it to
serve a game, the tools use it on a PC, and a launcher uses it to list what is
installed, all from one copy.

| Module    | Is                                                             |
|-----------|----------------------------------------------------------------|
| `disc`    | disc images and their containers, GameCube and Wii              |
| `nand`    | SYSCONF, the Mii database, NAND paths, tickets, TMDs, WADs      |
| `archive` | what games pack their files in: U8, Yaz0                        |
| `media`   | THP video, and the sound formats that go with it                |

### platform - the console

What a game's SDK talks to when it touches hardware. What is here is what both
consoles have; the peripherals only one of them has are in that console's own
library.

| Module   | Is                                                              |
|----------|-----------------------------------------------------------------|
| `os`     | threads, alarms, interrupts, time, caches, mutexes              |
| `fs`     | the disc (DVD), and saves through whichever store the console has |
| `gx`     | graphics: the GX FIFO into Aurora                               |
| `audio`  | AI/DSP/AX into Switch audio                                     |
| `input`  | the controller ports, and each console's own controllers        |
| `system` | video timing (VI), settings, power                              |
| `net`    | sockets and SSL, where a game has them                          |

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
| `ogc`     | libogc, for homebrew put through the translator             |

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
answers each address. Every game binds only what its own table names, including
the game the natives were read from, which writes those addresses as themselves
(`data/reference-bindings.json`). The translator bakes that in, so a bound call
costs nothing at runtime. A function nothing matches stays the game's own
translated code: an unknown build is slower, never wrong.

## What a game brings of its own

A game is its disc, its `bindings.json`, and - only if it needs one - its own
native code. libdol-nx names no game: where one needs something the console
does not do (scaling its canvas to the display, a renderer argument a setting
has to reach), it fills in a [hook](docs/game-hooks.md) at startup, and it says
there where its own copy of the Wii's OS keeps the scheduler's globals.

## Building a game

One command:

```sh
tools/wiinx-build mygame.iso        # or a game folder made earlier
```

extracts the disc, writes the project, scans the game for the natives libdol-nx
can bind, translates its code, builds it and leaves `<game>/build/<game>.nro`
for `sdmc:/wii-nx/games/<game>/`. It needs dawn-nx and aurora-nx checkouts
(`--dawn`, `--aurora`) and devkitPro's Switch toolchain.

Games are only ever built on the player's machine from their own disc; CI
builds the library and tools, never a game.

## Status

| Section | State |
|---|---|
| [`core`](src/core/README.md) | types, typed guest access, the host interface, the build table, the native registry |
| [`format`](src/format/README.md) | discs (raw, CISO, WBFS, RVZ, WIA), the NAND's files and WADs, Yaz0, THP |
| [`cpu`](src/cpu/README.md) | registers, memory, calls, threads, the guest OS layout a game installs |
| [`platform`](src/platform/README.md) | os, fs, gx, audio, input, net, system |
| [`accel/sdk`](src/accel/sdk/README.md) | the C library, the matrix library, the THP decoder |
| [`accel/nw4r`](src/accel/nw4r/README.md) | lyt `Pane::CalculateMtx`, two builds. g3d and snd are the largest gaps |
| [`accel/egg`](src/accel/egg/README.md) | Yaz0 |
| [translator](translator/README.md) | PowerPC → C++, one pass per game |
| [tools](tools/README.md) | discs, DOLs, NANDs, builds, signatures and game projects |

What each library covers and what the gaps cost is measured in
[docs/coverage.md](docs/coverage.md).

## Working on it

[docs/](docs/README.md) explains how builds, signatures and natives work, and
the rules every part follows.

## Building

The sections that need only the standard library build anywhere, and their
checks run there:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

The whole library, for a Switch:

```sh
cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
cmake --build build-switch
```

## License

GPL-3.0-or-later. The translator and runtime are built on
[WiiCompiled](https://github.com/patchzyy/wiicompiled) by patchzyy and contributors.
Behavior and layouts follow the decompilation projects doldecomp/ogws,
doldecomp/mkw and projectPiki/pikmin2 (all CC0); the THP IDCT is based in part
on the work of the Independent JPEG Group. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No game code or data is
included or distributed.
