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
