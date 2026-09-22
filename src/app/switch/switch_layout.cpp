// SD card layout on Switch: folder creation and the one-time move from the
// old sdmc:/WiiCompiled/ install. See switch_layout.h for the layout itself.
#if defined(__SWITCH__)

#include "switch_layout.h"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace SwitchLayout {
namespace {

bool Exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

void MakeDirectory(const std::string& path) {
    mkdir(path.c_str(), 0777);
}

// Moves one entry if it exists and its destination does not, noting the result.
void Move(const std::string& from, const std::string& to, std::string& summary) {
    if (!Exists(from) || Exists(to)) {
        return;
    }
    const bool moved = std::rename(from.c_str(), to.c_str()) == 0;
    summary += moved ? " " : " FAILED:";
    summary += from.substr(from.rfind('/') + 1);
}

// Config.toml becomes config.toml. The old file named the disc folder "DATA";
// the new layout's default is disc/, so the explicit setting is dropped.
void MoveConfig(const std::string& from, const std::string& to, std::string& summary) {
    if (!Exists(from) || Exists(to)) {
        return;
    }
    std::ifstream input(from);
    std::ostringstream text;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("dvd_root") != std::string::npos && line.find("\"DATA\"") != std::string::npos) {
            text << "# dvd_root defaults to disc/ next to this file.\n";
            continue;
        }
        text << line << '\n';
    }
    input.close();
    std::ofstream output(to);
    output << text.str();
    output.close();
    if (output) {
        std::remove(from.c_str());
        summary += " Config.toml";
    } else {
        summary += " FAILED:Config.toml";
    }
}

}  // namespace

std::string MigrateLegacy() {
    MakeDirectory(kRoot);
    MakeDirectory(kConfigDir);
    MakeDirectory(kSystemDir);
    MakeDirectory(kGamesDir);
    MakeDirectory(kGameDir);

    std::string summary;
    if (Exists(kLegacyDir)) {
        const std::string legacy = kLegacyDir;
        Move(legacy + "/NAND", System("nand"), summary);
        Move(legacy + "/wii_bootstrap", System("bootstrap"), summary);
        Move(legacy + "/dsp_coef.bin", System("dsp_coef.bin"), summary);
        Move(legacy + "/loghost.txt", Config("loghost.txt"), summary);
        Move(legacy + "/DATA", Game(kDiscDirName), summary);
        Move(legacy + "/Cache", Game(kCacheDirName), summary);
        Move(legacy + "/initial_pipeline_cache.db", Game("cache/initial_pipeline_cache.db"), summary);
        MoveConfig(legacy + "/Config.toml", Game(kConfigFileName), summary);
    }

    MakeDirectory(Game(kCacheDirName));
    MakeDirectory(Game(kLogsDirName));
    return summary.empty() ? std::string{} : "moved from " + std::string(kLegacyDir) + ":" + summary;
}

}  // namespace SwitchLayout

#endif  // __SWITCH__
