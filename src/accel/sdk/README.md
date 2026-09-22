# accel/sdk

Natives for the RVL SDK and the C library games link with it: THP movies,
PSMTX/VEC math, GD display-list builders, MSL (memcpy, math). Header:
`include/wiinx/accel/sdk.hpp`. Library: `wiinx::sdk`.

## Builds known

| Build id             | Banner                                   | Seen in        |
|----------------------|------------------------------------------|----------------|
| `rvl.thp@2007-08-08` | `RVL_SDK - THP` Aug 8 2007 (0x4199_60831) | Mario Kart Wii |

## Natives

| Name             | Builds     | File                 |
|------------------|------------|----------------------|
| `THPVideoDecode` | 2007-08-08 | `thp/thp_decode.cpp` |

`THPVideoDecode` decodes a THP frame (baseline JPEG with THP's own rules: no
byte stuffing, restarts without markers, GX I8 tile output) straight into the
game's three texture planes, then tells the graphics layer they changed.

## Check

```sh
g++ -std=c++20 -O2 -Iinclude tests/sdk_thp_check.cpp src/core/*.cpp \
    src/accel/sdk/sdk.cpp src/accel/sdk/thp/thp_decode.cpp -o check && ./check
```

Argument and header checks only: decoding real frames needs a game's movies,
which never belong in this repository.

## Notes

- 2026-09-22: ported, the decoder body unchanged and the entry reading its
  arguments through `Host`. Menus with movie buttons in Mario Kart Wii went from
  ~4 fps to ~40 with it.
- Sources: projectPiki/pikmin2's `THPDec.c` (CC0) for THP's departures from
  JPEG; libjpeg's `jidctflt.c` for the IDCT - based in part on the work of the
  Independent JPEG Group.
- Other THP builds are likely the same decoder; add their ids once a game shows
  its banner and the decode matches.
