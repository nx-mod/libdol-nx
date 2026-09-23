# format/media

Video and the sound that goes with it.

| Format | Is |
|---|---|
| THP | Nintendo's video format: JPEG-like frames and an audio track |
| BNR1, BNR2, IMET | banners: what a title calls itself, and its picture |

The decoder takes a frame of bytes and gives back three planes; the guest-facing
version, which resolves a game's buffers and tells the renderer the textures
changed, is a native in `accel/sdk`.

`wiinx/format/media/thp.hpp` is the decoder. THP departs from ordinary JPEG in
three ways a stock decoder gets wrong, and the file says which: the
entropy-coded data has no byte stuffing, restart intervals carry no markers, and
the output is arranged as GX I8 tiles rather than rows.

## Banners

A launcher needs two things from a title: a name in the player's language, and
something to draw. `banner.hpp` reads both kinds - a disc's `opening.bnr`, which
carries a 96x32 picture and a block of text per language, and a channel's IMET
header, which carries ten names and leaves its art to a layout.

Checked against real files: Double Dash's banner (one language, a picture), New
Super Mario Bros. Wii's (ten names, no picture) and Mega Man 9's channel banner.
