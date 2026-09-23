// Reading a banner.
//
//   BNR1  "BNR1", 0x1C of nothing, a 96x32 RGB5A3 image, then one 0x140 block
//         of text: 0x20 of short name, 0x20 of maker, 0x40 of long name, 0x40
//         of long maker, 0x80 of description.
//   BNR2  the same, with six blocks - one per language a Wii disc offers.
//   IMET  a header a channel's banner content carries: "IMET", sizes, then ten
//         names of 42 characters each, in UTF-16.
//
// Text in a disc banner is in the console's own encoding rather than UTF-8:
// Shift-JIS for a Japanese disc, Latin-1 for the rest. Only the second is
// converted here, and a byte that is not representable is left out rather than
// guessed at.
#include "wiinx/format/media/banner.hpp"

#include <cstring>

namespace wiinx::media {
namespace {

constexpr std::size_t kTextBlock = 0x140;
constexpr std::size_t kImageAt = 0x20;
constexpr std::size_t kImageBytes = 96 * 32 * 2;
constexpr std::size_t kTextAt = kImageAt + kImageBytes;  // 0x1820

std::uint16_t Read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// A fixed-width field, stopping at its first zero.
std::string Field(const std::uint8_t* data, std::size_t size) {
    const auto* end = static_cast<const std::uint8_t*>(std::memchr(data, 0, size));
    const std::size_t length = end != nullptr ? static_cast<std::size_t>(end - data) : size;

    // Latin-1 to UTF-8, which covers every language a disc banner is written in
    // but Japanese.
    std::string text;
    text.reserve(length);
    for (std::size_t index = 0; index < length; index++) {
        const std::uint8_t byte = data[index];
        if (byte < 0x80) {
            text.push_back(static_cast<char>(byte));
        } else {
            text.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            text.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return text;
}

// A name in a channel's banner: UTF-16, big-endian, padded with zeros.
std::string Utf16(const std::uint8_t* data, std::size_t characters) {
    std::string text;
    for (std::size_t index = 0; index < characters; index++) {
        const std::uint16_t unit = Read16(data + index * 2);
        if (unit == 0) {
            break;
        }
        if (unit < 0x80) {
            text.push_back(static_cast<char>(unit));
        } else if (unit < 0x800) {
            text.push_back(static_cast<char>(0xC0 | (unit >> 6)));
            text.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        } else {
            text.push_back(static_cast<char>(0xE0 | (unit >> 12)));
            text.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        }
    }
    return text;
}

BannerText ReadTextBlock(const std::uint8_t* block) {
    BannerText text;
    text.short_name = Field(block + 0x00, 0x20);
    text.maker = Field(block + 0x20, 0x20);
    text.long_name = Field(block + 0x40, 0x40);
    text.long_maker = Field(block + 0x80, 0x40);
    text.description = Field(block + 0xC0, 0x80);
    return text;
}

}  // namespace

const BannerText& Banner::Preferred(BannerLanguage language) const {
    static const BannerText kNothing;
    if (text.empty()) {
        return kNothing;
    }
    const auto index = static_cast<std::size_t>(language);
    return index < text.size() ? text[index] : text.front();
}

void DecodeRgb5A3(const std::uint8_t* tiles, std::uint32_t width, std::uint32_t height,
                  std::uint8_t* rgba) {
    // Four by four pixels at a time, in rows of tiles.
    for (std::uint32_t y = 0; y < height; y++) {
        for (std::uint32_t x = 0; x < width; x++) {
            const std::uint32_t tile = (y / 4) * (width / 4) + (x / 4);
            const std::uint32_t within = (y % 4) * 4 + (x % 4);
            const std::uint16_t pixel = Read16(tiles + (tile * 16 + within) * 2);
            std::uint8_t* out = rgba + (y * width + x) * 4;

            if ((pixel & 0x8000) != 0) {
                // Five bits a channel, and no transparency.
                const std::uint8_t red = (pixel >> 10) & 0x1F;
                const std::uint8_t green = (pixel >> 5) & 0x1F;
                const std::uint8_t blue = pixel & 0x1F;
                out[0] = static_cast<std::uint8_t>((red << 3) | (red >> 2));
                out[1] = static_cast<std::uint8_t>((green << 3) | (green >> 2));
                out[2] = static_cast<std::uint8_t>((blue << 3) | (blue >> 2));
                out[3] = 0xFF;
            } else {
                // Four bits a channel, and three of alpha.
                const std::uint8_t alpha = (pixel >> 12) & 0x7;
                const std::uint8_t red = (pixel >> 8) & 0xF;
                const std::uint8_t green = (pixel >> 4) & 0xF;
                const std::uint8_t blue = pixel & 0xF;
                out[0] = static_cast<std::uint8_t>((red << 4) | red);
                out[1] = static_cast<std::uint8_t>((green << 4) | green);
                out[2] = static_cast<std::uint8_t>((blue << 4) | blue);
                out[3] = static_cast<std::uint8_t>((alpha << 5) | (alpha << 2) | (alpha >> 1));
            }
        }
    }
}

std::optional<Banner> ReadDiscBanner(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kTextAt + kTextBlock) {
        return std::nullopt;
    }
    const bool one = std::memcmp(data, "BNR1", 4) == 0;
    const bool two = std::memcmp(data, "BNR2", 4) == 0;
    if (!one && !two) {
        return std::nullopt;
    }

    const std::size_t blocks = one ? 1 : 6;
    if (size < kTextAt + blocks * kTextBlock) {
        return std::nullopt;
    }

    Banner banner;
    banner.image.resize(Banner::kImageWidth * Banner::kImageHeight * 4);
    DecodeRgb5A3(data + kImageAt, Banner::kImageWidth, Banner::kImageHeight,
                 banner.image.data());

    banner.text.reserve(blocks);
    for (std::size_t index = 0; index < blocks; index++) {
        banner.text.push_back(ReadTextBlock(data + kTextAt + index * kTextBlock));
    }
    return banner;
}

std::optional<Banner> ReadChannelBanner(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) {
        return std::nullopt;
    }

    // The header sits a little way into the content, and where exactly has
    // varied, so it is looked for rather than assumed.
    std::size_t at = 0;
    bool found = false;
    for (const std::size_t candidate : {std::size_t{0x40}, std::size_t{0x80}, std::size_t{0x00}}) {
        if (candidate + 0x600 <= size && std::memcmp(data + candidate, "IMET", 4) == 0) {
            at = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return std::nullopt;
    }

    // "IMET", two words of nothing, the sizes of the three files it introduces,
    // more nothing, and then the names.
    constexpr std::size_t kNamesAt = 0x1C;
    constexpr std::size_t kNameCharacters = 42;
    constexpr std::size_t kLanguages = 10;

    Banner banner;
    banner.text.reserve(kLanguages);
    for (std::size_t index = 0; index < kLanguages; index++) {
        const std::uint8_t* name = data + at + kNamesAt + index * kNameCharacters * 2;
        BannerText text;
        text.short_name = Utf16(name, kNameCharacters);
        text.long_name = text.short_name;
        banner.text.push_back(text);
    }

    // A banner with nothing in any language is not a banner.
    for (const BannerText& text : banner.text) {
        if (!text.short_name.empty()) {
            return banner;
        }
    }
    return std::nullopt;
}

std::optional<Banner> ReadBanner(const std::uint8_t* data, std::size_t size) {
    if (auto disc = ReadDiscBanner(data, size)) {
        return disc;
    }
    return ReadChannelBanner(data, size);
}

}  // namespace wiinx::media
