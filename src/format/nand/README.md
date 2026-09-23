# format/nand

The Wii's internal memory, as files.

| Header | Holds |
|---|---|
| `wiinx/format/nand/isfs.hpp` | title ids, the paths IOS keeps things at, `setting.txt` |
| `wiinx/format/nand/sysconf.hpp` | the settings every game reads: language, aspect, sound, sensor bar |
| `wiinx/format/nand/mii.hpp` | `RFL_DB.dat`, the Mii database, read and written slot by slot |
| `wiinx/format/nand/title.hpp` | tickets and TMDs: what a title is and what it carries |
| `wiinx/format/nand/wad.hpp` | a title packed as one file, installable or boot2 |
| `wiinx/format/nand/store.hpp` | a NAND to keep them in: install a title, list one, remove one |

## What is here and what is not

Formats, not keys. A ticket's title key is carried as it is found, still
wrapped; the two initialisation vectors the format defines are provided because
they are format, and the keys and the AES are the caller's.

Signatures are read and reported - the type, the issuer - and verified by
nothing here. Trust is a decision, and it belongs to whatever is installing.

## A NAND is wherever you keep it

`store.hpp` does no I/O. A caller hands over four functions - read, write,
remove, list - and installing a title, listing what is there and removing one
are written once and work the same wherever the NAND lives: an SD card, a folder
on a PC, or a map in a test.

Installing follows what a console does rather than what is convenient: contents
are written decrypted, a content a title shares with others goes to `/shared1`
under the name the map there gives it, and removing a title leaves those alone
because another title may be using them.

## Checks

`nand_sysconf_check` and `nand_mii_check` run against files the runtime's own
first-run code wrote, so the two cannot drift. `nand_title_check` builds
tickets, TMDs and WADs of both kinds in memory and reads them back.
