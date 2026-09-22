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

## Notes

- 2026-09-22: ported. `dynamic_aspect.cpp` is Mario Kart Wii's own aspect
  handling and moves to that game's project.
