# TODO - platform/audio

- [ ] The guest's AI callback off the game thread, onto core 1: needs
      `OSDisableInterrupts` as a real lock and the callback's wakeups handed to
      the game thread's scheduler (~13 ms of a race frame)
- [x] A native AXFX reverb: `AXFXReverbStdExpCallback` (Mario Kart Wii) and
      `AXFXReverbHiCallback` (2026-11-02, read from Bomberman Blast, where it was a
      third of the game's time; signed from it, so any title linking it binds it)
- [ ] A check for `AXFXReverbHiCallback` on fake guest memory (tests/), and a
      comparison against iChris4/Wiicompiled_VR's `AXFXReverbHiExpCallback`
- [ ] AXFX beyond reverb
- [ ] ARAM voices and disc-streamed audio, for a GameCube game (in libgc-nx)
