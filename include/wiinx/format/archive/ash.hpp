#pragma once

// ASH: Nintendo's other compression, and the one standing between us and the
// Wii Menu.
//
// Where Yaz0 and LZ11 are sliding windows with a byte or two per reference,
// ASH is Huffman coded: two trees, one over literals and lengths and one over
// distances, each read from its own bit stream in the same file. That makes it
// denser and slower, which is why it holds a menu's fonts and layouts rather
// than a game's level data.
//
// Two numbers are not in the file: how many bits a symbol takes and how many a
// distance takes. Every title Nintendo shipped uses 9 and 11 - except My
// Pokemon Ranch, which uses 15 for distances - so they are arguments with the
// usual answer as their default.
//
// Format and algorithm follow ASH0-tools by Garhoogin and NinjaCheetah
// (https://github.com/NinjaCheetah/ASH0-tools), MIT; see THIRD-PARTY-NOTICES.md.

#include <cstddef>
#include <cstdint>

namespace wiinx::archive {

inline constexpr std::size_t kAshHeaderSize = 0x0C;

// Whether these bytes are an ASH file.
bool IsAsh(const std::uint8_t* data, std::size_t size);

// How large it expands to, or 0 when it is not one. The size is 24 bits: the
// top byte of that word is not part of it.
std::uint32_t AshExpandedSize(const std::uint8_t* data, std::size_t size);

// Expands `src` into `dst`. Returns the bytes written - the declared size when
// it succeeds - or 0 when the file is malformed or `dst` is too small.
//
// `symbol_bits` and `distance_bits` are how wide the two trees' symbols are.
// The defaults are what every title uses bar one.
std::uint32_t AshDecompress(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                            std::size_t dst_size, int symbol_bits = 9, int distance_bits = 11);

}  // namespace wiinx::archive
