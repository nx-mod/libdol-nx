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
