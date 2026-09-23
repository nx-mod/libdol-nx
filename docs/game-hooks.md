# Game hooks

libwii-nx knows no game. It never names a game's function, address or quirk:
the same library is what every disc is built against. The few places where one
game needs something of its own from the runtime are hooks it fills in, from
its own native code in its project's `native/` folder.

`src/cpu/include/game_hooks.h`:

| Hook | Called when | For |
|---|---|---|
| `surface_resized(w, h)` | at each frame boundary, and at startup, with the current surface size | a game that scales its own canvas to the display |
| `viewport_about_to_change()` | the guest is about to set a viewport | marking screens that render to fixed-size offscreen targets |
| `calling(target, cpu)` | a call is about to be made, arguments in `cpu` | adjusting one call's arguments - how a runtime graphics setting reaches a game's own renderer |

A game installs them once at startup, from a static object in its own
translation unit:

```cpp
RuntimeGameHooks::Hooks hooks;
hooks.surface_resized = &OnSurfaceResized;
hooks.calling = &OnCalling;   // compare target, return; runs on every call
RuntimeGameHooks::install(hooks);
```

Nothing is required. A game that installs nothing behaves as the Wii does: the
surface is whatever the display gives, viewports are the guest's, and calls are
made with the guest's own arguments.

`calling` is on the hot path - every indirect call reaches it. It must do no
more than compare an address and return.

## The guest OS layout

The Wii's OS is linked into each game, so the scheduler's own variables - the
run queue, the reschedule flag, the thread it idles on, the alarm queue - sit
wherever that game's link put them. The console's lowmem globals (`0x800000xx`)
are the hardware's and are the same everywhere; these are not.

A game installs them the same way, from `src/cpu/include/guest_os_layout.h`:

```cpp
RuntimeGuestOs::Layout layout;
layout.run_queue = 0x803477B0u;
layout.run_queue_bits = 0x80386920u;
// ...
RuntimeGuestOs::install(layout);
```

The runtime says so at startup when a game installed none, and the platform
natives that walk those structures then have nothing to walk: a game needs its
layout before its threads run.

Most of it is found in the game's own code rather than by hand:

```sh
tools/wiinx-scan os-globals disc/sys/main.dol
```

prints the layout to install. It works from the shape of `SelectThread`, which
every RVL OS build has and nothing else looks like - the run-queue mask read
from small data, `cntlzw` to get the highest waiting priority, the queue array
indexed by that priority times eight - and from the order `OSThread.c` puts its
variables in memory: the thread the game booted on, the 32 queues, the idle
thread. Every address it reports is one the game's own code builds somewhere.

On Mario Kart Wii it reproduces all six known values exactly, which is the
check that it is reading the game and not guessing. The rest of the layout -
the switch-thread callback and interrupt table slots, the alarm queue's r13
offset, `OSLoadContext` - is still found by hand, and follows from the
signature milestone ([signatures](signatures.md)).

## Notes

- 2026-09-23: `wiinx-scan os-globals` added; New Super Mario Bros. Wii's layout
  came out of it, and is installed by `nsmbwii-nx/native/nsmbwii_game.cpp`.

- 2026-09-22: the guest OS layout followed the hooks out of the library:
  `os_internal.h` and `fiber_manager.cpp` read the installed layout through
  references, so their call sites are unchanged.
- 2026-09-22: added, replacing the Mario Kart Wii functions and addresses the
  runtime called directly (its dynamic aspect handling and one renderer-path
  argument at `0x8023BD38`). Those now live in `src/app/mkwii_game.cpp`
  (was `dynamic_aspect.cpp`), with the game's own records in
  `src/app/mkwii_dynamic_aspect_records.h`; both move to the Mario Kart Wii
  project's `native/` folder at the cut-over, unchanged.
