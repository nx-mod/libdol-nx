# data

What the library knows about games, as facts rather than code.

| | |
|---|---|
| `builds.json` | every library build seen in a game, and which games named it. `tools/wiinx-scan record` adds to it; `wiinx-scan header` turns it into `include/wiinx/core/builds.hpp` |
| `signatures.json` | the verified set that binds natives: masked hashes of the functions a native replaces, per build |
| `symbols.json.gz` | names by code, signed from games whose symbols are known. Hashes and names only |
| `dsp/` | the DSP's own microcode hashes, for recognising which one a game uploads |
| `wii/` | the empty console: what a NAND has before anything is installed |

**No game code or data is here, and none ever should be.** A signature is a
hash, a build is a date, a name is a string. Anything read off a disc stays on
the machine of whoever owns the disc.
