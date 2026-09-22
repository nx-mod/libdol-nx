# accel/nw4r

Natives for nw4r, Nintendo's Wii middleware: `lyt` layouts, `g3d` 3D, `snd`,
`ef` effects, `math`. Header: `include/wiinx/accel/nw4r.hpp`. Library:
`wiinx::nw4r`.

## Builds known

| Build id              | Banner                           | Seen in        |
|-----------------------|----------------------------------|----------------|
| `nw4r.lyt@2007-06-08` | `NW4R - LYT` Jun 8 2007 (0x4199_60831) | Wii Sports |
| `nw4r.lyt@2008-03-08` | `NW4R - LYT` Mar 8 2008 (0x4201_127)   | Mario Kart Wii |

## Natives

| Name                            | Builds              | File              |
|---------------------------------|---------------------|-------------------|
| `nw4r::lyt::Pane::CalculateMtx` | 2007-06-08, 2008-03-08 | `lyt/pane.cpp` |

`Pane::CalculateMtx` runs the same logic in both builds; `PaneLayout2007` and
`PaneLayout2008` differ only in where `alpha`, `glbAlpha` and `flag` sit (0xB4
vs 0xB8: the 2008 build added four bytes after the matrices).

## Check

```sh
g++ -std=c++20 -Iinclude tests/nw4r_lyt_check.cpp src/core/*.cpp \
    src/accel/nw4r/nw4r.cpp src/accel/nw4r/lyt/pane.cpp -o check && ./check
```

Builds a parent and child pane in fake guest memory and runs both builds:
chained matrices, inherited alpha, the child reached through its vtable, and
each build touching only its own offsets.

## Notes

- 2026-09-22: `Pane::CalculateMtx` ported from wiicompiled-nx's
  `runtime/src/hle/nw4r/lyt_pane.cpp` (copy; the runtime's stays in use until
  the SDK builds Mario Kart Wii). The 2007 layout is new, from ogws's
  `lyt_pane.h`; vtable slot 0x10 and parent at 0x0C match 2008.
- The runtime copy hard-codes Mario Kart Wii's return address for child calls;
  here the host sets the link register, since only it knows where a native is
  bound.
- Next: g3d `CalcWorld` and `CalcView` (located in Mario Kart Wii at 0x800679A0
  and 0x80066AA0). They feed gameplay-visible state, so exact float order.
