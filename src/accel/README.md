# accel

Native versions of libraries games link into their own code. Optional: an
unbound function runs as the game's translated code. Every native must give the
original's exact results; gameplay code checks them (ghosts, online).

| Module    | Covers                                                    | Written so far                         |
|-----------|-----------------------------------------------------------|----------------------------------------|
| `sdk`     | MSL C library, PSMTX/VEC math, GD, THP video              | THP decoder (RVL SDK THP, Aug 2007)    |
| `nw4r`    | lyt, g3d, snd, ef, math                                   | lyt `Pane::CalculateMtx` (2008 layout) |
| `egg`     | Nintendo EAD's framework                                  | -                                      |
| `jsystem` | the GameCube-era framework                                | -                                      |
| `rfl`     | Miis, Home Button menu                                    | -                                      |

The two natives written so far live in wiicompiled-nx
(`runtime/src/hle/thp_decode.cpp`, `runtime/src/hle/nw4r/lyt_pane.cpp`) and
move here first.

## Adding a native

1. Name it as the original: `nw4r::g3d::CalcWorld`.
2. Find the builds that differ, in the decompilations and the games' banners.
3. Write one implementation per behavior; give each build a layout.
4. List it in the module's table with `WIINX_NATIVE`, once per build.
5. Credit its sources in the file header and `THIRD-PARTY-NOTICES.md`.

## Notes

- Pure-math functions (PSMTX, THP's IDCT, MSL math) can follow decompiled source
  closely. Functions that walk game structures read them through `Guest<>`:
  guest pointers are 32-bit and big-endian, host ones are not.
- Candidates found in Mario Kart Wii: `CalcWorld` 0x800679A0, `CalcView`
  0x80066AA0, `CalcView_LC` 0x80066DD0, `G3DState::LoadResShpPrimitive` 0x80063870.
