# Signatures and scanning

A native replaces one function, but each game links that function at its own
address. `tools/wiinx-scan` finds it by its code.

## What a signature is

Two hashes (truncated SHA-256) of the function's instructions with the operands
that differ between games zeroed:

| Masked | Why |
|---|---|
| `b` / `bl` / `bc` targets | where the function and its callees were linked |
| `lis` / `addis`, `twi` immediates | the high half of addresses |
| displacements off `r2` / `r13` | small-data offsets, laid out per game |

Opcodes, registers and stack offsets must match exactly. One hash covers the
first 16 instructions (the start window, used to find candidates quickly), the
other the whole function.

A signature holds no code: only the hashes, the size and the build. That is what
lets `data/signatures.json` be published.

## Scanning a game

```sh
tools/wiinx-scan builds game.dol     # the library builds it links
tools/wiinx-scan scan game.dol --out bindings.json
```

`scan` considers only natives whose build the game's banners name, hashes the
start window at every aligned address, and accepts a function when both hashes
match at exactly one address. Mario Kart Wii scans in about 3.5 seconds.

`--out` writes the game's `bindings.json` in the translator's format - the name
the runtime registers a native under, against the address it has in this game:

```json
{
 "LytPaneCalculateMtx_HLE": "0x80078EF0",
 "THPVideoDecode_HLE": "0x801B3BAC"
}
```

The game's `recomp.yml` names that file (`runtime.native_bindings`), and the
translator binds every call to it.

A native is registered at the address it has in the game it was written from,
so in any other game that address is some other function entirely - binding by
it would replace whatever happens to sit there. Every game therefore has to say
which it is:

```yaml
runtime:
  native_bindings: bindings.json   # this game's own table
  # or
  native_bindings: registered      # this IS the game the natives came from
```

Saying nothing is an error, not a default: the dangerous case is the one that
has to be written down.

A native that does not match is left unbound - reported as `not found`, with
the number of matches - and the game's translated code runs.

## Finding a game's functions

A game whose symbols nobody has mapped still has to be translated whole.
Recursive descent from the entry point only reaches what direct calls reach,
which in a C++ game is a fraction of it - everything behind a virtual call is
invisible. `wiinx-scan functions` finds the rest in the game's own code:

```sh
tools/wiinx-scan functions disc/sys/main.dol --out functions.map
```

It takes the target of every `bl`, every word in the image that points at code
following a return (vtables and pointer tables), and every place a function
opens its own stack frame (`stwu r1,-N(r1)`, with or without `mflr r0` first).
`recomp.yml` names the result as `translation.function_map.path`.

Measured against Mario Kart Wii's symbol map, it finds 91.8% of that game's
real function starts, and 2.8% of what it finds is not in the map. Map entries
are seeded speculatively - one that is not code is dropped with a count, never
an error - so the guesses cost translation time, not correctness. For New Super
Mario Bros. Wii it took the translation from 1,999 functions to 17,812.

## Signing a function

From your own disc's executable, with the function's address and size (from a
symbol map or decompilation):

```sh
tools/wiinx-scan sign main.dol "nw4r::lyt::Pane::CalculateMtx" nw4r.lyt@2008-03-08 80078EF0 300
```

prints the entry to add to `data/signatures.json`; give it the `registration`
name the runtime uses for that native. Functions shorter than 16
instructions are not signed; they are too common to be unique. Every build a
native supports needs its own signature, signed from a game that links it.

## Code that is the same in every game

Parts of the SDK are hand-written assembly - the cache range operations,
masking interrupts, reading the clock - and the assembler emits the same
instructions for every build of them. They are too short for the usual
signature (the window is 16 instructions; `OSDisableInterrupts` is five) and
have no relocated operands to mask, so they are signed whole, against no build:

```sh
tools/wiinx-scan sign-asm main.dol OSDisableInterrupts OS__DisableInterrupts_801a65ac 801a65ac 20
```

writes `"build": "any"`, which every game's scan considers. Signed from Mario
Kart Wii, these ten match New Super Mario Bros. Wii - a game linking library
builds two years newer - each at its own address:

| | Mario Kart Wii | New Super Mario Bros. Wii |
|---|---|---|
| `DCFlushRange` | `0x801A162C` | `0x801AC470` |
| `OSDisableInterrupts` | `0x801A65AC` | `0x801B1140` |
| `OSGetTime` | `0x801AAD5C` | `0x801B5F80` |

`sign-asm` is only right where the code really is build-independent: check a
signature against a second game before adding it, which is what the uniqueness
check inside it is for. Compiled C changes between builds and still needs a
signature per build.

## Signing the library's own natives

The console's natives are registered at the address they have in the game they
were written from. `wiinx-sign-natives` signs them all from that game, using
its symbol map for the function extents:

```sh
tools/wiinx-sign-natives mkwii-nx/disc/sys/main.dol mkwii-nx/MAP.txt --write
```

Each signature must find the function it was taken from, in that same game,
and find it once - a signature that matches somewhere else is describing
different code and is dropped rather than shipped.

They are signed against no build, because the masking removes what differs
between builds. That is worth stating plainly: the masker blanks branch
targets, `lis` halves, small-data displacements, **and the `addi`, `ori` or
load displacement that completes an address a `lis` started**. Without that
last one, the same function in two builds of the same library hashed
differently - four instructions out of 138 in `SelectThread`, each the low half
of a global's address, was enough.

A native too short to sign on its own - many SDK functions are four or five
instructions - is signed from its start through the functions that follow it,
until there is enough code to be unique. The linker keeps a translation unit's
functions in source order, so that run is the same wherever the library is
linked, and a match gives the native's address directly.

Signing the same native from more than one game is what covers more of them:
each game links a different build, and `--by-name` locates the registrations in
another game's map by name rather than by address.

Nothing is trusted on its own evidence. A signature is kept only when the whole
set, scanned against every game whose addresses are known, binds each native
where that game really has it - at home and in the others (`--cross`). Map
entries that are not functions at all, a switch's jump table or a case body,
are never signed.

What that bought, from one game's 560 registrations:

| game | SDK vintage | natives bound by code |
|---|---|---|
| Mario Kart Wii (signed from) | 2007-08 | 350 |
| Wii Sports rev 1 (signed from) | 2006-07 | 312 |
| New Super Mario Bros. Wii (signed from) | 2009 | 311 |
| Punch-Out!! | 2008 | 277 |

Punch-Out!! is the one game here with no symbol map of its own, so it only ever
receives; its count rises whenever another game is signed from.

They are matched by code and have not been run. A wrong binding is worse than
none, which is why the uniqueness and self-checks above exist, and why a native
that does not match is simply left to the game's own code.

Where a game's symbols come from, when the game has no decompilation of its
own: New Super Mario Bros. Wii's are a published symbol map
(github.com/RootCubed/NSMBW-Maps), one file per version, and the one for this
disc named every address the scan had already bound by code - 97 of 97, from a
project with no connection to this one. That is the strongest check these
signatures have had.

## The console's own natives

`accel` natives are bound this way today, and so are the ten assembly
primitives above. The rest of the `platform` natives - the console itself,
around 550 of them - are still registered at the addresses they were written
from, Mario Kart Wii's, by `PPC_NATIVE_OVERRIDE(<address>, ...)`. They
are the same functions every Wii game links, at each game's own addresses, so
another game binds none of them and runs its own translated SDK code instead:
correct, and far slower where it matters (DVD, DSP, VI).

Giving them signatures, so any game binds them by what the code is, is the next
milestone. Nothing about the natives changes - only how a game finds them - and
the same applies to the guest OS globals a game's SDK links into its own BSS
(the run queue, the reschedule counter), which a game supplies through
[game hooks](game-hooks.md).

## Not yet

- Following calls from a matched function to its callees - how short
  functions, too common to match alone, get found - needs call offsets stored
  per signature; the format leaves room for them.
- Turning bindings into the translator's input comes with the translator.

## Naming a game's functions

Signatures bind natives. The same idea, with the verification relaxed, also
carries *names*: `data/symbols.json.gz` holds one signature per named function
from every game whose symbol map anyone has, and `wiinx-name` gives those names
to a game nobody has mapped.

The names come from three places. Some maps are reconstructed by decompilation
projects; some shipped on the disc, because a few GameCube games were pressed
with the compiler's own link map still in their filesystem; and a few games
shipped an ELF with its symbol table intact, the disc's DOL being only a loader.
`wiinx-sign-map` reads the first two, `wiinx-sign-elf` the third.

What it is worth, measured:

| From | Into | Named |
|---|---|---|
| Mario Kart: Double Dash!! (`debugInfoS.MAP`, on the disc) | Super Mario Sunshine | 336 |
| the whole set | New Super Mario Bros. Wii | 2,262 |
| the whole set | a WiiWare game's payload (Crystal Defenders) | 2,610 |
| the whole set | Need for Speed: Most Wanted, GameCube | 1,625 |
| the whole set | Mario Smash Football's own executable | 5,600 |

Every game measured here is one the set does not contain: a game already in it
would be naming itself. Six maps and symbol tables are in it - Mario Kart Wii,
Mario Kart: Double Dash!!, Super Mario Sunshine, Twilight Princess, Mario Smash
Football and Medal of Honor: Rising Sun - which between them cover both
consoles, Nintendo's own middleware and a third-party engine.

A name is a hypothesis, not a fact: a short function can match another game's
code by coincidence. Natives are never bound this way - they bind by verified
signature - so a wrong name costs nothing but a wrong label.
