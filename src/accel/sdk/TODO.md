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
- [ ] The rest of `nw4r::lyt::Material::SetupGX`'s GX setters as native GX:
      GXSetTevColorOp/AlphaOp, GXSetTevKColor/KColorSel/KAlphaSel,
      GXSetTevColorS10, GXSetTevSwapMode(Table), GXSetChanCtrl/ChanMatColor,
      GXSetNumTevStages/TexGens/Chans/IndStages, GXSetTexCoordGen2,
      GXSetBlendMode, GXSetAlphaCompare, GXSetTevDirect/Indirect,
      GXLoadTexMtxImm (Bomberman Blast's 2007-12 code, as gx_tev.cpp)
