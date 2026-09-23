# accel/sdk/msl

The C library the game's compiler linked: the memory and string functions every
game calls constantly.

| | |
|---|---|
| memory | `memcpy`, `memmove`, `memset`, `memcmp` |
| strings | `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strchr`, `strrchr` |

These are called constantly by everything and are pure byte work, which is why
they pay for themselves. The semantics are the standard's rather than any one
build's, so they are registered under `kMsl_Any` and serve every game whatever
compiler wrote it.

They resolve the guest's memory once through the host and then work on it
directly; a range that is not readable is answered rather than faulted on, which
is what a game with a runaway pointer needs.

Two details the check pins down, because both are easy to get wrong: `memcpy`
moves rather than copies, so a game that overlaps its arguments gets what the
PowerPC loop gave it instead of whatever the host decides undefined behaviour
means today; and `strncpy` pads its destination with zeros and leaves it
unterminated when the source is longer, which is the standard's own oddity.
