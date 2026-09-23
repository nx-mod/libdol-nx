// Yaz0, expanded.
//
// Reference: the format as described by EGG::Decomp in Kinoko
// (https://github.com/vabold/Kinoko, MIT), and the overrun reported in szsHaxx
// (https://github.com/vabold/szsHaxx).
#include "wiinx/format/archive/yaz0.hpp"

#include <cstring>

namespace wiinx::archive {
namespace {

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

std::uint32_t Yaz0ExpandedSize(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 8 || std::memcmp(data, "Yaz0", 4) != 0) {
        return 0;
    }
    return Read32(data + 4);
}

std::uint32_t Yaz0Decode(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                         std::size_t dst_size) {
    const std::uint32_t expanded = Yaz0ExpandedSize(src, src_size);
    if (expanded == 0 || dst == nullptr || dst_size < expanded || src_size < kYaz0HeaderSize) {
        return 0;
    }

    std::size_t read = kYaz0HeaderSize;
    std::uint32_t written = 0;
    std::uint32_t mask = 0;
    std::uint32_t flags = 0;

    // Every read of the stream goes through this, so a truncated file ends the
    // decode instead of running off the buffer.
    const auto next = [&]() -> int {
        return read < src_size ? src[read++] : -1;
    };

    while (written < expanded) {
        if (mask == 0) {
            const int group = next();
            if (group < 0) {
                return 0;
            }
            flags = static_cast<std::uint32_t>(group);
            mask = 0x80;
        }

        if ((flags & mask) != 0) {
            const int literal = next();
            if (literal < 0) {
                return 0;
            }
            dst[written++] = static_cast<std::uint8_t>(literal);
        } else {
            const int high = next();
            const int low = next();
            if (high < 0 || low < 0) {
                return 0;
            }
            const std::uint32_t reference =
                (static_cast<std::uint32_t>(high) << 8) | static_cast<std::uint32_t>(low);

            const std::uint32_t distance = (reference & 0xFFF) + 1;
            if (distance > written) {
                // Reaching back before the output would copy whatever happened
                // to be in front of the buffer.
                return 0;
            }

            std::uint32_t count = reference >> 12;
            if (count == 0) {
                const int extra = next();
                if (extra < 0) {
                    return 0;
                }
                count = static_cast<std::uint32_t>(extra) + 18;
            } else {
                count += 2;
            }
            if (written + count > expanded) {
                return 0;
            }

            // Runs overlap by design, so this copies forward a byte at a time.
            std::uint32_t from = written - distance;
            for (std::uint32_t index = 0; index < count; index++) {
                dst[written++] = dst[from++];
            }
        }

        mask >>= 1;
    }
    return written;
}

}  // namespace wiinx::archive
