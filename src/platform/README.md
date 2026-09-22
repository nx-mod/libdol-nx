# platform

The console itself: what a game's SDK reaches when it touches hardware or the
OS. Required by every game; depends only on `core`.

| Module   | Does                                                        | Today, in wiicompiled-nx `runtime/src/hle/` |
|----------|-------------------------------------------------------------|---------------------------------------------|
| `os`     | threads, alarms, interrupts, time, caches, mutexes          | `os/`                                       |
| `fs`     | disc reads (DVD), NAND saves, IOS/ES file access            | `storage/`, `ios.cpp`, `esp.cpp`            |
| `gx`     | the GX FIFO and display lists into Aurora, decoded off-core | `gx/`                                       |
| `audio`  | AI/DSP/AX mixing into Switch audren                         | `audio/`, `../audio_backend.cpp`            |
| `input`  | GameCube pad, Wii Remote (KPAD/WPAD) from Joy-Con           | `input/`                                    |
| `system` | VI timing, SC settings, IPC, STM power                      | `vi.cpp`, `sc.cpp`                          |

Each module moves here whole, keeps its behavior, and trades its hard-wired
Mario Kart Wii addresses for bindings by name.

## Notes

- Not moved yet. `fs` goes first (most self-contained), `gx` last (tied to
  Aurora and the threaded decode).
