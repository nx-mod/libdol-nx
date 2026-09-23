# format

The consoles' own file formats, as bytes.

Nothing here knows about a host, a runtime or guest memory: each module takes a
buffer and gives back what it means, or takes a structure and gives back the
bytes a console would accept. That is what lets the same code serve three
callers - the runtime while a game runs, the tools on a PC, and a launcher
listing what is installed.

| Module | Holds |
|---|---|
| [`nand`](nand/README.md) | the Wii's internal memory: settings, Miis, paths and title ids, tickets, TMDs, WADs |
| [`disc`](disc/README.md) | disc images and the containers dumps come in |
| [`archive`](archive/README.md) | what games pack their files in: U8, Yaz0 |
| [`media`](media/README.md) | THP video and the sound that goes with it |

## Rules

- **Buffers in, buffers out.** A caller reads and writes files; these say what
  the bytes mean.
- **Malformed input is an answer, not a crash.** Every parser returns nothing
  rather than trusting a length it read.
- **No game data.** Formats are described here; the files themselves belong to
  whoever owns the disc.
