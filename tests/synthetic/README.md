# synthetic

A game with no Nintendo code: two hand-encoded PowerPC functions (a call, a
`ctr` loop, loads, stores, a float add). It is the one game anyone can build,
so it is how the whole path is checked without a disc.

```sh
tests/synthetic/make-game /tmp/synthetic
tools/wiinx-translate /tmp/synthetic
cmake -S cmake/game -B /tmp/synthetic/build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DWIINX_GAME_DIR=/tmp/synthetic
cmake --build /tmp/synthetic/build --target synthetic_nro
```

`ctest` runs the translation when `dotnet` is installed; the `game` workflow
builds the NRO.
