# tools

Helpers for working on libwii-nx and binding games to it. Python 3, no
dependencies beyond the standard library (openssl speeds up extraction when
installed).

| Tool | Does |
|---|---|
| `wiinx-scan` | lists a DOL's library builds; finds natives by signature; signs functions; records builds and generates `include/wiinx/builds.hpp`. See [docs/signatures.md](../docs/signatures.md) and [docs/builds.md](../docs/builds.md) |
| `wiinx-extract` | pulls `main.dol`, or any file with `--file <path>`, out of a Wii `.iso` without extracting the disc: decrypts only the clusters it needs |
| `wiinx-extract-disc` | the whole disc (ISO/WBFS/RVZ/GCZ/CISO), through Dolphin's `dolphin-tool`, with a free-space check |
| `wiinx-inspect-dol` | a DOL's entry point, sections, BSS and small-data bases; `--yaml` for a project file |
| `wiinx-unpack-u8` | lists or extracts a U8 archive (how channels pack their files), flagging compression inside |
| `wiinx-fetch-title` | downloads one Wii system title from Nintendo's update servers and unpacks its executable |
| `wiinx-fetch-nand` | a reference set of system titles into `nand/` (not tracked), from a menu or by flag |
| `wiinx-sysconf` | reads and edits a Wii SYSCONF, the console's settings |
| `wiicrypto.py`, `wiinand.py` | the Wii's AES, keys and title download, shared by the tools above |

Nothing any tool downloads or extracts is ever committed: it stays with whoever
owns the disc or console.

`wiinx-extract --file` is the way into two-in-one discs, whose `main.dol` is
only a launcher:

```sh
tools/wiinx-extract "Wii Sports + Wii Sports Resort.iso" sports.dol --file US/sys/sports/SportsPackUP.dol
```

## Notes

- 2026-09-22: disc, DOL and NAND tools ported in; the game-project tools (new
  game, translate, build) come with the translator and runtime.
- 2026-09-22: `wiinx-scan` and `wiinx-extract` added. A disc image that ends
  before a file does - a truncated copy - reads as `short read: 0 of N bytes`.
