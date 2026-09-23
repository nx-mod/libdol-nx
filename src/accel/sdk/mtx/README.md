# sdk/mtx

The SDK's matrix library, natively. Eight functions so far: `PSMTXIdentity`,
`PSMTXConcat`, `PSMTXConcatArray`, `PSMTXCopy`, `PSMTXTranspose`, `PSMTXTrans`,
`PSMTXTransApply`, `PSMTXScale`, `PSMTXScaleApply`.

## Why a whole section rather than a function

A native costs a boundary crossing. When only some of a library is native, the
rest keeps calling back into the guest and the crossings can cost more than the
native saves - the layout pane native spent its gain that way until it learned
to compute a child directly. A library taken whole crosses the boundary once,
at the entry, and never again.

## Why these are safe to take whole

They are hand-written assembly in the SDK and identical in every build of it,
like the cache and interrupt primitives, so one native serves every game and
`kMtx_Any` is the version they register under.

## Bit-exactness

The results reach physics and replays, so a differing last bit is a different
race. `PSMTXConcat` multiplies with `ps_muls0` and accumulates with
`ps_madds0`/`ps_madds1`, which are **fused**: one rounding for a multiply and
an add together. `std::fma` in the same order is the same answer, and a plain
`a * b + c` is not. The translation column adds `a[i][3]` through a fused add
against the constant `(0, 1)`, so it is an `fma` here too.

The arithmetic follows doldecomp/dolsdk2004 `src/mtx/mtx.c` (CC0), instruction
for instruction; the source is credited in THIRD-PARTY-NOTICES.md.

## What is not here

`PSMTXInverse` (a reciprocal whose rounding needs the same treatment) and
`PSMTXRotTrig` (trigonometry). `PSMTXTranspose` is written but Mario Kart Wii's
map has no address for it, so nothing binds it yet.

## Notes

- 2026-09-23: written. `Host` gained `fpr`/`set_fpr` for this: the SDK passes a
  float argument in f1-f3, and a native that can only read general registers
  cannot implement `PSMTXTrans` at all - an earlier attempt read them from a
  pointer that was never there.
- 2026-09-23: the hand-written binding is gone. `tools/wiinx-emit-bindings`
  writes each game's registrations from its own `bindings.json` and symbol map,
  and `wiinx-translate` runs it first, because a native only takes effect if
  its registration exists when the game is translated. Adding a native is now
  writing it, listing it in the module table, and signing it once from a game
  whose symbols are known.
