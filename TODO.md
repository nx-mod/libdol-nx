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
- [ ] `accel/nw4r` - ported whole from the decompilations (doldecomp/ogws, CC0),
      a library at a time: lyt animation done (2026-11-03, three builds); g3d
      calc and snd next - the two largest gaps in any game
      ([coverage](docs/coverage.md), [detail](src/accel/nw4r/TODO.md))
- [ ] `accel/sdk/gx` - GX natives written (texture objects, scissor); then TEV,
      vertex formats, transforms, lights and the pixel setters. The deko3d build
      switches off all ~120 Aurora-era GX natives (`#if !defined(WIINX_NATIVE_GX)`
      in platform/gx: they call Aurora's GX), so every GX setter runs translated
      and writes the FIFO a word at a time through the checked path - Bomberman
      Blast's Characters menu, 37,000 words a frame. Each one ported here writes
      the original's words in one burst; hottest first by profile
- [ ] Gather-pipe stores from translated code on a fast path: the vertex writes
      (GXPosition, GXColor, inlined into game code) no native can catch
- [ ] `accel/ogc` - natives for homebrew put through the translator
- [ ] `app` - one launcher, themed per console, rather than one per front repo

## Natives found in WiiCompiled forks (2026-11-02, all GPL-3.0; credit when taken)

Every branch of patchzyy/Wiicompiled and seven forks compared against our registrations. SDK functions every
game links, to port as universal natives (signed, tested, documented), not as one game's addresses:

- [ ] OS exits and resets - `OSReturnToMenu`, `OSReturnToMenuForError`, `OSReturnToDataManager`, `OSRestart`,
      `OSRebootSystem`, `OSShutdownSystem`, `OSHotReset`, `IOS_IoctlvReboot` (gourguesnicolas/NSMBW-Recompiled).
      The Mii Channel's exit hang is this path
- [ ] `OSReadROM`, `OSGetFontEncode`, `OSSetProgressiveMode`, `EXILock`, `GXAbortFrame`, `KPADReadEx`,
      `WUDIsReady`, `SCCheckStatus` (NSMBW-Recompiled)
- [ ] SC's Wii Remote settings - `SCGet/SetBtDeviceInfoArray`, `SCGet/SetBtCmpDevInfoArray`,
      `SCGetBtDpdSensibility`, `SCGet/SetWpadMotorMode`, `SCGet/SetWpadSpeakerVolume`,
      `SCGetWpadSensorBarPosition` (patchzyy, zydezu); `SCGetSimpleAddressData` (chrissotraidis)
- [ ] `DVDLowSeek` (patchzyy)
- [ ] `AXFXReverbHiExpCallback` (iChris4/Wiicompiled_VR): check ours (`ax_effects.cpp`) against it
- [ ] `Nw4rFrustumIntersectAabbEx` (iChris4): nw4r math, for accel/nw4r

## Beyond this library

- [ ] **libogc-nx**: homebrew rebuilt from source, which is the only thing that
      can be handed out already built ([notes](docs/homebrew.md))
- [ ] **libgc-nx**: ARAM, memory cards, DTK and boot
- [ ] **libwii-nx**: IOS, ES, ISFS, NAND, WPAD/KPAD, SC

## Speed, measured ([performance](docs/performance.md))

From KawaiiBunga/KartPadNX (GPL-3.0, Mario Kart Wii on Switch; credit when taken), 2026-11-02:

- [ ] A flat guest memory on the Switch: it reserves 4 GiB of address space with libnx virtmem and backs
      MEM1/MEM2 with shared-memory objects in 4 MiB slices mapped at every mirror (one large object fails
      with KernelError_OutOfResource) - `switch/src/runtime/guest_flat_memory_switch.cpp`. Ours takes the
      checked path on every access, the ~15% of a race frame performance.md measured
- [ ] The guest's sound thread hosted off the game thread (`snd_host_thread`, `snd_host_ticks`,
      `snd_host_parallel` in its runtime's `os_host_thread.cpp`) - our first speed item below
- [ ] Display-list caches (kartpad's: class cache, front cache, layout hashes, VAT row mask, prefetch)
      and native matrix/vector paths (`native_matrix.cpp`, `native_vector.cpp`)
- [ ] Instrumented PGO training for the translated shards, and its translator optimizations
- [ ] Thread placement and priorities per role (`switch/src/perf_switch.cpp`), its AArch64 fiber

- [ ] The guest's audio callback onto core 1 (~13 ms of a race frame)
- [ ] Vertex decode on the GPU, so the render thread stops being the wait. Bomberman Blast (2026-11-03):
      every menu but Characters runs at 60 with ~580 GX draws a frame; Characters (~17 fps) and battles make
      5,900-8,200, small strips merged into ~250 GPU draws, and the game thread then waits in `GXDrawDone`
      (25%). The cost is per GX draw, not per GPU draw. `gx-native: time:` (once a second) says how it splits
      between the command processor, GPU draws, the game's waits, FIFO bytes and display lists
- [ ] g3d and AXFX natives
- [ ] Async disc reads back on (a hang between menus is not found)

## Native GX ([deko3d](docs/deko3d.md))

- [ ] Miis: face texture eyes and mouth, heads, icons
- [ ] The race minimap
- [ ] Bump-alpha colour channels
- [ ] The settings overlay on deko3d
- [ ] Mega Man 9: a pixel row lost here and there in its scaled 256x240 picture
