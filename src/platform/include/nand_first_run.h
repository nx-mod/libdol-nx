#pragma once

// First-run NAND files that no payload can ship because they are per-console
// state the Wii Menu writes: the Mii database and SYSCONF. A managed NAND has
// no Wii Menu, so without these the game finds them missing - RFL (the Mii
// library) fails its database read, and every SC (system configuration) getter
// silently falls back to a built-in default. Dolphin generates both the same
// way; its Data/Sys/Wii payload ships only the WC24 tree, exactly like ours.
//
// Formats: https://wiibrew.org/wiki//shared2/menu/FaceLib/RFL_DB.dat
//          https://wiibrew.org/wiki//shared2/sys/SYSCONF

#include "console_region.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeNandFirstRun {

// ============================================================================
// Mii database (/shared2/menu/FaceLib/RFL_DB.dat)
// ============================================================================

inline constexpr size_t kMiiDatabaseSize = 0x1F1E0;
inline constexpr size_t kMiiDatabaseParadeOffset = 0x1D00;
inline constexpr size_t kMiiDatabaseCrcOffset = 0x1F1DE;

// CRC-16/CCITT (polynomial 0x1021, zero initial value), stored big-endian.
inline uint16_t Crc16Ccitt(const uint8_t* data, size_t length) {
    uint16_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// An empty database: both section magics, no Mii entries, valid checksum.
inline std::vector<uint8_t> MakeEmptyMiiDatabase() {
    std::vector<uint8_t> db(kMiiDatabaseSize, 0);
    const auto writeMagic = [&db](size_t offset, std::string_view magic) {
        std::copy(magic.begin(), magic.end(), db.begin() + static_cast<ptrdiff_t>(offset));
    };
    writeMagic(0, "RNOD");                            // Mii entries
    writeMagic(kMiiDatabaseParadeOffset, "RNHD");     // Mii Parade

    const uint16_t crc = Crc16Ccitt(db.data(), kMiiDatabaseCrcOffset);
    db[kMiiDatabaseCrcOffset] = static_cast<uint8_t>(crc >> 8);
    db[kMiiDatabaseCrcOffset + 1] = static_cast<uint8_t>(crc & 0xFFu);
    return db;
}

// ============================================================================
// SYSCONF (/shared2/sys/SYSCONF)
// ============================================================================

inline constexpr size_t kSysconfSize = 0x4000;

enum class SysconfType : uint8_t {
    BigArray = 1,
    SmallArray = 2,
    Byte = 3,
    Short = 4,
    Long = 5,
    LongLong = 6,
    Bool = 7,
};

struct SysconfItem {
    std::string name;
    SysconfType type;
    std::vector<uint8_t> data;
};

inline SysconfItem SysconfByte(std::string name, uint8_t value) {
    return {std::move(name), SysconfType::Byte, {value}};
}

inline SysconfItem SysconfBool(std::string name, bool value) {
    return {std::move(name), SysconfType::Bool, {static_cast<uint8_t>(value ? 1 : 0)}};
}

inline SysconfItem SysconfLong(std::string name, uint32_t value) {
    return {std::move(name),
            SysconfType::Long,
            {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)}};
}

inline SysconfItem SysconfBigArray(std::string name, size_t length) {
    return {std::move(name), SysconfType::BigArray, std::vector<uint8_t>(length, 0)};
}

// The settings a Wii ships with, matching what this runtime already answers in
// its SC HLE (widescreen and PAL60 on, English) so the file and the overrides
// cannot disagree. IPL.SADR is present because the game reads it and reports
// "Can't get SimpleAddressData" when it is absent.
inline std::vector<SysconfItem> DefaultSysconfItems() {
    std::vector<SysconfItem> items;
    items.push_back(SysconfLong("BT.SENS", 3));          // sensor bar sensitivity
    items.push_back(SysconfByte("BT.BAR", 1));           // sensor bar above the screen
    items.push_back(SysconfByte("BT.SPKV", 0x58));       // Wii Remote speaker volume
    items.push_back(SysconfBool("BT.MOT", true));        // rumble
    items.push_back(SysconfByte("IPL.AR", 1));           // 16:9
    const ConsoleRegion::Info& region = ConsoleRegion::Active();
    items.push_back(SysconfByte("IPL.LNG", region.language));
    // PAL60 only means anything on a 50 Hz console; NTSC is 60 Hz already.
    items.push_back(SysconfBool("IPL.E60", region.pal));
    items.push_back(SysconfBool("IPL.PGS", false));      // progressive scan
    items.push_back(SysconfByte("IPL.SND", 1));          // stereo
    items.push_back(SysconfByte("IPL.SSV", 0));          // screen saver
    items.push_back(SysconfBool("IPL.EULA", true));      // EULA accepted
    items.push_back(SysconfBool("IPL.UPT", true));       // update prompt seen
    items.push_back(SysconfLong("IPL.CB", 0));           // counter bias
    items.push_back(SysconfBigArray("IPL.SADR", 0x1007));  // simple address data
    return items;
}

// Header: "SCv0", item count, then one big-endian offset per item. Each item is
// a header byte (type in the high 3 bits, name length minus one in the low 5),
// the name, and the value. "SCed" closes the file at 0x3FFC.
inline std::vector<uint8_t> MakeSysconf(const std::vector<SysconfItem>& items) {
    std::vector<uint8_t> file(kSysconfSize, 0);
    const auto write16 = [&file](size_t offset, uint16_t value) {
        file[offset] = static_cast<uint8_t>(value >> 8);
        file[offset + 1] = static_cast<uint8_t>(value & 0xFFu);
    };

    const std::string_view magic = "SCv0";
    std::copy(magic.begin(), magic.end(), file.begin());
    write16(0x0004, static_cast<uint16_t>(items.size()));

    // Offsets follow the count; the table is terminated by a zero entry.
    const size_t offsetTable = 0x0006;
    size_t cursor = offsetTable + (items.size() + 1) * sizeof(uint16_t);

    for (size_t i = 0; i < items.size(); ++i) {
        const SysconfItem& item = items[i];
        const size_t payload = item.type == SysconfType::BigArray     ? item.data.size() + 2
                               : item.type == SysconfType::SmallArray ? item.data.size() + 1
                                                                      : item.data.size();
        if (cursor + 1 + item.name.size() + payload > 0x3FAE) {
            break;  // Never run into the trailing lookup table or the footer.
        }
        write16(offsetTable + i * sizeof(uint16_t), static_cast<uint16_t>(cursor));

        file[cursor++] = static_cast<uint8_t>((static_cast<uint8_t>(item.type) << 5) |
                                              ((item.name.size() - 1) & 0x1Fu));
        std::copy(item.name.begin(), item.name.end(), file.begin() + static_cast<ptrdiff_t>(cursor));
        cursor += item.name.size();

        // Array lengths are stored as "length minus one", like the name length.
        if (item.type == SysconfType::BigArray) {
            write16(cursor, static_cast<uint16_t>(item.data.size() - 1));
            cursor += 2;
        } else if (item.type == SysconfType::SmallArray) {
            file[cursor++] = static_cast<uint8_t>(item.data.size() - 1);
        }
        std::copy(item.data.begin(), item.data.end(), file.begin() + static_cast<ptrdiff_t>(cursor));
        cursor += item.data.size();
    }

    const std::string_view footer = "SCed";
    std::copy(footer.begin(), footer.end(), file.begin() + 0x3FFC);
    return file;
}

// ============================================================================
// Seeding
// ============================================================================

inline bool WriteFileIfMissing(const std::filesystem::path& path,
                               const std::vector<uint8_t>& contents) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return true;  // Never overwrite a profile the player already has.
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(contents.data()),
              static_cast<std::streamsize>(contents.size()));
    out.flush();
    return static_cast<bool>(out);
}

// Returns false and names the file when one could not be created.
inline bool SeedGeneratedFiles(const std::filesystem::path& root, std::string* outError = nullptr) {
    const std::filesystem::path miiDatabase = root / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat";
    if (!WriteFileIfMissing(miiDatabase, MakeEmptyMiiDatabase())) {
        if (outError) {
            *outError = "could not create " + miiDatabase.string();
        }
        return false;
    }

    const std::filesystem::path sysconf = root / "shared2" / "sys" / "SYSCONF";
    if (!WriteFileIfMissing(sysconf, MakeSysconf(DefaultSysconfItems()))) {
        if (outError) {
            *outError = "could not create " + sysconf.string();
        }
        return false;
    }
    return true;
}

}  // namespace RuntimeNandFirstRun
