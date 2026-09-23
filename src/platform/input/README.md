# platform/input

Controllers, as the game expects them.

| File | Is |
|---|---|
| `pad.cpp` | the controller ports both consoles have |
| `wpad.cpp`, `kpad.cpp` | the Wii Remote: buttons, motion, its pointer, and extensions |
| `wii_remote_input.cpp` | turning a Switch controller's state into that |
| `input_bindings.cpp`, `input_expr.cpp` | which physical control answers which guest button, as an expression a player can change |

A game reads its own SDK's structures, so a Joy-Con ends up as a Wii Remote
rather than as a translation layer the game can notice.
