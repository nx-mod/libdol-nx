// Banners: one of each kind built here, read back.
#include "wiinx/format/media/banner.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

void PutText(std::vector<std::uint8_t>& out, std::size_t at, const std::string& text) {
    std::memcpy(out.data() + at, text.data(), text.size());
}

void PutUtf16(std::vector<std::uint8_t>& out, std::size_t at, const std::string& text) {
    for (std::size_t index = 0; index < text.size(); index++) {
        out[at + index * 2] = 0;
        out[at + index * 2 + 1] = static_cast<std::uint8_t>(text[index]);
    }
}

// A disc's banner: the magic, a picture, and a block of text per language.
std::vector<std::uint8_t> MakeDiscBanner(const char* magic, std::size_t languages) {
    constexpr std::size_t kTextAt = 0x1820;
    std::vector<std::uint8_t> banner(kTextAt + languages * 0x140, 0);
    std::memcpy(banner.data(), magic, 4);

    // A picture that is one flat colour, so the decode can be checked: the top
    // bit set means five bits a channel, and 0xFC00 is red at full.
    for (std::size_t at = 0x20; at < kTextAt; at += 2) {
        banner[at] = 0xFC;
        banner[at + 1] = 0x00;
    }

    for (std::size_t index = 0; index < languages; index++) {
        const std::size_t block = kTextAt + index * 0x140;
        PutText(banner, block + 0x00, "Short " + std::to_string(index));
        PutText(banner, block + 0x20, "Maker");
        PutText(banner, block + 0x40, "The Long Name " + std::to_string(index));
        PutText(banner, block + 0x80, "The Long Maker");
        PutText(banner, block + 0xC0, "What the game is about.");
    }
    return banner;
}

// A channel's: the IMET header, and ten names.
std::vector<std::uint8_t> MakeChannelBanner() {
    std::vector<std::uint8_t> banner(0x700, 0);
    std::memcpy(banner.data() + 0x40, "IMET", 4);
    const char* names[] = {"Nihongo", "The Channel", "Der Kanal", "La Chaine",
                           "El Canal", "Il Canale", "Het Kanaal", "", "", "Korean"};
    for (std::size_t index = 0; index < 10; index++) {
        PutUtf16(banner, 0x40 + 0x1C + index * 84, names[index]);
    }
    return banner;
}
}  // namespace

int main() {
    using namespace wiinx::media;
    std::printf("wiinx::media banner check\n\n");

    {   // A GameCube disc: one language.
        const auto bytes = MakeDiscBanner("BNR1", 1);
        const auto banner = ReadBanner(bytes.data(), bytes.size());
        Check("a BNR1 banner reads", banner.has_value());
        if (banner) {
            Check("  one language", banner->text.size() == 1);
            Check("  its short name", banner->text[0].short_name == "Short 0");
            Check("  its long name", banner->text[0].long_name == "The Long Name 0");
            Check("  its maker", banner->text[0].maker == "Maker");
            Check("  its description",
                  banner->text[0].description == "What the game is about.");
            Check("  a picture of the right size",
                  banner->image.size() == 96u * 32u * 4u);
            Check("  decoded from RGB5A3",
                  banner->image[0] == 0xFF && banner->image[1] == 0x00 &&
                      banner->image[2] == 0x00 && banner->image[3] == 0xFF);
        }
    }

    {   // A Wii disc: six.
        const auto bytes = MakeDiscBanner("BNR2", 6);
        const auto banner = ReadBanner(bytes.data(), bytes.size());
        Check("a BNR2 banner reads", banner.has_value());
        if (banner) {
            Check("  six languages", banner->text.size() == 6);
            Check("  the one the player set",
                  banner->Preferred(BannerLanguage::French).short_name == "Short 3");
            Check("  and a language it does not have falls back",
                  banner->Preferred(BannerLanguage::Dutch).short_name == "Short 0");
        }
    }

    {   // A channel.
        const auto bytes = MakeChannelBanner();
        const auto banner = ReadBanner(bytes.data(), bytes.size());
        Check("a channel banner reads", banner.has_value());
        if (banner) {
            Check("  ten languages", banner->text.size() == 10);
            Check("  the English name",
                  banner->Preferred(BannerLanguage::English).short_name == "The Channel");
            Check("  and another",
                  banner->Preferred(BannerLanguage::Italian).short_name == "Il Canale");
            Check("  no picture, because its art is a layout", banner->image.empty());
        }
    }

    {   // Things that are not banners.
        const std::vector<std::uint8_t> junk(0x2000, 0x5A);
        Check("something else is refused", !ReadBanner(junk.data(), junk.size()).has_value());

        const auto cut = MakeDiscBanner("BNR2", 6);
        Check("a banner cut short is refused", !ReadBanner(cut.data(), 0x100).has_value());

        std::vector<std::uint8_t> empty(0x700, 0);
        std::memcpy(empty.data() + 0x40, "IMET", 4);
        Check("a channel banner with no name in any language is refused",
              !ReadBanner(empty.data(), empty.size()).has_value());
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
