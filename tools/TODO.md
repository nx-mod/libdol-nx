# TODO - tools

- [ ] One front end: `wiinx build`, `wiinx scan`, `wiinx sign`, `wiinx nand`,
      with today's scripts underneath. One command is what makes it read as a
      toolkit rather than a folder
- [ ] Read discs through `format/disc` instead of shelling out to Dolphin's
      tool, so a dump needs nothing installed to be used
- [ ] Install a WAD into a NAND, once `format/nand` can write one
- [ ] Generate the compatibility list the front repos show
- [ ] `wiinx-check-bindings`: a function's extent comes from the title's own
      map, so a map with missing starts (a scanned map, the Wii Menu's before
      2026-11-02) makes bodies too long and flags correct bindings as "shape".
      Settle a flag with the scanner's own hash first: on 2026-11-02, 23 of 26
      flags outside the Wii Menu matched their native's signature exactly - and
      "shared" flags a native and its Aurora-only twin (`GX__InitTexObj` beside
      accel's `GXInitTexObj`) that the native-GX build compiles out
