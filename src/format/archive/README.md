# format/archive

What games pack their files in.

| Format | Is |
|---|---|
| Yaz0 (`SZS`) | the compression every EAD game stores its archives in |
| U8 (`ARC`) | a directory of files in one blob, which the SDK and the channels both use |
| CX (`LZ77`, `LZ11`) | what Nintendo's own library compresses with - including, often, a game's executable |

The algorithms live here; the guest-facing versions - `EGG::Decomp::decodeSZS`,
`ARCOpen` - are natives in `accel` that resolve a game's buffers and call them.

`wiinx/format/archive/yaz0.hpp` expands a Yaz0 stream. It refuses what the
original trusts: a reference pointing before the output, a run past the declared
size, or a stream that stops in the middle. `archive_yaz0_check` covers each of
those.

## Why CX matters more than it looks

A WiiWare title keeps its code as a compressed content and expands it while
loading. Mega Man 9 is the example: the content its TMD boots is a 312 KB
loader, and the game itself is 1.4 MB of LZ11 that expands to a 2.87 MB
executable - which then reads as an ordinary Wii game, RVL SDK banners and all.

Without this, such a title looks like a loader and a pile of unreadable blobs.
