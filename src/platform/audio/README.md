# platform/audio

Sound: the DSP's mixer, and the samples that come out of it.

| File | Is |
|---|---|
| `ax_mix.cpp`, `ax_mix_kernels.h` | AX's voices and their mixing |
| `ax_effects.cpp` | reverb and the rest of AXFX |
| `ax_memory.cpp` | where voices read their samples from |
| `audio.cpp`, `audio_backend.cpp` | the interface that plays the result on a Switch |

## Threads

- **The game thread** runs the guest's AI callback (`Audio_HLE_Tick`, from the
  idle loop): one AX frame per 3 ms block, the game's own sound code and AXFX
  included. In a slow race that is ~24 frames back to back, ~13 ms.
- **The mix worker** (core 1, priority 0x2D) runs the DSP's command list; the
  game thread joins it before reading what it wrote.
- **The output thread** (core 1, Switch) takes converted blocks from a queue
  and hands them to audren; `audrvUpdate` is an IPC call per block.

Both consoles have the same DSP. What the GameCube has in addition - voices
reading out of ARAM, and disc-streamed audio - belongs to
[libgc-nx](https://github.com/nx-mod/libgc-nx).
