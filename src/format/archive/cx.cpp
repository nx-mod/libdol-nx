// Expanding a CX stream.
//
// Both sliding-window kinds work the same way: a flag byte, then eight items,
// each either a literal byte or a reference back into what has been written.
// What differs is how a reference is spelled. LZ77 always spends two bytes on
// one - four bits of length and twelve of distance. LZ11 spends two, three or
// four, so that a long run costs no more to describe than a short one does.
//
// Runs overlap by design, so every copy goes forward one byte at a time.
#include "wiinx/format/archive/cx.hpp"

#include <cstring>

namespace wiinx::archive {
namespace {

std::uint32_t Read24(const std::uint8_t* p) {
    // The size is little-endian, unlike everything else these consoles write.
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16);
}

std::uint32_t Read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Where the compressed bytes begin: after the header, and after the longer size
// when the header's own three bytes were not enough.
std::size_t PayloadAt(const std::uint8_t* data) { return Read24(data + 1) == 0 ? 8 : 4; }

}  // namespace

CxKind CxIdentify(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return CxKind::None;
    }
    switch (data[0]) {
        case 0x10: return CxKind::Lz10;
        case 0x11: return CxKind::Lz11;
        case 0x20: return CxKind::Huffman4;
        case 0x28: return CxKind::Huffman8;
        case 0x30: return CxKind::Rle;
        default: return CxKind::None;
    }
}

std::uint32_t CxExpandedSize(const std::uint8_t* data, std::size_t size) {
    if (CxIdentify(data, size) == CxKind::None) {
        return 0;
    }
    const std::uint32_t small = Read24(data + 1);
    if (small != 0) {
        return small;
    }
    return size >= 8 ? Read32(data + 4) : 0;
}

std::uint32_t CxDecompress(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                           std::size_t dst_size) {
    const CxKind kind = CxIdentify(src, src_size);
    if (kind != CxKind::Lz10 && kind != CxKind::Lz11) {
        return 0;
    }
    const std::uint32_t expanded = CxExpandedSize(src, src_size);
    if (expanded == 0 || dst == nullptr || dst_size < expanded) {
        return 0;
    }

    std::size_t read = PayloadAt(src);
    std::uint32_t written = 0;
    std::uint32_t flags = 0;
    int left = 0;

    // Every read goes through this, so a stream that stops early ends the
    // expansion instead of running off the buffer.
    const auto next = [&]() -> int { return read < src_size ? src[read++] : -1; };

    while (written < expanded) {
        if (left == 0) {
            const int group = next();
            if (group < 0) {
                return 0;
            }
            flags = static_cast<std::uint32_t>(group);
            left = 8;
        }
        const bool reference = (flags & 0x80) != 0;
        flags = (flags << 1) & 0xFF;
        left--;

        if (!reference) {
            const int literal = next();
            if (literal < 0) {
                return 0;
            }
            dst[written++] = static_cast<std::uint8_t>(literal);
            continue;
        }

        const int first = next();
        if (first < 0) {
            return 0;
        }

        std::uint32_t length = 0;
        std::uint32_t distance = 0;
        if (kind == CxKind::Lz10) {
            const int second = next();
            if (second < 0) {
                return 0;
            }
            length = static_cast<std::uint32_t>(first >> 4) + 3;
            distance = ((static_cast<std::uint32_t>(first & 0xF) << 8) |
                        static_cast<std::uint32_t>(second)) + 1;
        } else {
            // LZ11 spends more bytes on a longer run, and says which by the top
            // nibble of the first one.
            const std::uint32_t indicator = static_cast<std::uint32_t>(first) >> 4;
            const int second = next();
            if (second < 0) {
                return 0;
            }
            if (indicator == 0) {
                const int third = next();
                if (third < 0) {
                    return 0;
                }
                length = ((static_cast<std::uint32_t>(first) << 4) |
                          (static_cast<std::uint32_t>(second) >> 4)) + 0x11;
                distance = ((static_cast<std::uint32_t>(second & 0xF) << 8) |
                            static_cast<std::uint32_t>(third)) + 1;
            } else if (indicator == 1) {
                const int third = next();
                const int fourth = next();
                if (third < 0 || fourth < 0) {
                    return 0;
                }
                length = ((static_cast<std::uint32_t>(first & 0xF) << 12) |
                          (static_cast<std::uint32_t>(second) << 4) |
                          (static_cast<std::uint32_t>(third) >> 4)) + 0x111;
                distance = ((static_cast<std::uint32_t>(third & 0xF) << 8) |
                            static_cast<std::uint32_t>(fourth)) + 1;
            } else {
                length = indicator + 1;
                distance = ((static_cast<std::uint32_t>(first & 0xF) << 8) |
                            static_cast<std::uint32_t>(second)) + 1;
            }
        }

        if (distance > written || written + length > expanded) {
            // A reference before the output, or a run past the declared size.
            return 0;
        }
        std::uint32_t from = written - distance;
        for (std::uint32_t index = 0; index < length; index++) {
            dst[written++] = dst[from++];
        }
    }
    return written;
}

}  // namespace wiinx::archive
