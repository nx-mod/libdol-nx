# platform/audio

Sound: the DSP's mixer, and the samples that come out of it.

| File | Is |
|---|---|
| `ax_mix.cpp`, `ax_mix_kernels.h` | AX's voices and their mixing |
| `ax_effects.cpp` | reverb and the rest of AXFX |
| `ax_memory.cpp` | where voices read their samples from |
| `audio.cpp`, `audio_backend.cpp` | the interface that plays the result on a Switch |

Both consoles have the same DSP. What the GameCube has in addition - voices
reading out of ARAM, and disc-streamed audio - belongs to
[libgc-nx](https://github.com/nx-mod/libgc-nx).
