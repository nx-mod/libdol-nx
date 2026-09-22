# tools

Helpers for working on libwii-nx and binding games to it. Python 3, no
dependencies beyond the standard library (openssl speeds up extraction when
installed).

| Tool | Does |
|---|---|
| `wiinx-scan` | lists a DOL's library builds; finds natives by signature; signs functions; records builds and generates `include/wiinx/builds.hpp`. See [docs/signatures.md](../docs/signatures.md) and [docs/builds.md](../docs/builds.md) |
| `wiinx-extract` | pulls `main.dol`, or any file with `--file <path>`, out of a Wii `.iso` without extracting the disc: decrypts only the clusters it needs |
| `wiicrypto.py` | the Wii's AES and disc constants, for `wiinx-extract` |

`wiinx-extract` is a copy of wii-nx's `example-wii-nx/scripts/extract-dol`, with
`--file` added - the way into two-in-one discs, whose `main.dol` is only a
launcher:

```sh
tools/wiinx-extract "Wii Sports + Wii Sports Resort.iso" sports.dol --file US/sys/sports/SportsPackUP.dol
```

## Notes

- 2026-09-22: `wiinx-scan` and `wiinx-extract` added. A disc image that ends
  before a file does - a truncated copy - reads as `short read: 0 of N bytes`.
