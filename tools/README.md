# tools

Helpers for working on libdol-nx and binding games to it. Python 3, no
dependencies beyond the standard library (openssl speeds up extraction when
installed).

| Tool | Does |
|---|---|
| `wiinx-scan` | lists a DOL's library builds; finds natives by signature; finds where a game's functions start, in its own code (`functions`); finds where its copy of the Wii's OS keeps the scheduler's globals (`os-globals`); signs functions; records builds and generates `include/wiinx/core/builds.hpp`. See [docs/signatures.md](../docs/signatures.md) and [docs/builds.md](../docs/builds.md) |
| `wiinx-near-map` | names a game's natives from a game of a nearby SDK build by masked-code similarity, for `wiinx-sign-natives --by-name` to sign and verify. See [docs/signatures.md](../docs/signatures.md) |
| `wiinx-extract` | pulls `main.dol`, or any file with `--file <path>`, out of a Wii `.iso` without extracting the disc: decrypts only the clusters it needs |
| `wiinx-extract-disc` | the whole disc into a game project's `disc/`, through this library's own reader; Dolphin's `dolphin-tool` only as a fallback |
| `wiinx-disc` | the reader itself (built from `tools/src/`): `info`, `list`, `dol`, `file`, `extract`. Reads raw images, CISO, WBFS, RVZ and WIA, GameCube and Wii |
| `wiinx-inspect-dol` | a DOL's entry point, sections, BSS and small-data bases; `--yaml` for a project file |
| `wiinx-install-title` | installs a title into a NAND the way a console stores one: ticket, TMD, contents decrypted, shared contents in `/shared1` with the map that names them. Takes a WAD you own or a title downloaded by `wiinx-fetch-title` |
| `wiinx-wad` | looks inside a WAD - which title it is, what it wants, what it carries - and unpacks one, decrypting each content and writing the executable as `main.dol`. Reads both kinds, installable and boot2 |
| `wiinx-unpack-u8` | lists or extracts a U8 archive (how channels pack their files), flagging compression inside |
| `wiinx-fetch-title` | downloads one Wii system title from Nintendo's update servers and unpacks its executable |
| `wiinx-fetch-nand` | a reference set of system titles into `nand/` (not tracked), from a menu or by flag |
| `wiinx-sysconf` | reads and edits a Wii SYSCONF, the console's settings |
| `wiicrypto.py`, `wiinand.py` | the Wii's AES, keys and title download, shared by the tools above |
| `wiinx-new-title` | the same for a title you own as a WAD: unpacks it, expands the content its loader would have, and reads the project out of the executable |
| `wiinx-new-game` | a whole game project from your own disc - an image, or one already extracted: `game.toml`, `recomp.yml` pointing back at this checkout, README, `.gitignore` |
| `wiinx-read-boot` | what a disc says it is (ID, revision, region, title), read from its `sys/boot.bin` |
| `wiinx-translate` | the translator's four steps over a game project, with a memory guard |
| `wiinx-audit` | how universal the natives are: SDK, middleware, or one game's |
| `wiinx-manual-adds` | what a game still needs by hand, and `--template` to start one |
| `wiinx-make-bindings` | resolved addresses as the C++ table a game's build links |
| `wiinx-profile-report` | a device log's profile, with the game's function names |
| `wiinx-check-console` | compiles a console library's sources against this one, with devkitA64 and nothing else: what catches a broken include before someone spends an hour building a game |
| `wiinx-check-layers` | that the layers only depend downward (app → accel → platform → cpu → core) |
| `wiinx-build` | disc (or game folder) to `.nro`: scan, translate, build, package |
| `wiinx-emit-bindings` | a game's `bindings.json` from its scan, so no binding is written by hand |
| `wiinx-reference-bindings` | the master table: where every native was first identified; `--game` writes the reference game's `bindings.json` from it, checked against that game's map |
| `wiinx-check-bindings` | natives bound to the wrong function, found by comparing titles |
| `wiinx-coverage` | what the library covers of a game's SDK, and what it does not ([docs/coverage.md](../docs/coverage.md)) |
| `wiinx-find-globals` | where a game keeps the SDK's variables, read from its own code |
| `wiinx-make-globals` | a game's `globals.json` as the table its build links |
| `wiinx-identity` | which title a project is, read from the title itself |
| `wiinx-name` | names a game's functions by matching its code against games whose symbols are known |
| `wiinx-suggest-names` | names a game's unknown functions from a game that names them |
| `wiinx-sign-map`, `wiinx-sign-elf` | code signatures from a symbol map, or from an executable that carries its own symbols |
| `wiinx-rso` | a title's RSO modules: code it loads and links at run time |
| `wiinx-title-program` | the program a system title boots, written as a DOL the toolkit reads |
| `wiinx-nand-dump` | files out of a console's own NAND backup (BootMii's `nand.bin`) |
| `wiinx-uam-object` | the one object that carries UAM, the shader compiler, into a Switch build ([docs/deko3d.md](../docs/deko3d.md)) |
| `wiinx-distcc-launch` | the compiler launcher for `wiinx-build --distcc`: strips the absolute path distccd refuses ([docs/distcc.md](../docs/distcc.md)) |

Nothing any tool downloads or extracts is ever committed: it stays with whoever
owns the disc or console.

`wiinx-extract --file` is the way into two-in-one discs, whose `main.dol` is
only a launcher:

```sh
tools/wiinx-extract "Wii Sports + Wii Sports Resort.iso" sports.dol --file US/sys/sports/SportsPackUP.dol
```

## Notes

- 2026-09-22: disc, DOL, NAND and game-project tools are all here. A game is
  created, translated and profiled from its own folder, wherever it lives;
  `wiinx-new-game` writes the path back to this checkout into its `recomp.yml`.
- `wiinx-build` puts them together: disc to NRO, through `cmake/game`. It is
  how every game here is built; on the native renderer Aurora and Dawn are
  configured for their headers only.
- 2026-09-22: `wiinx-scan` and `wiinx-extract` added. A disc image that ends
  before a file does - a truncated copy - reads as `short read: 0 of N bytes`.
- 2026-09-23: building New Super Mario Bros. Wii from nothing found four of
  these tools wrong for any game but the first: `wiinx-extract-disc` assumed
  the layout dolphin-tool gives some discs, `wiinx-translate` reused a stale
  translator binary and ran the REL manifest step for a game with no REL, and
  `wiinx-new-game` refused to work inside a repository that already existed.
  `wiinx-scan functions` was added the same day, because without a symbol map
  that game translated only 1,999 of its functions.
- 2026-10-12: `wiinx-build` builds with libdol's deko3d renderer unless the
  game's `recomp.yml` lists `runtime.defines` without `WIINX_NATIVE_GX`
  (`defines: []` is Aurora's build); the translator reads the same rule.
  `wiinx-near-map` added (docs/signatures.md, "Builds nobody has mapped").
