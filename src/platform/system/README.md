# platform/system

The console around the game: the display, its settings, and power.

| File | Is |
|---|---|
| `vi.cpp` | video timing - modes, fields, and when a frame is shown |
| `sc.cpp` | the settings a game reads: language, aspect, sound |
| `system_bridge.cpp` | the rest of the machine: power, reset, identity |

Timing here is what a game measures its own frame against, so it follows the
console's, not the host's.
