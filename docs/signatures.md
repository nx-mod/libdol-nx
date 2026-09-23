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
translator binds every call to it. Without one, a native keeps the address it
was registered at, which is the game it was written from.

A native that does not match is left unbound - reported as `not found`, with
the number of matches - and the game's translated code runs.

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

## The console's own natives

`accel` natives are bound this way today. The `platform` natives - the console
itself, around 700 of them - are still registered at the addresses they were
written from, Mario Kart Wii's, by `PPC_NATIVE_OVERRIDE(<address>, ...)`. They
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
