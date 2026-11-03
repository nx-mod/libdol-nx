# accel/nw4r

Natives for nw4r, Nintendo's Wii middleware: `lyt` layouts, `g3d` 3D, `snd`,
`ef` effects, `math`. Header: `include/wiinx/accel/nw4r.hpp`. Library:
`wiinx::nw4r`.

## Builds known

| Build id              | Banner                           | Seen in        |
|-----------------------|----------------------------------|----------------|
| `nw4r.lyt@2007-06-08` | `NW4R - LYT` Jun 8 2007 (0x4199_60831) | Wii Sports |
| `nw4r.lyt@2008-03-08` | `NW4R - LYT` Mar 8 2008 (0x4201_127)   | Mario Kart Wii; Bomberman Blast's banner-less copy is laid out as it |
| `nw4r.lyt@2009-04-02` | `NW4R - LYT` Apr 2 2009 (0x4302_145)   | New Super Mario Bros. Wii |

## Natives

| Name | Builds | File |
|---|---|---|
| `nw4r::lyt::Pane::CalculateMtx` | 2007-06, 2008-03, 2009-04 | `lyt/pane.cpp` |
| `nw4r::lyt::Pane::Animate` | 2007-06, 2008-03, 2009-04 | `lyt/animation.cpp` |
| `nw4r::lyt::Pane::AnimateSelf` | 2007-06, 2008-03, 2009-04 | `lyt/animation.cpp` |
| `nw4r::lyt::Material::Animate` | 2007-06, 2008-03, 2009-04 | `lyt/animation.cpp` |
| `nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)` | 2007-06, 2008-03, 2009-04 | `lyt/animation.cpp` |
| `nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Material*)` | 2008-03, 2009-04 | `lyt/animation.cpp` |
| `nw4r::lyt::detail::DrawQuad` (with and without the alpha) | 2007-06, 2008-03, 2009-04 | `lyt/draw_quad.cpp` |
| `nw4r::ut::CharWriter::PrintGlyph` | any (`nw4r.ut` has no banner; 2007-11 and 2009 agree field for field) | `ut/char_writer.cpp` |

An overloaded function is named with its parameters, as the demangler writes
them (`tools/cwdemangle.py`): one name per function. What each build lays out
where, and how the walk stays native, is in `lyt/README.md`.

`Pane::CalculateMtx` runs the same logic in every build; `PaneLayout2007` and
`PaneLayout2008` differ only in where `alpha`, `glbAlpha` and `flag` sit (0xB4
vs 0xB8: the 2008 build added four bytes after the matrices; 2009 is 2008's).

## Check

`ctest` in a host build runs `nw4r_lyt_check` (matrices: a parent and child
pane, chained matrices, inherited alpha, the child reached through its vtable)
and `nw4r_lyt_animation_check` (the animation walk on a fake layout, every
build).

## Notes

- 2026-09-22: `Pane::CalculateMtx` ported (2008 layout). The 2007 layout is
  new, from ogws's `lyt_pane.h`; vtable slot 0x10 and parent at 0x0C match 2008.
- 2026-11-03: the animation walk ported whole (ogws lyt_animation.cpp,
  lyt_pane.cpp, lyt_material.cpp, lyt_texMap.cpp), checked against Mario Kart
  Wii's, New Super Mario Bros. Wii's and Bomberman Blast's code.
- Calls back into the game go through `wiinx::call_guest` with the native's own
  binding (`wiinx::bound`): only the runtime knows where a native is bound in a
  given game and where the original's calls return, so a native never
  hard-codes an address.
- 2026-11-03: `ut::CharWriter::PrintGlyph`: the quad in one FIFO write, the
  texture through the game's own GX functions when it changes (its static
  cache from globals.json).
- Next: g3d `CalcWorld` and `CalcView` (located in Mario Kart Wii at 0x800679A0
  and 0x80066AA0). They feed gameplay-visible state, so exact float order.
