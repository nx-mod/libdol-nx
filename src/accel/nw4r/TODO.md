# TODO - accel/nw4r

- [ ] `g3d`: `CalcWorld` and `CalcView` first - they are the frame's largest
      middleware cost and nothing here replaces them yet
- [ ] `snd`: the mixer's inner loops
- [ ] `lyt`: the rest of the pane walk, and text drawing
- [x] `lyt` animation: the whole walk, 2007/2008/2009 (lyt/README.md). Not
      why Bomberman Blast's Characters menu is slow (2-8% there, the same share
      as in its menus that run at 60): that one is its thousands of GX draws
      (TODO.md, speed)
- [ ] `math`: the vector and matrix helpers the rest of nw4r calls
