# accel/nw4r/lyt

Layouts: the screens a Wii game draws that are not the world - menus, HUDs,
buttons, and the animations they run.

| File | Natives |
|---|---|
| `pane.cpp` | `Pane::CalculateMtx`: each pane's matrix from its parent's, down the tree |
| `animation.cpp` | `Pane::Animate`, `Pane::AnimateSelf`, `Material::Animate`, `AnimTransformBasic::Animate` (the pane and the material form): the whole animation walk, root to last key |

Both are ported from the ogws decompilation (doldecomp/ogws, CC0) and checked
against the shipped code of each build they are written for:

| Build | Read off | Layout |
|---|---|---|
| `nw4r.lyt@2007-06-08` | ogws's headers (Wii Sports) | pane flags at 0xB7; AnimTransform's `Animate` slots 0x18/0x1C |
| `nw4r.lyt@2008-03-08` | Mario Kart Wii | flags at 0xBB (four bytes after the matrices); slots 0x1C/0x20 |
| `nw4r.lyt@2009-04-02` | New Super Mario Bros. Wii | the same as 2008, function for function |

The 2007 material form is not written: it swaps texture patterns through a
TexMap built whole from the TPL rather than `TexMap::ReplaceImage`, and no 2007
game is at hand to check it against. Its walks still run natively and call the
game's own through the vtable.

The animation is exact: the curves are the original's single-precision
operations in its order (the 2009 code has no fused multiply-adds, and the file
is built without contraction), colours clamp and truncate as Gekko's quantised
stores do, and a texture swap binds the TPL in place as `TPLBind` does
(`accel/sdk/tpl`).

## How the walk stays native

A walk leaves for the game only where the game may have something of its own:
a pane, material or animation class whose vtable entry is not one of these
natives (a `Window` animates its frames' materials in its own `AnimateSelf`),
and vertex colours, which `Picture`, `TextBox` and `Window` set themselves.
Everywhere else the next call is the native, run directly.

Both depend on where the game binds each native, which its generated bindings
register at startup (`wiinx::bound`, docs/natives.md): the address its vtables
hold, and the return address after each indirect call the original makes, so a
call out to the game leaves lr where the original's would be however the
natives nest.

## Builds of games without a banner

Bomberman Blast's lyt has no banner - the string is not in its executable, so it
never tells the console either - and its code fits both the 2008 and the 2009
layout field for field. Its other nw4r libraries (EF, G3D, SND) say Nov 15 2007,
and nw4r ships as one release, so its lyt is that release's; the build nearest
it that fits is taken (`2008-03-08`). `tools/wiinx-emit-bindings` prints the
reasoning.

## Check

`tests/nw4r_lyt_check.cpp` (matrices) and `tests/nw4r_lyt_animation_check.cpp`
(a layout in fake guest memory: every kind of target, the curves' exact values,
a TPL bound and swapped in, calls out with their return addresses, each build's
offsets).
