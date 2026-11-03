# accel/sdk/gx

The SDK's GX functions, natively. Each does what the SDK function does: it
updates the game's own GX state (`gxData`, through the layout in
`gx_internal.hpp`) and writes the same register words into the command stream
(`wiinx::Host::gx_write`). So any one can be bound alone, next to GX functions
still running as the game's code, and display lists, `GXBeginDisplayList`'s save
of `gxData` and the `GXGet*` functions keep working.

These replace the old Aurora-bridge natives in `src/platform/gx`, which only
fed Aurora's copy of the state and stay for the Aurora build.

## Builds known

| Build id            | Banner                                  | Seen in                             |
|---------------------|-----------------------------------------|-------------------------------------|
| `rvl.gx@2007-12-11` | `RVL_SDK - GX` Dec 11 2007 (0x4199_60831) | Mario Kart Wii, Bomberman Blast, Pikmin 2 (Wii), channels |

## Natives

| Name                    | Builds     | File             |
|-------------------------|------------|------------------|
| `GXInitTexObj`          | 2007-12-11 | `gx_texture.cpp` |
| `GXInitTexObjCI`        | 2007-12-11 | `gx_texture.cpp` |
| `GXInitTexObjLOD`       | 2007-12-11 | `gx_texture.cpp` |
| `GXLoadTexObjPreLoaded` | 2007-12-11 | `gx_texture.cpp` |
| `GXLoadTexObj`          | 2007-12-11 | `gx_texture.cpp` |
| `GXSetScissor`          | 2007-12-11 | `gx_transform.cpp` |
| `GXSetTevColorIn`       | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetTevAlphaIn`       | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetTevOrder`         | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetTevColorOp`, `GXSetTevAlphaOp` | 2007-12-11 | `gx_tev.cpp` |
| `GXSetTevKColorSel`, `GXSetTevKAlphaSel` | 2007-12-11 | `gx_tev.cpp` |
| `GXSetTevSwapMode`, `GXSetTevSwapModeTable` | 2007-12-11 | `gx_tev.cpp` |
| `GXSetTevKColor`, `GXSetTevColorS10` | 2007-12-11 | `gx_tev.cpp` |
| `GXSetNumTevStages`     | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetBlendMode`, `GXSetAlphaCompare` | 2007-12-11 | `gx_tev.cpp` |
| `GXSetChanCtrl`         | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetTevDirect`        | 2007-12-11 | `gx_tev.cpp`       |
| `GXSetIndTexOrder`, `GXSetIndTexCoordScale` | 2007-12-11 | `gx_tev.cpp` |
| `GXBegin`               | 2007-12-11 | `gx_geometry.cpp`: the header and the SDK's dummy flush primitive; the dirty-state flush stays the game's (`__GXSetDirtyState`, through its binding) |

The deko3d build switches off the ~120 Aurora-era GX natives in platform/gx
(they call Aurora's GX), so under it every GX setter not written here runs as
the game's translated code, its FIFO writes a word at a time. Each one here
writes the original's words in one burst. `GXSetScissor` came first: Bomberman
Blast's Characters menu calls it thousands of times a frame; the TEV setters
next, which `nw4r::lyt::Material::SetupGX` calls up to eight times each per
material. What is left of SetupGX (`GXSetTexCoordGen2`, the full `GXSetTevIndirect`,
`GXSetChanMatColor`, `GXLoadTexMtxImm`, `GXLoadTlut`) is in TODO.md: each is
called once per material and the first three dispatch through a jump table or
take stack arguments, so they stay translated for now.

## Check

```sh
g++ -std=c++20 -O2 -Iinclude tests/sdk_gx_check.cpp src/core/*.cpp \
    src/accel/sdk/gx/gx_texture.cpp -o check && ./check
```

Every expected word in the check was worked out by hand from the original's
instructions, not from the native.

## Notes

- A native only takes effect once the game is translated with it: the
  translator skips a function it knows is replaced, and translated callers call
  each other directly. Add natives in batches, then re-translate.
- Registers are used as the original uses them - whole, where it compares or
  counts the whole register - and its tables and constants are read from the
  game's main.dol rather than assumed.
- Sources: Mario Kart Wii's instructions and offsets from doldecomp/mkw
  (`lib/rvl/gx`, CC0); field names and intent from doldecomp/ogws and
  projectPiki/pikmin2's GX (CC0).
