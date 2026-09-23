#pragma once

// The NAND's own filesystem: title ids, the paths IOS keeps things at, and the
// obfuscation `setting.txt` is stored with.
//
// A Wii's internal memory is one flat tree owned by IOS. Everything a title
// carries lives under `/title/<high>/<low>/`, its ticket under `/ticket/`, and
// the two files every game reads - the console's settings and the Mii database -
// under `/shared2/`. The paths are spelled the way IOS spells them: absolute,
// '/'-separated, whatever the host's own separator is.
//
// Nothing here touches a disk. A caller maps this tree onto wherever it keeps
// the NAND (on Switch, `sdmc:/wii-nx/system/nand/`) and reads the bytes itself.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wiinx::nand {

// A title id. The high word says what kind of title it is, the low word which
// one; together they name a directory in the NAND.
struct TitleId {
    std::uint64_t value = 0;

    static constexpr TitleId Make(std::uint32_t high, std::uint32_t low) {
        return TitleId{(static_cast<std::uint64_t>(high) << 32) | low};
    }

    constexpr std::uint32_t High() const { return static_cast<std::uint32_t>(value >> 32); }
    constexpr std::uint32_t Low() const { return static_cast<std::uint32_t>(value); }

    friend constexpr bool operator==(TitleId a, TitleId b) { return a.value == b.value; }

    // The two halves as a NAND path spells them: eight lowercase hex digits.
    std::string HighHex() const;
    std::string LowHex() const;

    // The low word read as four characters, which is what it is for discs and
    // channels: "RMCE" is Mario Kart Wii, "HAEA" the Photo Channel. Empty when
    // the bytes are not printable.
    std::string Code() const;
};

// The high word: what kind of title this is.
namespace title_kind {
inline constexpr std::uint32_t kSystem = 0x00000001;   // IOS, the System Menu, BC/MIOS
inline constexpr std::uint32_t kDisc = 0x00010000;     // a disc game's save title
inline constexpr std::uint32_t kChannel = 0x00010001;  // WiiWare and installed channels
inline constexpr std::uint32_t kSystemChannel = 0x00010002;  // News, Weather, Shop, menus
inline constexpr std::uint32_t kGameChannel = 0x00010004;    // a disc game's own channel
inline constexpr std::uint32_t kDlc = 0x00010005;
inline constexpr std::uint32_t kHiddenChannel = 0x00010008;
}  // namespace title_kind

// The titles anything reading a NAND ends up naming.
inline constexpr TitleId kSystemMenu = TitleId::Make(title_kind::kSystem, 0x00000002);
inline constexpr TitleId kMiiChannel = TitleId::Make(title_kind::kSystemChannel, 0x48414641);  // HAFA

// Where things live. Every path is absolute and has no trailing slash.
std::string TitlePath(TitleId id);                      // /title/00010001/48414641
std::string ContentPath(TitleId id);                    // .../content
std::string DataPath(TitleId id);                       // .../data
std::string TmdPath(TitleId id);                        // .../content/title.tmd
std::string ContentFilePath(TitleId id, std::uint32_t content_id);  // .../content/0000001b.app
std::string TicketPath(TitleId id);                     // /ticket/00010001/48414641.tik

// The files that belong to the console rather than to a title.
inline constexpr std::string_view kSysconfPath = "/shared2/sys/SYSCONF";
inline constexpr std::string_view kMiiDatabasePath = "/shared2/menu/FaceLib/RFL_DB.dat";
inline constexpr std::string_view kSettingPath = "/title/00000001/00000002/data/setting.txt";

// `setting.txt` holds the console's identity - region, model, serial - as plain
// `KEY=VALUE` lines run through a keystream. It is obfuscation, not encryption:
// the key is fixed and the same operation undoes it.
std::vector<std::uint8_t> ScrambleSetting(std::string_view text);
std::string UnscrambleSetting(const std::vector<std::uint8_t>& file);

}  // namespace wiinx::nand
