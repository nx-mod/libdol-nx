# Third-Party Notices

libwii-nx is GPL-3.0-or-later. What it is built from or follows:

## Code this library comes from

- **wiicompiled-nx** - GPL-3.0. The platform layer and the first natives move
  here from its runtime. <https://github.com/nx-mod/wiicompiled-nx>

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
