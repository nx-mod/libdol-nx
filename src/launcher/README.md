# launcher

One NRO that lists what is installed and starts it.

This is the fallback, not the front door. The Wii Menu itself is what should
list games and channels, with each one's own banner; this stays because it works
with no NAND, no translated code and no graphics stack, which makes it the thing
that still runs when none of that does.

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

Little, deliberately. It is meant to stay small enough to be trustworthy:
the Wii Menu is where tiles, banners and the console's own look belong. What
would earn its place here is only what helps when something else is broken -
saying why a title cannot start, for instance, rather than only that it did
not.
