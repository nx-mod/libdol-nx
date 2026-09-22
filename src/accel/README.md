# accel

Native versions of libraries games link into their own code. Optional: an
unbound function runs as the game's translated code. Every native must give the
original's exact results; gameplay code checks them (ghosts, online).

| Module    | Covers                                                    | Written so far                         |
|-----------|-----------------------------------------------------------|----------------------------------------|
| [`sdk`](sdk/README.md)   | MSL C library, PSMTX/VEC math, GD, THP video | THP decoder (RVL SDK THP, Aug 2007)       |
| [`nw4r`](nw4r/README.md) | lyt, g3d, snd, ef, math                       | lyt `Pane::CalculateMtx` (2007, 2008)     |
| `egg`     | Nintendo EAD's framework                                  | -                                      |
| `jsystem` | the GameCube-era framework                                | -                                      |
| `rfl`     | Miis, Home Button menu                                    | -                                      |

Both natives written so far are ported here as copies of wiicompiled-nx's
(`runtime/src/hle/thp_decode.cpp`, `runtime/src/hle/nw4r/lyt_pane.cpp`); the
runtime keeps using its own until the SDK builds Mario Kart Wii.

## Adding a native

1. Name it as the original: `nw4r::g3d::CalcWorld`.
2. Find the builds that differ, in the decompilations and the games' banners.
3. Write one implementation per behavior; give each build a layout.
4. List it in the module's table with `WIINX_NATIVE`, once per build, and give
   new builds a `LibVersion` in the module header.
5. Credit its sources in the file header and `THIRD-PARTY-NOTICES.md`.

## Notes

- Pure-math functions (PSMTX, THP's IDCT, MSL math) can follow decompiled source
  closely. Functions that walk game structures read them through `Guest<>`:
  guest pointers are 32-bit and big-endian, host ones are not.
- Candidates found in Mario Kart Wii: `CalcWorld` 0x800679A0, `CalcView`
  0x80066AA0, `CalcView_LC` 0x80066DD0, `G3DState::LoadResShpPrimitive` 0x80063870.
