# launcher

One NRO that lists what is installed and starts it.

A title - a game, a WiiWare title, a system title - builds into its own NRO at
`sdmc:/wii-nx/<kind>/<name>/<name>.nro`. Without this, reaching one means
finding it in the homebrew menu's file list, which says nothing about what any
of them is.

## What it does

Reads `games/`, `wads/` and `titles/` under `sdmc:/wii-nx`, and treats a folder
as a title when it holds an NRO named after itself. Up and down move, A starts,
B leaves.

Starting one is `envSetNextLoad`: it names the next NRO and returns, and the
loader runs it. Nothing of the launcher stays resident while a game plays -
there is no second process and no memory held - which is why a game gets the
same machine it would have got on its own.

## Build

It shares nothing with a game build: no Dawn, no Aurora, no translated code. So
it builds on its own, in seconds, whichever game is configured.

```sh
cmake -S src/launcher -B build/launcher -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
cmake --build build/launcher
```

`wii-nx-launcher.nro` goes anywhere on the card.

## Still to do

- Each title's own banner, which the disc or the title already carries, instead
  of its folder name.
- The console's own look, per front repo, rather than a list.
