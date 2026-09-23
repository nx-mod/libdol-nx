# Working with libwii-nx

How the parts work, for anyone adding to the library. Start with the one you
need.

| Topic | Read when |
|---|---|
| [Builds](builds.md) | a game links a library build libwii-nx has not seen, or behavior depends on a build |
| [Signatures and scanning](signatures.md) | binding a game, or a native is not found in a game |
| [Writing a native](natives.md) | adding a native, or a new build of one |
| [Game hooks](game-hooks.md) | a game needs something of its own from the runtime |
| [Performance](performance.md) | what a frame costs on hardware, and the settings that have been measured |
| [Coverage](coverage.md) | which SDK libraries the library has taken over, and what the gaps cost |
| [Tools](../tools/README.md) | what each tool in `tools/` does |

Each part of the library also has a README next to its code, with notes kept
as work lands: [core](../src/core/README.md), [platform](../src/platform/README.md),
[accel](../src/accel/README.md), [sdk](../src/accel/sdk/README.md),
[nw4r](../src/accel/nw4r/README.md).

## Rules that hold everywhere

- **No game code or data.** Not in the source, not in `data/`, not in tests.
  Signatures are hashes; layouts are offsets. Anything read from a disc stays on
  the machine of whoever owns the disc.
- **Natives give the original's results.** Code whose results the game keeps
  (physics, animation feeding collision, anything replayed by ghosts or sent
  online) must match the original bit for bit. Only code nothing reads back -
  layout matrices, video decode - may use plain host math.
- **Unknown means unbound.** When a build or a signature does not match, the
  game's own translated code runs. Slower, never wrong.
- **Credit sources** in the file header and `THIRD-PARTY-NOTICES.md`. Take
  code only from explicitly licensed projects.
