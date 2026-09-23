#pragma once

// THP: the format Nintendo's movies are in.
//
// Baseline-JPEG frames at 4:2:0, decoded into three 8-bit planes. It departs
// from ordinary JPEG in ways a stock decoder gets wrong - no byte stuffing in
// the entropy-coded data, restart intervals without markers, and an output
// arranged as GX I8 tiles rather than rows - so it has a decoder of its own.
//
// Bytes in, planes out. Where those bytes come from, and what is done with the
// planes, is the caller's: a game's video native passes guest memory, a tool
// passes a file.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace wiinx::media {

// The SDK's own error codes, so a caller that checks them behaves the same.
inline constexpr std::int32_t kThpOk = 0;
inline constexpr std::int32_t kThpBadSyntax = 3;
inline constexpr std::int32_t kThpBadPrecision = 10;
inline constexpr std::int32_t kThpUnsupportedMarker = 11;
inline constexpr std::int32_t kThpBadComponents = 12;
inline constexpr std::int32_t kThpMissingHuffman = 15;
inline constexpr std::int32_t kThpBadSampling = 19;
inline constexpr std::int32_t kThpNoInput = 25;
inline constexpr std::int32_t kThpNoWork = 26;
inline constexpr std::int32_t kThpNoOutput = 27;

// One frame, decoded in two steps: the headers say how large the planes must
// be, then the scan is decoded into them.
//
//     ThpDecoder decoder{bytes, size};
//     if (decoder.ParseHeaders() != kThpOk) { ... }
//     decoder.Decode(luma, chromaU, chromaV);
//
// `luma` is Width() * Height() bytes; each chroma plane is a quarter of that.
// The frame's length is not in its header, so `size` is however much of the
// buffer is readable - the decoder never reads past it.
class ThpDecoder {
  public:
    ThpDecoder(const std::uint8_t* data, std::size_t size);
    ~ThpDecoder();
    ThpDecoder(ThpDecoder&&) noexcept;
    ThpDecoder& operator=(ThpDecoder&&) noexcept;

    std::int32_t ParseHeaders();
    std::uint16_t Width() const;
    std::uint16_t Height() const;

    std::int32_t Decode(std::uint8_t* luma, std::uint8_t* chromaU, std::uint8_t* chromaV);

  private:
    struct State;
    std::unique_ptr<State> mState;
};

}  // namespace wiinx::media
