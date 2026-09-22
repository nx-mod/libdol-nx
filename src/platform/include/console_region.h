#pragma once

// Which Wii the guest sees. A real console has one region; wii-nx shares one
// NAND between games of any region, so the region-dependent system settings are
// derived per launch from the disc's own game ID rather than fixed at build time.
//
// The ID's fourth character is the region: J Japan, E/N NTSC-U, K/Q/T Korea, and
// the rest (P, D, F, S, I, H, R, U, X, Y, Z...) are PAL. The area/video/game/code
// values are Dolphin's (Boot_BS2Emu.cpp's region_settings), so a NAND written
// here and one written by Dolphin agree.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "runtime_config.h"

namespace ConsoleRegion {

struct Info {
    char letter;         // the disc ID's region character
    const char* area;    // setting.txt AREA, and MODEL's RVL-001(<area>)
    const char* video;   // setting.txt VIDEO
    const char* game;    // setting.txt GAME
    const char* code;    // setting.txt CODE
    bool pal;            // 50 Hz console: PAL60 applies to it alone
    uint8_t language;    // SYSCONF IPL.LNG: 0 Japanese, 1 English, 9 Korean
};

inline constexpr Info kJapan{'J', "JPN", "NTSC", "JP", "LJH", false, 0};
inline constexpr Info kAmerica{'E', "USA", "NTSC", "US", "LU", false, 1};
inline constexpr Info kEurope{'P', "EUR", "PAL", "EU", "LEH", true, 1};
inline constexpr Info kKorea{'K', "KOR", "NTSC", "KR", "LKH", false, 9};

inline const Info& FromLetter(char letter) noexcept {
    switch (letter) {
        case 'J':
            return kJapan;
        case 'E':
        case 'N':
            return kAmerica;
        case 'K':
        case 'Q':
        case 'T':
            return kKorea;
        default:
            return kEurope;
    }
}

// The game ID from the disc's own boot.bin ("SMNE01"), or empty when the disc
// is not readable yet (the region then falls back to PAL).
inline std::string DiscGameId() noexcept {
    const std::filesystem::path root = RuntimeConfigFile::ResolvedDvdRoot();
    if (root.empty()) {
        return {};
    }
    const std::filesystem::path boot = root / "sys" / "boot.bin";
    std::array<char, 7> id{};
#if defined(_WIN32)
    FILE* file = nullptr;
    ::fopen_s(&file, boot.string().c_str(), "rb");
#else
    FILE* file = std::fopen(boot.c_str(), "rb");
#endif
    if (file == nullptr) {
        return {};
    }
    const size_t read = std::fread(id.data(), 1, 6, file);
    std::fclose(file);
    if (read != 6) {
        return {};
    }
    for (size_t index = 0; index < 6; index++) {
        if (id[index] < 0x20 || id[index] > 0x7E) {
            return {};
        }
    }
    return std::string(id.data(), 6);
}

// Resolved once: the disc does not change while the game runs.
inline const Info& Active() noexcept {
    static const Info& active = [] () -> const Info& {
        const std::string id = DiscGameId();
        return FromLetter(id.size() >= 4 ? id[3] : 'P');
    }();
    return active;
}

}  // namespace ConsoleRegion
