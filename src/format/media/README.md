# format/media

Video and the sound that goes with it.

| Format | Is |
|---|---|
| THP | Nintendo's video format: JPEG-like frames and an audio track |

The decoder takes a frame of bytes and gives back three planes; the guest-facing
version, which resolves a game's buffers and tells the renderer the textures
changed, is a native in `accel/sdk`.

`wiinx/format/media/thp.hpp` is the decoder. THP departs from ordinary JPEG in
three ways a stock decoder gets wrong, and the file says which: the
entropy-coded data has no byte stuffing, restart intervals carry no markers, and
the output is arranged as GX I8 tiles rather than rows.
