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
#include <iterator>
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

// The settings a Wii ships with: Dolphin's default set (Core/SysConf.cpp), so
// the Wii Menu finds everything it reads, with this runtime's choices where the
// SC HLE already answers (widescreen, PAL60 per region, English). IPL.CD and
// IPL.CD2 ("setup done") stay false, as on a new console: the menu runs its own
// first-time setup and sets them itself. IPL.SADR names a country for the
// region; with none, the menu's setup asked for US pages a European menu does
// not have. Games report "Can't get SimpleAddressData" without IPL.SADR.
inline std::vector<SysconfItem> DefaultSysconfItems() {
    const ConsoleRegion::Info& region = ConsoleRegion::Active();
    std::vector<SysconfItem> items;
    items.push_back(SysconfBigArray("BT.DINF", 0x461));  // paired Wii Remotes (none)
    items.push_back(SysconfBigArray("BT.CDIF", 0x205));
    items.push_back(SysconfLong("BT.SENS", 3));          // sensor bar sensitivity
    items.push_back(SysconfByte("BT.BAR", 1));           // sensor bar above the screen
    items.push_back(SysconfByte("BT.SPKV", 0x58));       // Wii Remote speaker volume
    items.push_back(SysconfByte("BT.MOT", 1));           // rumble

    // UTF-16 name, 10 characters at most, then a terminator and the length.
    SysconfItem nickname{"IPL.NIK", SysconfType::SmallArray, std::vector<uint8_t>(22, 0)};
    const std::string_view name = "Wii";
    for (size_t i = 0; i < name.size(); ++i) {
        nickname.data[i * 2 + 1] = static_cast<uint8_t>(name[i]);
    }
    nickname.data[21] = static_cast<uint8_t>(name.size());
    items.push_back(std::move(nickname));

    items.push_back(SysconfByte("IPL.LNG", region.language));
    SysconfItem address = SysconfBigArray("IPL.SADR", 0x1008);  // simple address data
    address.data[0] = region.country;
    items.push_back(std::move(address));
    SysconfItem parental{"IPL.PC", SysconfType::SmallArray, std::vector<uint8_t>(0x4A, 0)};
    parental.data[1] = 0x04;
    parental.data[2] = 0x14;
    items.push_back(std::move(parental));
    items.push_back(SysconfLong("IPL.CB", 0));           // counter bias
    items.push_back(SysconfByte("IPL.AR", 1));           // 16:9
    items.push_back(SysconfByte("IPL.SSV", 0));          // screen saver
    items.push_back(SysconfBool("IPL.CD", false));       // setup done: the menu sets it
    items.push_back(SysconfBool("IPL.CD2", false));
    items.push_back(SysconfBool("IPL.EULA", true));      // EULA accepted
    items.push_back(SysconfByte("IPL.UPT", 2));          // update prompt
    items.push_back(SysconfByte("IPL.PGS", 0));          // progressive scan
    // PAL60 only means anything on a 50 Hz console; NTSC is 60 Hz already.
    items.push_back(SysconfByte("IPL.E60", region.pal ? 1 : 0));
    items.push_back(SysconfByte("IPL.SND", 1));          // stereo
    items.push_back(SysconfByte("IPL.DH", 0));
    items.push_back(SysconfLong("IPL.INC", 8));
    items.push_back(SysconfLong("IPL.FRC", 0x28));
    items.push_back({"IPL.IDL", SysconfType::SmallArray, {0, 1}});
    items.push_back(SysconfLong("NET.WCFG", 1));
    items.push_back(SysconfLong("NET.CTPC", 0));
    items.push_back(SysconfByte("WWW.RST", 0));
    items.push_back(SysconfBool("MPLS.MOVIE", true));
    return items;
}

// What a SYSCONF already holds. An unreadable file gives nothing.
inline std::vector<SysconfItem> ParseSysconf(const std::vector<uint8_t>& file) {
    std::vector<SysconfItem> items;
    if (file.size() != kSysconfSize || std::string_view(reinterpret_cast<const char*>(file.data()), 4) != "SCv0") {
        return items;
    }
    const size_t count = (static_cast<size_t>(file[4]) << 8) | file[5];
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = 6 + i * 2;
        const size_t offset = (static_cast<size_t>(file[entry]) << 8) | file[entry + 1];
        if (offset == 0 || offset + 1 >= file.size()) {
            continue;
        }
        const auto type = static_cast<SysconfType>(file[offset] >> 5);
        const size_t nameLength = (file[offset] & 0x1Fu) + 1;
        size_t at = offset + 1 + nameLength;
        size_t length = 0;
        switch (type) {
            case SysconfType::BigArray: length = ((static_cast<size_t>(file[at]) << 8) | file[at + 1]) + 1; at += 2; break;
            case SysconfType::SmallArray: length = static_cast<size_t>(file[at]) + 1; at += 1; break;
            case SysconfType::Byte: case SysconfType::Bool: length = 1; break;
            case SysconfType::Short: length = 2; break;
            case SysconfType::Long: length = 4; break;
            case SysconfType::LongLong: length = 8; break;
            default: return {};
        }
        if (at + length > file.size()) {
            return {};
        }
        items.push_back({std::string(reinterpret_cast<const char*>(&file[offset + 1]), nameLength), type,
                         std::vector<uint8_t>(file.begin() + static_cast<ptrdiff_t>(at),
                                              file.begin() + static_cast<ptrdiff_t>(at + length))});
    }
    return items;
}

// An existing SYSCONF given what it lacks, every value it has kept. Earlier
// builds of this runtime wrote a short one (no nickname, no setup flags, an
// IPL.SADR a byte short with no country); that one IPL.SADR is replaced, since
// nothing but this runtime wrote it.
inline bool CompleteSysconf(std::vector<SysconfItem>& items) {
    bool changed = false;
    for (SysconfItem& item : items) {
        if (item.name == "IPL.SADR" && item.data.size() == 0x1007 &&
            std::all_of(item.data.begin(), item.data.end(), [](uint8_t b) { return b == 0; })) {
            item.data.assign(0x1008, 0);
            item.data[0] = ConsoleRegion::Active().country;
            changed = true;
        }
    }
    for (SysconfItem& wanted : DefaultSysconfItems()) {
        const bool present = std::any_of(items.begin(), items.end(),
                                         [&](const SysconfItem& item) { return item.name == wanted.name; });
        if (!present) {
            items.push_back(std::move(wanted));
            changed = true;
        }
    }
    return changed;
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
    std::error_code ec;
    if (std::filesystem::exists(sysconf, ec)) {
        std::ifstream in(sysconf, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::vector<SysconfItem> items = ParseSysconf(file);
        if (!items.empty() && CompleteSysconf(items)) {
            const std::vector<uint8_t> completed = MakeSysconf(items);
            std::ofstream out(sysconf, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(completed.data()),
                      static_cast<std::streamsize>(completed.size()));
        }
    } else if (!WriteFileIfMissing(sysconf, MakeSysconf(DefaultSysconfItems()))) {
        if (outError) {
            *outError = "could not create " + sysconf.string();
        }
        return false;
    }
    return true;
}

}  // namespace RuntimeNandFirstRun
