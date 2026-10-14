# TODO - platform/audio

- [ ] The guest's AI callback off the game thread, onto core 1: needs
      `OSDisableInterrupts` as a real lock and the callback's wakeups handed to
      the game thread's scheduler (~13 ms of a race frame)
- [ ] A native AXFX reverb (`AXFXReverbStdExpCallback`, the callback's
      heaviest part)
- [ ] AXFX beyond reverb
- [ ] ARAM voices and disc-streamed audio, for a GameCube game (in libgc-nx)
