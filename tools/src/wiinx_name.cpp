// What a title calls itself, in its own words.
//
//   wiinx-name <file>...
//
// A disc carries opening.bnr and a channel carries an IMET header, and both
// hold the name the console puts on screen - in up to ten languages, the maker
// alongside it. That is the name to show, rather than a folder ("megaman9-nx")
// or the internal one the disc header holds, which runs the words together
// ("MegaMan9").
//
// Files are read whole and nothing is written. A file that is not a banner is
// reported as such and the next one is tried.

#include "wiinx/format/media/banner.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

const char* LanguageName(std::size_t index) {
    static const char* kNames[] = {"Japanese", "English", "German",  "French",
                                   "Spanish",  "Italian", "Dutch",   "Korean"};
    return index < std::size(kNames) ? kNames[index] : "?";
}

int Report(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    if (bytes.empty()) {
        std::fprintf(stderr, "%s: cannot read\n", path.c_str());
        return 1;
    }
    const auto banner = wiinx::media::ReadBanner(bytes.data(), bytes.size());
    if (!banner) {
        std::fprintf(stderr, "%s: not a disc banner or a channel banner\n", path.c_str());
        return 1;
    }

    // English is what the launcher shows; the rest are printed so a title whose
    // English entry is empty can be seen to have one at all.
    const auto& preferred = banner->Preferred(wiinx::media::BannerLanguage::English);
    std::printf("%s\n", path.c_str());
    std::printf("  name   %s\n", preferred.short_name.c_str());
    if (!preferred.long_name.empty() && preferred.long_name != preferred.short_name) {
        std::printf("  long   %s\n", preferred.long_name.c_str());
    }
    if (!preferred.maker.empty()) {
        std::printf("  maker  %s\n", preferred.maker.c_str());
    }
    if (!banner->image.empty()) {
        std::printf("  image  %u x %u\n", wiinx::media::Banner::kImageWidth,
                    wiinx::media::Banner::kImageHeight);
    }
    for (std::size_t index = 0; index < banner->text.size(); ++index) {
        const auto& text = banner->text[index];
        if (!text.short_name.empty()) {
            std::printf("  %-9s %s\n", LanguageName(index), text.short_name.c_str());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("wiinx-name <file>...   the name a disc or channel banner carries\n");
        return 2;
    }
    int worst = 0;
    for (int index = 1; index < argc; ++index) {
        worst = std::max(worst, Report(argv[index]));
    }
    return worst;
}
