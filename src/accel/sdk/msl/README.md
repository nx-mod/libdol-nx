# accel/sdk/msl

The C library the game's compiler linked: `memcpy`, `memmove`, `memset`,
`strlen`, `strcmp`.

These are called constantly by everything and are pure byte work, which is why
they pay for themselves. The semantics are the standard's rather than any one
build's, so they are registered under `kMsl_Any` and serve every game whatever
compiler wrote it.

They resolve the guest's memory once through the host and then work on it
directly; a range that is not readable makes the native decline rather than
guess, and the game's own code runs.
