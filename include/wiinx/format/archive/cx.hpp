#pragma once

// CX: the compression Nintendo's own library applies, and the SDK expands.
//
// A file compressed this way says so in its first byte - the kind in the high
// nibble, and how long it expands to in the three bytes after it. Four kinds
// exist; the two that matter are the sliding-window ones, which is what a game
// packs its code and its archives with.
//
//   0x10  LZ77, the older form: a run is at most 18 bytes
//   0x11  LZ11, the same idea with three lengths of run, up to 0x10110 bytes
//   0x30  run-length
//   0x20  Huffman (4-bit), 0x28 Huffman (8-bit) - not expanded here
//
// A game's own executable is often inside one: a WiiWare title keeps its code
// as a compressed content and the loader expands it before jumping in.
//
// As everywhere in this section, a malformed stream is an answer rather than a
// crash: nothing is read or written outside the buffers given.

#include <cstddef>
#include <cstdint>

namespace wiinx::archive {

enum class CxKind : std::uint8_t {
    None = 0,
    Lz10 = 0x10,
    Lz11 = 0x11,
    Huffman4 = 0x20,
    Huffman8 = 0x28,
    Rle = 0x30,
};

// What the first bytes say this is, or None when they say nothing.
CxKind CxIdentify(const std::uint8_t* data, std::size_t size);

// How large it expands to, or 0 when it is not a CX stream. A stream longer
// than 16 MB carries its size in four bytes after the header instead of three.
std::uint32_t CxExpandedSize(const std::uint8_t* data, std::size_t size);

// Expands `src` into `dst`. Returns the bytes written - the declared size when
// it succeeds - or 0 when the stream is malformed, the kind is one this does
// not expand, or `dst` is too small.
std::uint32_t CxDecompress(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                           std::size_t dst_size);

}  // namespace wiinx::archive
