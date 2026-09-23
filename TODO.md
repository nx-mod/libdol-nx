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
- [ ] `accel/ogc` - natives for homebrew put through the translator
- [ ] `app` - one launcher, themed per console, rather than one per front repo

## Beyond this library

- [ ] **libogc-nx**: homebrew rebuilt from source, which is the only thing that
      can be handed out already built ([notes](docs/homebrew.md))
- [ ] **libgc-nx**: ARAM, memory cards, DTK and boot
- [ ] **libwii-nx**: IOS, ES, ISFS, NAND, WPAD/KPAD, SC

## Measured, not guessed

- [ ] Display-list decode, 12.6 ms a frame
- [ ] Audio, about 5 ms a frame
- [ ] Free memory: 3 MB of 3189 MB
