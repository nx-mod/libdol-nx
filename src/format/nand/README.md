# format/nand

The Wii's internal memory, as files.

| Header | Holds |
|---|---|
| `wiinx/format/nand/isfs.hpp` | title ids, the paths IOS keeps things at, `setting.txt` |
| `wiinx/format/nand/sysconf.hpp` | the settings every game reads: language, aspect, sound, sensor bar |
| `wiinx/format/nand/mii.hpp` | `RFL_DB.dat`, the Mii database, read and written slot by slot |
| `wiinx/format/nand/title.hpp` | tickets and TMDs: what a title is and what it carries |
| `wiinx/format/nand/wad.hpp` | a title packed as one file, installable or boot2 |

## What is here and what is not

Formats, not keys. A ticket's title key is carried as it is found, still
wrapped; the two initialisation vectors the format defines are provided because
they are format, and the keys and the AES are the caller's.

Signatures are read and reported - the type, the issuer - and verified by
nothing here. Trust is a decision, and it belongs to whatever is installing.

## Checks

`nand_sysconf_check` and `nand_mii_check` run against files the runtime's own
first-run code wrote, so the two cannot drift. `nand_title_check` builds
tickets, TMDs and WADs of both kinds in memory and reads them back.
