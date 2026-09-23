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
| CPU runtime: CpuContext, guest memory, dispatch, fibers | wiicompiled-nx `runtime/` | `src/cpu` | copied; building as one runtime library, split next |
| platform: os, fs, gx, audio, input, system | wiicompiled-nx `runtime/src/hle` | `src/platform/*` | copied; building as one runtime library, split next |
| NAND formats: settings, Miis, saves, archives | wii-nx `wiinand-nx/lib` | `src/platform/fs` | to copy |
| app shell: main loop, config, settings overlay | wiicompiled-nx `runtime/src` | `src/app` | copied; building as one runtime library, split next |
| game tools: new-game, translate, audit, manual-adds, make-bindings, profile-report | wii-nx `example-wii-nx/scripts` | `tools/wiinx-*` | ported, pointed at libwii-nx's own translator and runtime |
| port notes, pitfalls | wiicompiled-nx `docs/switch-port-notes.md` | `docs/` | with the runtime |
| Aurora changes (threaded decode, pipeline memo) | wiicompiled-nx `aurora-main/` | aurora-nx | offered to aurora-nx |

## Runtime file map

wiicompiled-nx's `runtime/` holds the CPU core, the console, the app and Mario
Kart Wii's own code in one tree and one CMake project. Each file's destination:

| Goes to | From `runtime/` |
|---|---|
| `src/cpu` | `abi_bridge`, `fiber_manager`, `guest_flat_memory` (+ macOS), `guest_interrupt_context`, `host_context`, `host_cpu_baseline`, `memory`, `memory_access`, `native_bindings`, `native_cpu_calls.inc`, `ppc_helpers`, `fpu_helpers`, `ppc_runtime`, `ppc_isa_memory`, `include/isa/*`, `timebase_contract`, `mkw_thread_local`, `mkw_visibility`, `src/platform/*/co_switch.S`; `third_party/libco` |
| `src/platform/os` | `hle/os/*`, `hle/task_thread`, `hle/trk`, `hle/c_stdio`, `guest_printf` |
| `src/platform/fs` | `hle/storage/*` (DVD, NAND, ISFS, Riivolution), `hle/ios`, `hle/esp`, `wii_es_crypto`, `nand_*`, `console_identity`, `console_region`; `third_party/cryptopp` |
| `src/platform/gx` | `hle/gx/*`, `gx_guest_write` |
| `src/platform/audio` | `hle/audio/*`, `audio_backend` |
| `src/platform/input` | `hle/input/*`, `wii_remote_input`, `input_bindings`, `input_expr`, `controller_status_contract`, `controller_button_names` |
| `src/platform/system` | `hle/vi`, `hle/sc`, `sc_serial_contract`, `system_bridge`, `hle/net/*` (network, until it earns a module) |
| `src/accel/egg` | `hle/egg_decomp` |
| `src/app` | `main`, `settings_overlay`, `controller_mapping_wizard`, `discord_presence`, `music_attenuation`, `runtime_config`, `runtime_log`, `runtime_product`, `product/base_product`, `host_platform`, `platform_switch/*`, `switch_layout`, `switch_account_identity`, `aurora_events`; `third_party/toml11`, `third_party/pugixml` |
| data | `assets/dsp/dsp_coef.bin`, `assets/wii/shared2/wc24/**` - Dolphin's free data files, GPL-2.0-or-later, credited |
| the MKW game project | `dynamic_aspect`, `game_graphics_options`, `product/retro_rewind_product`, `recomp_mod_loader`, `assets/pipeline/initial_pipeline_cache.db` (Mario Kart Wii's own shader pipelines) |

Copied into those places and first built as one runtime library with today's
sources and flags, so Mario Kart Wii keeps building at every step; then split
into `cpu`, `platform` and `app` libraries, fixing upward dependencies as they
show up.

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
- 2026-09-22: runtime copied into `src/cpu`, `src/platform/{os,fs,gx,audio,input,net,system}`,
  `src/accel/egg` and `src/app` by `wiicompiled/tools/local/port_runtime.py`,
  include paths rewritten (`hle/gx/` -> `gx/` and so on). Headers stay
  bare-named in one `include/` folder per layer, since translated game code
  includes them by name. The runtime binds THP and the layout pane through
  `*_bind.cpp` glue over the accel natives, with the runtime's names and
  signatures, so translation output is unchanged; `app/wiinx_host.cpp` hands
  natives the guest memory and CPU. Build: `cmake/runtime/Runtime.cmake`, the
  upstream CMake with paths moved; the MKW pipeline cache and Retro Rewind
  product are the game's and only used when the game supplies them.
- 2026-09-22: translator ported. Its CLI no longer references the launcher
  (the one command that did, a mod payload check, was dropped with its test
  and a third-party payload binary). Mario Kart Wii re-translated with it is
  byte-identical to the original's output.
- 2026-09-22: the runtime no longer names a game. The Mario Kart Wii functions
  and addresses `abi_bridge.h`, `aurora_events.h`, `gx_transform.cpp`,
  `main.cpp` and `game_graphics_options.h` called directly became
  `RuntimeGameHooks` (`src/cpu/include/game_hooks.h`, see
  [game hooks](game-hooks.md)); the game's side sits in
  `src/app/dynamic_aspect.cpp` with its records in
  `src/app/mkwii_dynamic_aspect_records.h`, ready to move to the game's project
  as they are. `tools/wiinx-check-layers` reports no violations.
- 2026-09-22: the guest OS globals left the library too. `os_internal.h`,
  `fiber_manager.cpp`, `network_deferred.cpp` and `abi_bridge.h` read
  `RuntimeGuestOs::layout()`, which the game installs; Mario Kart Wii's are in
  `src/app/mkwii_game.cpp` (renamed from `dynamic_aspect.cpp`). What is left of
  one game in the library is the ~700 `PPC_NATIVE_OVERRIDE` addresses the
  platform natives are registered at, which the signature milestone replaces.
