#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace RuntimeProduct {

// A value a product wants in guest low memory before the game boots, for
// something in the game or its mod that reads it back.
struct BootMarker {
    uint32_t address;
    uint32_t value;
    const char* label;
};

struct Descriptor {
    // What this build calls itself in logs and messages.
    std::string_view displayName;
    // The same, as a folder name: what its logs and saves sit under.
    std::string_view folderName = "base";
    // A product that overlays the disc with a mod's own files (Riivolution
    // replacements, patched modules), rather than running the disc as it is.
    bool overlaysDisc = false;
    // The Config.toml [paths] key naming where those files are, when it has
    // one, so the runtime can say which setting is missing.
    std::string_view fileRootSetting{};
    std::span<const BootMarker> bootMarkers{};
};

// Each public executable links exactly one small provider definition - the
// base game's is in the library, a mod's comes with the game it belongs to.
// Keeping the choice out of target-wide preprocessor definitions lets the
// runtime be compiled once and shared by every product.
const Descriptor& Active() noexcept;

// True in a build that replaces the disc's files with a mod's.
inline bool OverlaysDisc() noexcept { return Active().overlaysDisc; }

} // namespace RuntimeProduct
