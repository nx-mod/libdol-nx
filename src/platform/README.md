# platform

The console itself: what a game's SDK reaches when it touches hardware or the
OS. Required by every game; depends on `cpu` and `core`.

| Module   | Does                                                        | Ported from (see docs/porting.md) |
|----------|-------------------------------------------------------------|---------------------------------------------|
| `os`     | threads, alarms, interrupts, time, caches, mutexes          | `os/`                                       |
| `fs`     | disc reads (DVD), NAND saves, IOS/ES file access            | `storage/`, `ios.cpp`, `esp.cpp`            |
| `gx`     | the GX FIFO and display lists into Aurora, decoded off-core | `gx/`                                       |
| `audio`  | AI/DSP/AX mixing into Switch audren                         | `audio/`, `../audio_backend.cpp`            |
| `input`  | GameCube pad, Wii Remote (KPAD/WPAD) from Joy-Con           | `input/`                                    |
| `system` | VI timing, SC settings, IPC, STM power                      | `vi.cpp`, `sc.cpp`                          |

Each module came here whole and keeps its behavior.

## Notes

- 2026-09-22: ported, all seven modules, and building as `wiinx_platform`.
- 2026-09-22: the guest OS globals in `os/os_internal.h` are the game's now,
  read from the layout it installs ([game hooks](../../docs/game-hooks.md)).
- The natives are still registered at the addresses they were written from -
  Mario Kart Wii's - by `PPC_NATIVE_OVERRIDE(<address>, ...)`. Another game
  binds none of them and runs its own translated SDK code: correct, slower.
  Signing them so any game binds them by code is the next milestone
  ([signatures](../../docs/signatures.md)).
- 2026-09-23: `system` seeds low memory from the disc's own header rather than
  from one game's ID, and `fs`/`net` ask the product whether it overlays the
  disc instead of asking whether it is Retro Rewind.

## What is not here

The peripherals and system software only one console has live in that console's
own library, and are compiled in beside these by a runtime build:

- [libwii-nx](https://github.com/nx-mod/libwii-nx) - IOS and ES, the NAND as a
  game sees it, the Wii Remote, the console's settings
- [libgc-nx](https://github.com/nx-mod/libgc-nx) - ARAM, memory cards, disc
  streaming, the GameCube's boot path

A build says so when one of them is missing, rather than producing a runtime
that is quietly short of a console.
