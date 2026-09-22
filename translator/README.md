# translator

Turns a game's PowerPC code into C++. It parses the DOL (and a REL when the
game has one), decodes every reachable function, lifts it through IR and SSA,
and emits C++ that is compiled for Switch together with libwii-nx's CPU runtime.
Everything game-specific - paths, addresses, the small-data bases - comes from
the game's `recomp.yml`; the translator holds no game data.

> Translating a DOL does not exempt you from owning the game it came from.

## Translating a game

Four steps, in this order, each reading the game's `recomp.yml`:

| Step | Does |
|---|---|
| `translate-recursive <entry> --project recomp.yml` | walks the call graph from the entry point and translates every function reached (a `function_map` seeds more boundaries) |
| `emit-base-manifest --project recomp.yml` | the table of translated functions |
| `generate-data-init --project recomp.yml` | the game's `.data` / `.rodata` / `.sdata` as initialisers, and `RuntimeConfig.h` |
| `emit-build-shards --project recomp.yml` | the C++ grouped into build units (`shards.cmake`), with natives bound |

Mario Kart Wii translates in about two minutes (29,063 functions, 72 shards).
Unsupported instructions fail translation unless the project allows them, and
such a build can never ship.

## Building it

```sh
dotnet build -c Release translator/src/Translator.Cli
dotnet translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll --help
```

Needs the .NET 8 SDK. Every command prints its options with `--help`.

## The project file

- `inputs.dol` - the DOL; a SHA-256 pins the revision.
- `memory.base` / `size`, `sda_base` / `sda2_base` - the guest address space and
  the r13/r2 small-data bases the game's boot code installs
  (`tools/wiinx-inspect-dol --yaml` prints them).
- `translation.entry_points`, `translation.function_map` - where discovery starts.
- `runtime.native_registration_root` - the natives translated calls bind to.

Relative paths resolve from `workspace_root`, which resolves from the file's
own folder: for a game project, that is the game's folder (`workspace_root: .`).

## Test

```sh
dotnet test -c Release translator/Translator.sln
```

## Notes

- 2026-09-22: ported. Re-translating Mario Kart Wii with this copy gives output
  identical to the original translator's, byte for byte (every shard, the data
  initialiser, the metadata).
- The payload check for one Mario Kart Wii mod was removed: it needed a
  third-party binary and a launcher library. `translate-mod` and the Kamek/Pulsar
  parsers are Mario Kart Wii mod support too; they move to that game's project.
- `runtime.native_registration_root` still names the natives as source files to
  scan; it will name libwii-nx's native tables once the runtime is ported.
