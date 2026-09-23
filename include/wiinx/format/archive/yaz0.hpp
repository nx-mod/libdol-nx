#pragma once

// Yaz0 ("SZS"): the compression Nintendo's games store their archives in.
//
// A header, then groups of eight codes, each code either a literal byte or a
// reference back into what has been written already. Runs may overlap - a
// distance of one repeats a byte - so the copy goes forward, one byte at a
// time.
//
// The original decoder trusts its input. This one does not: a reference
// pointing before the output, or a run that would write past the size the
// header declares, stops the decode rather than reading or writing memory that
// is not the caller's.

#include <cstddef>
#include <cstdint>

namespace wiinx::archive {

inline constexpr std::size_t kYaz0HeaderSize = 16;

// The expanded size a Yaz0 stream declares, or 0 when the bytes are not one.
// `size` may be as little as the header.
std::uint32_t Yaz0ExpandedSize(const std::uint8_t* data, std::size_t size);

// Expands `src` into `dst`. Returns the number of bytes written, which is the
// declared size when it succeeds and 0 when the stream is malformed or either
// buffer is too small.
//
// A compressed stream does not say how long it is, so `src_size` is however
// much of the buffer is readable; the decoder never reads past it.
std::uint32_t Yaz0Decode(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                         std::size_t dst_size);

}  // namespace wiinx::archive
