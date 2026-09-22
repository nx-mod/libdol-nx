# Builds

Nintendo's libraries register a banner at startup, and every executable carries
one per library it links:

```
<< NW4R    - LYT 	final   build: Mar  8 2008 20:59:41 (0x4201_127) >>
<< RVL_SDK - THP 	release build: Aug  8 2007 01:31:54 (0x4199_60831) >>
```

libwii-nx names a build by its library and banner date: `nw4r.lyt@2008-03-08`,
`rvl.thp@2007-08-08`. The hex code (`0x4201_127`) is the SDK release the library
was built against; the date is what separates builds of one library.

## Where builds are kept

- `data/builds.json` - every build seen, with the games it was seen in. Add a
  game with `tools/wiinx-scan record <game id> <dol>...`.
- `include/wiinx/builds.hpp` - generated from it by `tools/wiinx-scan header`:
  each build as a `LibVersion`, e.g. `wiinx::builds::kNw4rLyt_2008_03_08`.
  Never edit it by hand.

A build no executable here shows can still be added by hand with its source,
as the four 2007 nw4r builds of Wii Sports are, from ogws's `NW4R_LIB_VERSION`.

Today: 224 builds - from six games' executables (Mario Kart Wii, New Super
Mario Bros. Wii, Super Paper Mario, Metroid Prime 3, Pikmin 2, Punch-Out!!),
the Wii Sports + Resort pack's launcher, Wii Sports by way of ogws, the System
Menu, IOS21 and nine channels.

## Using a build

- A native names the build it matches: see [natives](natives.md).
- Platform code whose behavior depends on a build asks the host-detected set:

```cpp
if (wiinx::detected("rvl.os") == &wiinx::builds::kRvlOs_2008_01_28) { ... }
```

## What a banner does not tell you

The same banner date does not promise the same code. Pikmin 2 (New Play
Control) links `rvl.thp@2007-08-08`, as Mario Kart Wii does, yet its
`THPVideoDecode` is compiled differently and does not match Mario Kart Wii's
signature. Binding always checks the code as well: see
[signatures](signatures.md).
