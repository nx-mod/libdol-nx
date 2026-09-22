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
other the whole function. Masking follows wii-nx's `resolve-symbols`.

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

A native that does not match is left unbound - reported as `not found`, with
the number of matches - and the game's translated code runs.

## Signing a function

From your own disc's executable, with the function's address and size (from a
symbol map or decompilation):

```sh
tools/wiinx-scan sign main.dol "nw4r::lyt::Pane::CalculateMtx" nw4r.lyt@2008-03-08 80078EF0 300
```

prints the entry to add to `data/signatures.json`. Functions shorter than 16
instructions are not signed; they are too common to be unique. Every build a
native supports needs its own signature, signed from a game that links it.

## Not yet

- Following calls from a matched function to its callees (how
  `resolve-symbols` reaches short functions) needs call offsets stored per
  signature; the format leaves room for them.
- Turning bindings into the translator's `bindings.json` comes with the runtime
  using libwii-nx.
