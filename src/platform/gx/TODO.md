# TODO - platform/gx

- [ ] Display-list decode costs 12.6 ms a frame, the largest single cost in a
      frame. Decoding is repeated for lists a game replays unchanged
- [ ] Decode a buffer with no guest address behind it, for homebrew
- [ ] A golden check: a recorded FIFO in, a known draw list out, so a change to
      the decoder is provable without a Switch
