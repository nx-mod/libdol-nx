# Third-Party Notices

libwii-nx is GPL-3.0-or-later. What it is built from or follows:

## Code this library comes from

- **WiiCompiled** by patchzyy and contributors - GPL-3.0. The translator
  (`translator/`) is its static recompiler, and the CPU runtime and platform
  layer being ported in are built on its runtime.
  <https://github.com/patchzyy/wiicompiled>
- **wiicompiled-nx** - GPL-3.0. WiiCompiled's Switch port, which the translator,
  the first natives and the platform layer come here from.
  <https://github.com/nx-mod/wiicompiled-nx>
- **wii-nx** - GPL-3.0. `tools/wiinx-extract` and `tools/wiicrypto.py` are
  copies of its `example-wii-nx/scripts/extract-dol` and `wiicrypto.py`; the
  signature masking follows its `resolve-symbols`. <https://github.com/nx-mod/wii-nx>

## Symbols these signatures were taken from

A signature is a hash of masked instructions and carries no code. The names
beside them come from work other people did:

- **doldecomp/mkw** - CC0-1.0. Mario Kart Wii's symbol map, which the natives
  were first signed from. <https://github.com/doldecomp/mkw>
- **doldecomp/ogws** - CC0-1.0. Wii Sports' symbol map, and the reason a second
  build of the SDK could be signed. <https://github.com/doldecomp/ogws>
- **RootCubed/NSMBW-Maps** - no license given, so nothing of it is kept here.
  New Super Mario Bros. Wii's maps are fetched by that game's own project when
  its owner wants them, and the signatures taken with their help carry the
  library's own names. <https://github.com/RootCubed/NSMBW-Maps>

## Bundled in this repository

Vendored with the runtime, each under its own license, in `third_party/`:

- **Crypto++ 8.9.0** - Boost Software License 1.0 / public domain. Copyright (c)
  1995-2019 Wei Dai and contributors. <https://github.com/weidai11/cryptopp>
- **pugixml** - MIT. Copyright (c) 2006-2025 Arseny Kapoulkine.
  <https://github.com/zeux/pugixml>
- **toml11 4.4.0** - MIT. Copyright (c) 2017 Toru Niina.
  <https://github.com/ToruNiina/toml11>
- **libco** - ISC (`valgrind.h`: BSD-style). Copyright byuu and the higan team.
  <https://github.com/higan-emu/libco>

## Data files

- **Dolphin Emulator** - GPL-2.0-or-later. Copyright (c) 2003+ Dolphin Emulator
  Project. `data/dsp/dsp_coef.bin` (the free DSP resampling coefficients, hash
  checked at build time) and `data/wii/shared2/wc24/**` (the default
  WiiConnect24 tree a new NAND is seeded from) are Dolphin's `Data/Sys` files,
  unmodified. Neither holds Nintendo code or game assets.
  <https://github.com/dolphin-emu/dolphin>

## Reimplemented natively from other work

Natives here are written for this project; their behavior follows the sources
below. No code is copied verbatim unless stated.

- **libjpeg** - IJG License. This software is based in part on the work of the
  Independent JPEG Group. The THP decoder's IDCT is derived from libjpeg's float
  AAN IDCT (`jidctflt.c`), Copyright (C) 1994-1998, Thomas G. Lane. <https://ijg.org>
- **projectPiki/pikmin2** - CC0-1.0. The SDK THP decoder (`THPDec.c`).
  <https://github.com/projectPiki/pikmin2>
- **doldecomp/ogws** (Wii Sports) - CC0-1.0. nw4r behavior and 2007 layouts.
  <https://github.com/doldecomp/ogws>
- **doldecomp/mkw** (Mario Kart Wii) - CC0-1.0. Function names for 2008 builds.
  <https://github.com/doldecomp/mkw>

## Design reference

- **Source SDK 2013** (`public/tier1/interface.h`) - versioned interfaces and
  their static-linking pitfalls informed the registry. Pattern only; no code.
  <https://github.com/ValveSoftware/source-sdk-2013>

## Rules for new sources

- Take code only from explicitly licensed projects; treat the rest as reference.
- libogc's threading and kernel code derives from RTEMS: anything taken from
  those parts credits RTEMS and carries its license alongside libogc's.
