# Running GameCube and Wii homebrew

Homebrew is the easiest thing this library can run, and the only thing it can
run that may also be given away: a homebrew program built here contains nothing
Nintendo wrote, so where its licence allows it, the result can be distributed
like any other Switch application.

There are two ways in, and which one an application takes depends on whether its
source exists.

## 1. Rebuild from source

Almost all homebrew is written against **libogc**, the homebrew SDK, and libogc
is zlib-licensed: it may be used, altered and redistributed, with its notices
kept. So the way in is not a reimplementation but a port, the same shape as
aurora-nx and dawn-nx - libogc's own headers, kept as they are, over a backend
that calls this library instead of the hardware.

That gives exact source compatibility, which is the whole point: an application
keeps its `#include <gccore.h>`, its `GX_Begin`, its `WPAD_ScanPads`, and only
its makefile changes - devkitPPC and `wii_rules` become devkitA64 and
`switch_rules`, and the output is an NRO instead of a DOL.

### What the backend has to answer

Everything in the left column already exists in this library; the port is
plumbing, not new console work.

| libogc | Answered by |
|---|---|
| `VIDEO_*`, the framebuffer and `VIDEO_WaitVSync` | platform/system (VI) and the app's present path |
| `GX_*` | platform/gx, through Aurora |
| `PAD_*`, `SI_*` | platform/input |
| `WPAD_*`, `KPAD_*`, wiiuse | platform/input, Wii side |
| `AUDIO_*`, `ASND_*`, `AESND_*` | platform/audio |
| `LWP_*`, `SYS_*` threads, alarms, semaphores | platform/os |
| `fatInitDefault`, `libfat` | the Switch filesystem directly - the SD card is already a filesystem |
| `net_*` | platform/net |
| `DI_*`, `ES_*`, `ISFS_*`, `IOS_*` | not answered, and not intended to be |

### Where source resists, in the order it bites

1. **Pointer width.** The source assumes a 32-bit machine. Anything that stores
   a pointer in a `u32`, casts addresses about, or relies on `sizeof(void*) == 4`
   has to be corrected. In a typical application this is a handful of lines; in
   one that manages its own memory it is more. This is the real cost of the
   source path and there is no way around it: aarch64 has no 32-bit pointer mode
   available here.
2. **Cache and address macros** - `MEM_K0_TO_K1`, `MEM_VIRTUAL_TO_PHYSICAL`,
   `DCFlushRange`, `SYS_AllocateFramebuffer`. They assume the Gekko's cached and
   uncached mirrors of the same memory, which do not exist on ARM. The backend
   absorbs nearly all of it, because the places homebrew uses them are the
   framebuffer and the GX FIFO, and both are ours.
3. **Inline PowerPC assembly** - `mfspr`, `__lwbrx`, an occasional paired-single
   trick. Rare outside libogc itself, and libogc's own are part of the port.
4. **Endianness** mostly takes care of itself: where an application hand-builds
   data for the hardware, that data is big-endian by definition and is read back
   by the same code that reads a game's.

### The one piece of real work

GX. The graphics layer reads a FIFO out of guest memory, because that is where a
translated game writes it. Homebrew built this way has no guest memory at all -
its FIFO is an ordinary host allocation. The decoder needs to accept a buffer
that is simply a buffer, with the address translation removed rather than
replaced. That is one seam in one module, and it is worth doing anyway: it is
also what lets the FIFO decoder be tested without a game.

## 2. Recompile the DOL

For homebrew whose source was never released, the executable goes through the
translator exactly as a game does, and none of the friction above applies -
pointer width and endianness are irrelevant when the PowerPC code is translated
as it stands.

This path is *easier* than a commercial game, for two reasons: the executables
are small, seconds rather than hours to translate; and libogc is open source
with known builds, so a signature scan identifies nearly every library function
instead of leaving it as translated code. Natives for libogc therefore belong in
`accel`, beside the ones for Nintendo's SDK, keyed by libogc version the same
way.

## What is deliberately not supported

Applications whose purpose is the physical machine: USB loaders, NAND dumpers,
IOS exploit tools, drive rippers. They want IOS and a disc drive, and neither
exists here. They are not a gap to be filled.

## Libraries this implies

- **libogc-nx** - the port described above. Its own library, because it is
  unlike everything else here: it is compiled for the host, links against no
  guest memory, and its headers are upstream's rather than ours.
- **accel/ogc** - natives for translated homebrew, inside the shared library
  with the rest of `accel`. Not a library of its own.
- The layers homebrew stacks on libogc - GRRLIB, libwiigui - are small and get
  ported one at a time, if and when something needs them. SDL applications are
  the cheapest of all, since the Switch already has a native SDL2.

## Credit

libogc is copyright Michael Wiedenbauer and Dave Murphy, zlib-licensed, and
parts of it derive from RTEMS. Any port keeps those notices in the files that
carry that code and records them in THIRD-PARTY-NOTICES.md.
