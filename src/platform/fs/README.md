# platform/fs

Where a game's files come from, and where its saves go.

| File | Is |
|---|---|
| `dvd.cpp` | the disc: reads, the file table, and the timing a game expects of them |
| `nand_fs.cpp`, `nand_isfs.cpp`, `nand_api.cpp`, `nand_async.cpp` | the NAND as a game sees it |
| `ios.cpp`, `esp.cpp` | IOS and the title service behind those calls |
| `riivolution.cpp` | replacing a disc's files with a mod's, without touching the disc |

A read is served from an extracted folder today, and from the disc image itself
once `format/disc` lands - the interface above does not change either way.
