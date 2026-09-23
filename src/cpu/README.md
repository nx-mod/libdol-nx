# cpu

The guest CPU the translated game code runs on: its registers and state, the
guest's memory, calls between translated functions and natives, and the fibers
the game's threads become. Everything else builds on this; it depends only on
`core`.

| Piece | Is |
|---|---|
| `abi_bridge`, `native_bindings` | calls between translated code and natives, and which native answers an address |
| `memory`, `guest_flat_memory` | the guest's address space as one flat mapping, with the traps that catch MMIO and protected pages |
| `host_context`, `fiber_manager`, `platform/*/co_switch.S` | guest threads as fibers, and the context switch itself |
| `ppc_helpers`, `fpu_helpers`, `include/isa/*` | PowerPC semantics the generated code calls: flags, paired singles, quantised loads |
| `recomp_mod_loader` | where execution is, for the profiler and for mods |
| `include/game_hooks.h`, `include/guest_os_layout.h` | the few [hooks](../../docs/game-hooks.md) a game fills in, and where its OS globals live |
| `platform/host_platform`, `switch_layout`, `runtime_config`, `runtime_log` | host paths, the SD-card layout, settings and logging |

Headers live in `include/`, flat: translated game code includes them by bare
name (`ppc_runtime.h`, `memory.h`), and the translator fixes those names.

## Notes

- 2026-09-22: ported. `ppc_helpers.cpp` and `fpu_helpers.cpp` keep the exact
  float rounding the translated code does - no fast math, no contraction - and
  are built outside the unity groups so that cannot be lost.
- `game_graphics_options.h` and `recomp_mod_loader` are here because translated
  code and the call bridge reach them; the Mario Kart Wii-specific parts of
  both move to that game's project.
- 2026-09-22: `abi_bridge.h` no longer knows any game. It calls
  `RuntimeGameHooks::calling`, which is empty unless a game installed one, so
  a call costs a null check.
