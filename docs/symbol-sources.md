# Where names come from

A name costs nothing to carry and saves hours: a bound native is verified by its
code, but knowing that a function is `PSMTXConcat` rather than `sub_8021A4C0` is
the difference between reading a game and guessing at it. Names come from four
places, and three of them are things a disc already has.

## 1. Maps a disc shipped with

Some games were pressed with the compiler's own link map still in the
filesystem. It names every function, with its address, its size and the file it
came from.

```sh
wiinx-disc list <image> | grep -i '\.map'
wiinx-disc file <image> debugInfoS.MAP
tools/wiinx-sign-map <game.dol> debugInfoS.MAP --write
```

| Game | On the disc | Why it matters |
|---|---|---|
| Mario Kart: Double Dash!! | `debugInfoS.MAP` | Nintendo's SDK and JSystem, 17,399 names |
| Super Mario Sunshine | `marioEU.MAP` (the PAL disc) | the same, from a different year |
| Pikmin | `build.MAP` | Nintendo first-party |
| Twilight Princess (Japan) | `frameworkF.MAP` | Nintendo's framework |
| The Wind Waker (Korea) | `f_pc_profile_lst.MAP` and others | JSystem |
| Animal Crossing | `foresta.map`, `static.map` | Nintendo first-party |
| Mario Smash Football | four `MarioSoccer*.MAP` | Nintendo first-party |
| Final Fantasy: Crystal Chronicles | `dvd/map/stg009/game.MAP` | |

## 2. Executables that kept their symbol table

Others shipped an ELF with its symbols intact - the DOL beside it being only a
loader. Any of them names the whole SDK it linked, which every other GameCube
game shares.

```sh
wiinx-disc list <image> | grep -i '\.elf'
wiinx-disc file <image> MOH3RDVD.elf
tools/wiinx-sign-elf MOH3RDVD.elf --write
```

The largest, by how many functions they name:

| Game | File | Names |
|---|---|---|
| GoldenEye: Rogue Agent | `GE2RDVD.ELF` | 20,654 |
| Harry Potter and the Goblet of Fire | `gof_f.elf` | 19,843 |
| Medal of Honor: European Assault | `MOH4RDVD.ELF` | 17,815 |
| Mission: Impossible - Operation Surma | `IMF_GC-Final.elf` | 17,258 |
| SpongeBob: Creature from the Krusty Krab | `SpongeBob_ngc_mfb.elf` | 16,884 |
| Freedom Fighters | `startup_release.elf` | 16,664 |
| Pac-Man World 3 | `PMA_GC_M.elf` | 16,619 |
| TY the Tasmanian Tiger 3 | `Ty3.elf` | 16,618 |
| The Incredibles | `ingc_m.elf` | 13,931 |
| Medal of Honor: Rising Sun | `MOH3RDVD.elf` | 9,941 |
| Super Mario Strikers (Japan) | `MarioSoccerR.elf` | 8,350 |

Around eighty retail discs carry one. The full list is catalogued at
[RetroReversing](https://www.retroreversing.com/gamecube-debug-symbols).

## 3. Decompilation projects

Projects using decomp-toolkit keep a symbol file per game at
`config/<GAMEID>/symbols.txt`, which is the same thing in a different spelling.
These are only useful for a game you have, since a name has to be signed against
the code it belongs to.

| Project | Licence | Game |
|---|---|---|
| [doldecomp/mkdd](https://github.com/doldecomp/mkdd) | CC0 | Mario Kart: Double Dash!! |
| [doldecomp/sms](https://github.com/doldecomp/sms) | CC0 | Super Mario Sunshine |
| [zeldaret/tww](https://github.com/zeldaret/tww) | CC0 | The Wind Waker |
| [zeldaret/tp](https://github.com/zeldaret/tp) | CC0 | Twilight Princess |
| [adonis-singh/re4](https://github.com/adonis-singh/re4) | CC0 | Resident Evil 4, complete |
| [doldecomp/ogws](https://github.com/doldecomp/ogws) | CC0 | Wii Sports |
| [doldecomp/mkw](https://github.com/doldecomp/mkw) | CC0 | Mario Kart Wii |

A project with no licence is read, not taken from, and nothing derived from one
is committed here.

## 4. The set itself

Every game signed adds to `data/symbols.json.gz`, and the set names the next
game nobody has mapped. What it is worth is measured in
[signatures.md](signatures.md).

## Worth dumping for names alone

If the aim is to name as much of the two consoles' shared code as possible, the
discs that pay most are the ones above: one Nintendo first-party title for the
SDK and its middleware, and one large third-party title for the breadth of its
symbol table. Two discs cover most of what either console runs.
