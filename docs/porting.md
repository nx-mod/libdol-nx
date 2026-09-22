# Porting into libwii-nx

libwii-nx is the toolkit: everything it needs to turn a Wii disc into a Switch
game lives here, and other projects use libwii-nx instead of keeping their own
copy. wii-nx ends up as the games and one build that consumes it.

Until libwii-nx builds Mario Kart Wii on its own, pieces come in as **copies**:
the originals keep running the game unchanged, and are retired only after the
cut-over. Graphics backends stay separate libraries underneath (aurora-nx,
dawn-nx, nxvk, sqlite-nx), since each is general-purpose and has its own
upstream.

## Inventory

| Piece | From | Goes to | State |
|---|---|---|---|
| core: types, host, typed guest access, builds, registry | new | `src/core` | done |
| THP decoder | wiicompiled-nx `runtime/src/hle/thp_decode.cpp` | `src/accel/sdk` | ported |
| lyt `Pane::CalculateMtx` | wiicompiled-nx `runtime/src/hle/nw4r` | `src/accel/nw4r` | ported, 2007 + 2008 |
| builds, signatures, binding | new (masking from wii-nx `resolve-symbols`) | `tools/wiinx-scan`, `data/` | done; call following next |
| disc tools: extract, extract-disc, inspect-dol, unpack-u8 | wii-nx `example-wii-nx/scripts` | `tools/wiinx-*` | copied |
| NAND tools: fetch-title, fetch-nand, sysconf, wiinand/wiicrypto | wii-nx `example-wii-nx/scripts` | `tools/` | copied |
| translator (PowerPC → C++) | wiicompiled-nx `translator/` | `translator/` | ported; output identical to the original's |
| Mario Kart Wii mod support: `translate-mod`, Kamek/Pulsar, Retro WFC | wiicompiled-nx `translator/` | the MKW game project | to split out of `translator/` |
| CPU runtime: CpuContext, guest memory, dispatch, fibers | wiicompiled-nx `runtime/` | `src/cpu` | to copy |
| platform: os, fs, gx, audio, input, system | wiicompiled-nx `runtime/src/hle` | `src/platform/*` | to copy |
| NAND formats: settings, Miis, saves, archives | wii-nx `wiinand-nx/lib` | `src/platform/fs` | to copy |
| app shell: main loop, config, settings overlay | wiicompiled-nx `runtime/src` | `src/app` | to copy |
| game tools: new-game, translate, audit, manual-adds, make-bindings, profile-report | wii-nx `example-wii-nx/scripts` | `tools/wiinx-*` | with the translator and runtime, pointed at them |
| port notes, pitfalls | wiicompiled-nx `docs/switch-port-notes.md` | `docs/` | with the runtime |
| Aurora changes (threaded decode, pipeline memo) | wiicompiled-nx `aurora-main/` | aurora-nx | offered to aurora-nx |

## Order

1. Translator and CPU runtime - nothing builds without them.
2. Platform, module by module: fs, input, audio, system, os, gx (last: most
   tied to Aurora).
3. App shell, then the game tools, pointed at libwii-nx's own translator and
   runtime.
4. Build Mario Kart Wii from libwii-nx alone, compare with the working build,
   then cut wii-nx over and retire the copies.

## Notes

- 2026-09-22: inventory written; disc and NAND tools copied; the game tools
  wait for the translator and runtime, which they are wired to, so they are
  ported once.
- 2026-09-22: translator ported. Its CLI no longer references the launcher
  (the one command that did, a mod payload check, was dropped with its test
  and a third-party payload binary). Mario Kart Wii re-translated with it is
  byte-identical to the original's output.
