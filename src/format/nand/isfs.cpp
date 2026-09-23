#include "wiinx/format/nand/isfs.hpp"

#include <cstdio>

namespace wiinx::nand {
namespace {

std::string Hex8(std::uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

// The keystream `setting.txt` is stored with: one fixed seed, rotated left by
// one bit after every byte.
constexpr std::uint32_t kSettingSeed = 0x73B5DBFAu;

}  // namespace

std::string TitleId::HighHex() const { return Hex8(High()); }
std::string TitleId::LowHex() const { return Hex8(Low()); }

std::string TitleId::Code() const {
    const std::uint32_t low = Low();
    std::string code(4, '\0');
    for (int index = 0; index < 4; index++) {
        const auto character = static_cast<unsigned char>(low >> (24 - index * 8));
        if (character < 0x20 || character > 0x7E) {
            return {};
        }
        code[static_cast<std::size_t>(index)] = static_cast<char>(character);
    }
    return code;
}

std::string TitlePath(TitleId id) {
    return "/title/" + id.HighHex() + "/" + id.LowHex();
}

std::string ContentPath(TitleId id) { return TitlePath(id) + "/content"; }

std::string DataPath(TitleId id) { return TitlePath(id) + "/data"; }

std::string TmdPath(TitleId id) { return ContentPath(id) + "/title.tmd"; }

std::string ContentFilePath(TitleId id, std::uint32_t content_id) {
    return ContentPath(id) + "/" + Hex8(content_id) + ".app";
}

std::string TicketPath(TitleId id) {
    return "/ticket/" + id.HighHex() + "/" + id.LowHex() + ".tik";
}

std::vector<std::uint8_t> ScrambleSetting(std::string_view text) {
    std::vector<std::uint8_t> file(text.begin(), text.end());
    std::uint32_t key = kSettingSeed;
    for (std::uint8_t& byte : file) {
        byte ^= static_cast<std::uint8_t>(key & 0xFF);
        key = (key << 1) | (key >> 31);
    }
    return file;
}

std::string UnscrambleSetting(const std::vector<std::uint8_t>& file) {
    std::string text;
    text.reserve(file.size());
    std::uint32_t key = kSettingSeed;
    for (const std::uint8_t byte : file) {
        text.push_back(static_cast<char>(byte ^ static_cast<std::uint8_t>(key & 0xFF)));
        key = (key << 1) | (key >> 31);
    }
    // The real file is padded with whatever was in the block; settings end at
    // the first byte that is not text.
    const std::size_t end = text.find('\0');
    if (end != std::string::npos) {
        text.resize(end);
    }
    return text;
}

}  // namespace wiinx::nand
