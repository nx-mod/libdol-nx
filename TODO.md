# TODO

The large moves, newest work first. Each section's own TODO.md has its detail.

## Structure

- [ ] Move the internal headers under `wiinx/`, so translated code includes
      `<wiinx/cpu/runtime.hpp>` rather than `"ppc_runtime.h"`. Needs one
      retranslate, which is owed anyway.
- [ ] Split the namespaces at the same time: `dolnx::` here, `wiinx::` and
      `gcnx::` in the console libraries.
- [ ] Move the Wii-only modules to libwii-nx and write the GameCube's four in
      libgc-nx.

## Sections

- [x] `format/disc` - a dump is read where it lies: raw images, CISO, WBFS, RVZ
      and WIA, both consoles ([detail](src/format/disc/TODO.md))
- [ ] `platform/fs` - serve a game's files from a dump, so nothing is extracted
      at all
- [ ] `format/nand` - install a title, not only read one
      ([detail](src/format/nand/TODO.md))
- [ ] `format/archive` - U8, and a Yaz0 encoder
- [ ] `accel/nw4r` - g3d and snd are the two largest gaps in any game
      ([coverage](docs/coverage.md))
- [ ] `accel/sdk/gx` - GX natives written (texture objects); bind them with
      `wiinx-emit-bindings` and retranslate, then TEV, vertex formats,
      transforms, lights and the pixel setters
- [ ] `accel/ogc` - natives for homebrew put through the translator
- [ ] `app` - one launcher, themed per console, rather than one per front repo

## Beyond this library

- [ ] **libogc-nx**: homebrew rebuilt from source, which is the only thing that
      can be handed out already built ([notes](docs/homebrew.md))
- [ ] **libgc-nx**: ARAM, memory cards, DTK and boot
- [ ] **libwii-nx**: IOS, ES, ISFS, NAND, WPAD/KPAD, SC

## Speed, measured ([performance](docs/performance.md))

- [ ] The guest's audio callback onto core 1 (~13 ms of a race frame)
- [ ] Vertex decode on the GPU, so the render thread stops being the wait
- [ ] g3d and AXFX natives
- [ ] Async disc reads back on (a hang between menus is not found)

## Native GX ([deko3d](docs/deko3d.md))

- [ ] Miis: face texture eyes and mouth, heads, icons
- [ ] The race minimap
- [ ] Bump-alpha colour channels
- [ ] The settings overlay on deko3d
- [ ] Mega Man 9: a pixel row lost here and there in its scaled 256x240 picture
