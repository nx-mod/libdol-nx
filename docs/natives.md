# Writing a native

A native is C++ that replaces one guest function. It gets the guest CPU,
reads its arguments from registers, works on guest memory through typed
layouts, and writes its result back.

## Shape

```cpp
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

namespace wiinx::nw4r::lyt {
namespace {
struct PaneLayout2008 {                           // one build's offsets
    static constexpr Field<u8, 0xB8> alpha{};
    static constexpr Field<Mtx34, 0x84> glbMtx{};
};

template <class L>
void calculate_mtx(Cpu* cpu) {
    const Host& h = host();
    const Guest<L> self{h.gpr(cpu, 3)};           // arguments: r3, r4, ...
    u8 alpha = self[L::alpha];                    // big-endian, through the host's mapping
    ...
    h.set_gpr(cpu, 3, result);                    // results: r3 (f1 for floats)
}
}  // namespace

extern const Native kPaneNatives[] = {
    WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kLyt_2008_03, calculate_mtx<PaneLayout2008>),
};
}
```

The module's `natives()` returns its tables; the program adds each module with
`add_natives()`.

## Builds

- Same logic, different struct layout: one template, one layout per build, one
  `WIINX_NATIVE` line per build. `Pane::CalculateMtx` is this: its 2007 and
  2008 builds differ only in where three fields sit.
- Different logic: a separate function for that build.
- Find the differences in the decompilations (ogws for 2007 nw4r, doldecomp/mkw
  for 2008) and in the games' own code; check a layout offset in two builds
  before trusting it.

## Guest calls and memory

- `h.call(cpu, target)` calls a guest function with arguments already set by
  `set_gpr`. Virtual calls read the vtable through `load<u32>`.
- A native that writes memory a texture is read from calls
  `h.notify_write(addr, size)`, or the renderer keeps drawing the old data.
- Guest pointers are 32-bit big-endian addresses, never host pointers: a struct
  with pointers cannot be used natively, only read through a layout.

## Exactness

Gameplay-visible results must match the original bit for bit. Paired-single
math on the Wii rounds once per fused multiply-add; the runtime's translated
code does too, and a native must follow the same operation order, or two runs
of a ghost diverge. UI-only code (layout matrices) may use plain float math;
say so in the file header.

## Checks

Each native gets a test in `tests/` on fake guest memory: a small structure,
the expected results, both builds. Game data never goes in a test.

## Checklist

1. Header comment: what it replaces, why it is worth it, its sources.
2. Layouts per build, from a decompilation and the games.
3. Registered for every build, with a signature for each (`wiinx-scan sign`).
4. A test.
5. Credits in `THIRD-PARTY-NOTICES.md`; a note in the module's README.

## Bindings write themselves

A native has to be registered at the address the game has, before that game is
translated - the translator skips emitting a function it knows is replaced, and
a registration added afterwards collides with the code already emitted.

That used to mean a hand-written file of near-identical wrappers per module,
each carrying one game's addresses, which is how Mario Kart Wii's addresses
ended up inside the library twice.

`tools/wiinx-emit-bindings` writes them instead, into
`<game>/native/wiinx_bindings.cpp`, and `wiinx-translate` runs it first. It
finds each native's address in that game by:

1. the game's `bindings.json`, where a scan put what it matched by code, and
2. the game's symbol map, if it has one, matched by name - an SDK function the
   map files under its module (`GX::InitTexObj`) matching the native's own
   spelling (`GXInitTexObj`).

It registers a native written for a particular build only in a game that links
that build, and leaves alone any native a hand-written binding already claims -
some need setup the generated form cannot know about, like the layout pane's
resume address.

A library a game links with no banner of its own (Bomberman Blast's nw4r.lyt)
still has a build, and its code says which: a signature records the struct
fields its function uses (`fields`, `wiinx-scan field_fingerprint` - a hash, no
code), and where the game's bound function uses exactly those of a signature
taken from a game whose banner names build B of that library, the game is laid
out as B. Builds that all fit are the same layout as far as the game's code can
tell: the one with natives for most of its functions is taken, then the one
nearest the release of the library's package - nw4r ships as one, so
Bomberman Blast's EF, G3D and SND banners (Nov 15 2007) date its LYT, which
fits both 2008-03 and 2009-04 and is given 2008-03. A native the game has but
cannot be given - another build, none proven - is named with the reason
(`withheld:`), never skipped silently.

### Where a game binds a native, for the native

The generated bindings register every native they bind at startup
(`wiinx::set_bound`): the guest address, and the return address after each
indirect call (`bctrl`) the original makes, in code order, read from the game's
own code. A native asks with `wiinx::bound(name)`:

- whether a vtable entry is one of its own natives - a layout pane's child, a
  material's animation - so the walk recurses natively instead of leaving
  through the guest dispatcher and coming back;
- where the original's nth call returns, for a call back into the game
  (`wiinx::call_guest(cpu, target, binding, n)`, `Host::call_from`): some
  callees resume through lr, and natives that call each other directly would
  otherwise leave the wrong one.

This replaced a per-native setup table and one resume address per call
(2026-11-03): a library of natives nests, and each needs its own.

### Overloads

Two functions with one name - `AnimTransformBasic::Animate(u32, Pane*)` and
`(u32, Material*)` - are named with their parameters as the demangler writes
them: `nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)`.
`wiinx-sign-natives` reads mangled map names through `tools/cwdemangle.py`, so
a published map (New Super Mario Bros. Wii's) signs them by name; a map of ours,
demangled without parameters, cannot tell them apart, and near-map finds them
there instead.

So adding a native is three things: write it, list it in the module's table,
and sign it once from a game whose symbols are known. Every other game picks it
up, with or without a map of its own - Punch-Out!!, which has none, binds them
by code.

## A native's contract moves with it

The translator also reads each native's register contract (what it reads and
writes) and argument layout from the sources, where the address is the one the
library was written against. Both go to the game's address through its
bindings, as the registration does, and are dropped where the game does not
bind the native (`RuntimeNativeIndex`). Until 2026-10-14 they stayed at Mario
Kart Wii's addresses, so whatever another game keeps there was called as that
native: Mega Man 9's nw4r reader at `0x801A65F8` as `__OSSetInterruptHandler`,
its third argument never passed. Every title had between 1 and 15 such functions.
