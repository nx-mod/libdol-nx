# format/archive

What games pack their files in.

| Format | Is |
|---|---|
| Yaz0 (`SZS`) | the run-length compression every EAD game stores its archives in |
| U8 (`ARC`) | a directory of files in one blob, which the SDK and the channels both use |

The algorithms live here; the guest-facing versions - `EGG::Decomp::decodeSZS`,
`ARCOpen` - are natives in `accel` that resolve a game's buffers and call them.

`wiinx/format/archive/yaz0.hpp` expands a Yaz0 stream. It refuses what the
original trusts: a reference pointing before the output, a run past the declared
size, or a stream that stops in the middle. `archive_yaz0_check` covers each of
those.

*U8 is to write.*
