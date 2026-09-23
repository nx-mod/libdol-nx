#pragma once

// Banners: what a title calls itself, and what it looks like on a menu.
//
// Two kinds, because the two eras did it differently:
//
//   BNR1, BNR2   a disc's `opening.bnr`. A 96x32 picture, then a block of text
//                per language - the game's name, who made it, and a sentence
//                about it. BNR1 carries one block, BNR2 six.
//   IMET         a channel's banner. Its art is a layout rather than a picture,
//                so what is readable here is the name, in up to ten languages.
//
// A launcher wants exactly this: something to write under a tile, in the
// language the player set.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::media {

// The languages a disc banner carries, in the order it carries them.
enum class BannerLanguage {
    Japanese,
    English,
    German,
    French,
    Spanish,
    Italian,
    Dutch,
};

// What a title says about itself, in one language.
struct BannerText {
    std::string short_name;   // "Mega Man 9"
    std::string long_name;    // the same, or a fuller form
    std::string maker;
    std::string long_maker;
    std::string description;
};

struct Banner {
    // The picture, when there is one: 96 by 32, as RGBA, eight bits a channel.
    // Empty for a channel, whose art is a layout rather than an image.
    std::vector<std::uint8_t> image;
    static constexpr std::uint32_t kImageWidth = 96;
    static constexpr std::uint32_t kImageHeight = 32;

    // One entry per language the banner carries. A disc banner's order is the
    // list above; a channel's is that list with Korean after it.
    std::vector<BannerText> text;

    // The name to show, given what the console's settings say. Falls back to
    // the first language the banner has rather than to nothing.
    const BannerText& Preferred(BannerLanguage language) const;
};

// Reads a disc's `opening.bnr` (BNR1 or BNR2).
std::optional<Banner> ReadDiscBanner(const std::uint8_t* data, std::size_t size);

// Reads a channel's banner: the IMET header a title's first content carries,
// wherever in the buffer it is.
std::optional<Banner> ReadChannelBanner(const std::uint8_t* data, std::size_t size);

// Either kind, whichever the bytes turn out to be.
std::optional<Banner> ReadBanner(const std::uint8_t* data, std::size_t size);

// RGB5A3, the way the hardware stores a small image: a pixel is sixteen bits,
// and the top one says whether the other fifteen are five bits a channel with
// no transparency, or four with three of alpha. Tiles are 4x4.
void DecodeRgb5A3(const std::uint8_t* tiles, std::uint32_t width, std::uint32_t height,
                  std::uint8_t* rgba);

}  // namespace wiinx::media
