# app

The program around the game: startup, the frame loop, settings, the on-screen
overlay, and the Switch shims. It is the only layer allowed to know every other
one, and the place where the parts are wired together.

| Piece | Is |
|---|---|
| `main.cpp` | startup: memory, the renderer, the guest's boot, the frame loop, the profiler |
| `wiinx_host.cpp` | hands natives the guest memory and CPU (`wiinx::set_host`) and adds the native modules |
| `settings_overlay`, `controller_mapping_wizard` | the in-game settings screen and input setup |
| `music_attenuation`, `discord_presence` | host niceties: ducking the console's music, presence |
| `switch/` | Switch shims: SDL stand-ins, thread stacks and core placement, the SD-card layout |
| `product/base_product.cpp` | the executable's entry point |
| `mkwii_game.cpp` | Mario Kart Wii's own: its aspect handling and its OS layout, installed through the [game hooks](../../docs/game-hooks.md) |

## Notes

- 2026-09-22: ported. `mkwii_game.cpp` is Mario Kart Wii's own, and moves to
  that game's `native/` folder at the cut-over.
- 2026-09-22: nothing outside `mkwii_game.cpp` names a game. It installs
  `RuntimeGameHooks` at startup - the surface size and the one renderer-path
  argument the runtime used to reach into Mario Kart Wii for, and the game's OS
  globals - and keeps the game's records in `mkwii_dynamic_aspect_records.h`.
  Moving both files to the game's project is all the cut-over needs.
