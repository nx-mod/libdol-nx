# tools/src

The tools that are programs rather than scripts, because they read a lot of
bytes and the scripts would be slow at it.

| | |
|---|---|
| `wiinx_disc.cpp` | `wiinx-disc`: what a dump is, what is on it, its executable, one file, or the whole disc as a project's `sys/` and `files/` |

They build in a host build (`cmake -S . -B build`) and are left out of a Switch
one. Zstandard is used when it is there, which is what RVZ needs; without it
everything stored plainly still reads.
