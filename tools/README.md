# tools

Helpers for working on libwii-nx and binding games to it. Python 3, no
dependencies beyond the standard library (openssl speeds up extraction when
installed).

| Tool | Does |
|---|---|
| `wiinx-scan` | lists a DOL's library builds; finds natives by signature; finds where a game's functions start, in its own code (`functions`); finds where its copy of the Wii's OS keeps the scheduler's globals (`os-globals`); signs functions; records builds and generates `include/wiinx/core/builds.hpp`. See [docs/signatures.md](../docs/signatures.md) and [docs/builds.md](../docs/builds.md) |
| `wiinx-extract` | pulls `main.dol`, or any file with `--file <path>`, out of a Wii `.iso` without extracting the disc: decrypts only the clusters it needs |
| `wiinx-extract-disc` | the whole disc into a game project's `disc/`, through this library's own reader; Dolphin's `dolphin-tool` only as a fallback |
| `wiinx-disc` | the reader itself (built from `tools/src/`): `info`, `list`, `dol`, `file`, `extract`. Reads raw images, CISO, WBFS, RVZ and WIA, GameCube and Wii |
| `wiinx-inspect-dol` | a DOL's entry point, sections, BSS and small-data bases; `--yaml` for a project file |
| `wiinx-wad` | looks inside a WAD - which title it is, what it wants, what it carries - and unpacks one, decrypting each content and writing the executable as `main.dol`. Reads both kinds, installable and boot2 |
| `wiinx-unpack-u8` | lists or extracts a U8 archive (how channels pack their files), flagging compression inside |
| `wiinx-fetch-title` | downloads one Wii system title from Nintendo's update servers and unpacks its executable |
| `wiinx-fetch-nand` | a reference set of system titles into `nand/` (not tracked), from a menu or by flag |
| `wiinx-sysconf` | reads and edits a Wii SYSCONF, the console's settings |
| `wiicrypto.py`, `wiinand.py` | the Wii's AES, keys and title download, shared by the tools above |
| `wiinx-new-game` | a whole game project from your own disc - an image, or one already extracted: `game.toml`, `recomp.yml` pointing back at this checkout, README, `.gitignore` |
| `wiinx-read-boot` | what a disc says it is (ID, revision, region, title), read from its `sys/boot.bin` |
| `wiinx-translate` | the translator's four steps over a game project, with a memory guard |
| `wiinx-audit` | how universal the natives are: SDK, middleware, or one game's |
| `wiinx-manual-adds` | what a game still needs by hand, and `--template` to start one |
| `wiinx-make-bindings` | resolved addresses as the C++ table a game's build links |
| `wiinx-profile-report` | a device log's profile, with the game's function names |
| `wiinx-check-layers` | that the layers only depend downward (app → accel → platform → cpu → core) |
| `wiinx-build` | disc (or game folder) to `.nro`: scan, translate, build, package |

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
- `wiinx-build` puts them together: disc to NRO, through `cmake/game`, which
  composes Dawn, Aurora and libwii-nx the way the working build does. Written
  from that recipe; the pieces are proven, the one command is not yet.
- 2026-09-22: `wiinx-scan` and `wiinx-extract` added. A disc image that ends
  before a file does - a truncated copy - reads as `short read: 0 of N bytes`.
- 2026-09-23: building New Super Mario Bros. Wii from nothing found four of
  these tools wrong for any game but the first: `wiinx-extract-disc` assumed
  the layout dolphin-tool gives some discs, `wiinx-translate` reused a stale
  translator binary and ran the REL manifest step for a game with no REL, and
  `wiinx-new-game` refused to work inside a repository that already existed.
  `wiinx-scan functions` was added the same day, because without a symbol map
  that game translated only 1,999 of its functions.
