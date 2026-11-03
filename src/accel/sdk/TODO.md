# TODO - accel/sdk

- [ ] GD, the display-list builder games use to pack graphics commands
- [ ] The rest of the C library: `strcpy`, `strncmp`, `memcmp`, `qsort`
- [ ] The maths the SDK ships: `sin`, `cos`, `atan2`, the ones a game's physics
      calls per object per frame
- [ ] `rvl.mtx` beyond the 8 already here, and the vector and quaternion sets
- [ ] AXFX as a library of its own here (`rvl.axfx`), both reverbs together:
      `AXFXReverbStdExpCallback` (Mario Kart Wii) and `AXFXReverbHiCallback`
      (Bomberman Blast) are platform natives in `platform/audio/ax_effects.cpp`,
      bound by signature; written against `Host` and registered by name and build,
      with a check each on fake guest memory
- [ ] TPLBind and TPLGet as natives of their own (`tpl/tpl.cpp` has them as
      helpers for lyt): sign them, and check the shipped builds agree
- [ ] The last of `nw4r::lyt::Material::SetupGX`'s setters, each once per
      material: `GXSetTexCoordGen2` (a jump table over the gen type),
      `GXSetTevIndirect` (stack arguments; only indirect materials need it -
      `GXSetTevDirect` for the common disable is done), `GXSetChanMatColor`,
      `GXLoadTexMtxImm` (a matrix into XF memory), `GXLoadTlut` (a palette into
      TMEM). The TEV, konstant, swap, count, blend, alpha-compare, channel and
      indirect-order/scale setters are done (gx_tev.cpp)
