# format/media

Video and the sound that goes with it.

| Format | Is |
|---|---|
| THP | Nintendo's video format: JPEG-like frames and an audio track |

The decoder itself belongs here, taking a frame of bytes and giving back pixels;
the guest-facing version, which reads a game's buffers and writes its textures,
is a native in `accel/sdk`.

*The decoder is in `accel/sdk/thp` and moves here.*
