#pragma once

// SD card layout on Switch (wii-nx). Every path the runtime touches on the SD
// card comes from here.
//
//   sdmc:/switch/wii-nx/wii-nx.nro   launcher (in the homebrew menu): finds games,
//                                     edits settings, launches, installs forwarders
//   sdmc:/wii-nx/
//     config/                  settings for every game
//       wii-nx.toml            defaults each game's config.toml overrides
//       <library>.toml         per-library settings (dawn-nx, nxvk, aurora-nx)
//       loghost.txt            dev: UDP log target
//     system/                  the emulated Wii, shared by every game
//       nand/                  system files, Mii database, saves (per title ID)
//       bootstrap/             Wii system files seeded into a new NAND
//       dsp_coef.bin           DSP coefficient ROM
//     games/<game>/            one folder per game, named after its project (mkwii-nx)
//     wads/<title>/            WiiWare, same shape
//     titles/<title>/          system titles, same shape
//       <game>.nro
//       config.toml
//       disc/                  the extracted disc (DATA: files/ and sys/)
//       cache/                 shader and pipeline caches, initial_pipeline_cache.db
//       logs/
//
// Older builds kept everything in sdmc:/WiiCompiled/; SwitchLayout::MigrateLegacy()
// moves it here once.

#if defined(__SWITCH__)

#include <string>

// The game's folder name. Each game build defines it; Mario Kart Wii is the default.
#ifndef WIINX_GAME_DIR
#define WIINX_GAME_DIR "mkwii-nx"
#endif

// Which shelf that folder is on: games, wads or titles. A title kept writing
// its config, caches and logs into games/ whatever shelf its NRO was on, so the
// Mii Channel's data and the Mii Channel were in different places.
#ifndef WIINX_GAME_SHELF
#define WIINX_GAME_SHELF "games"
#endif

// Compile-time path literals, for code that must not allocate (boot, crash and
// watchdog logging).
#define WIINX_GAME_PATH(relative) "sdmc:/wii-nx/" WIINX_GAME_SHELF "/" WIINX_GAME_DIR "/" relative
#define WIINX_CONFIG_PATH(relative) "sdmc:/wii-nx/config/" relative

namespace SwitchLayout {

inline constexpr const char* kRoot = "sdmc:/wii-nx";
inline constexpr const char* kConfigDir = "sdmc:/wii-nx/config";
inline constexpr const char* kSystemDir = "sdmc:/wii-nx/system";
inline constexpr const char* kGamesDir = "sdmc:/wii-nx/" WIINX_GAME_SHELF;
inline constexpr const char* kGameDir = "sdmc:/wii-nx/" WIINX_GAME_SHELF "/" WIINX_GAME_DIR;
inline constexpr const char* kLegacyDir = "sdmc:/WiiCompiled";

inline constexpr const char* kConfigFileName = "config.toml";
inline constexpr const char* kDiscDirName = "disc";
inline constexpr const char* kCacheDirName = "cache";
inline constexpr const char* kLogsDirName = "logs";

inline std::string Config(const char* relative) {
    return std::string(kConfigDir) + "/" + relative;
}

inline std::string System(const char* relative) {
    return std::string(kSystemDir) + "/" + relative;
}

inline std::string Game(const char* relative) {
    return std::string(kGameDir) + "/" + relative;
}

inline std::string Log(const char* fileName) {
    return std::string(kGameDir) + "/" + kLogsDirName + "/" + fileName;
}

// Creates the folder tree and, on the first run after an update, moves an
// sdmc:/WiiCompiled/ install into it. Call before anything opens an SD path.
// Returns a one-line summary for the boot log (empty when nothing was moved).
std::string MigrateLegacy();

}  // namespace SwitchLayout

#endif  // __SWITCH__
