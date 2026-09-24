// The launcher: what is on the card, and handing over to it.
//
// Every game, WAD and system title builds into its own NRO under
// sdmc:/wii-nx/<kind>/<name>/<name>.nro. Reaching one meant finding it in the
// homebrew menu's file list, which says nothing about what it is. This lists
// what is actually installed and starts it.
//
// Handing over is libnx's: envSetNextLoad names the next NRO and returning
// lets the loader run it, so the launcher is not resident while a game plays.
// That is the whole trick - there is no second process and no memory kept.

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

// Where a build puts a title, and what to call that shelf on screen.
struct Shelf {
    const char* directory;
    const char* label;
};

constexpr Shelf kShelves[] = {
    {"games", "Games"},
    {"wads", "WiiWare"},
    {"titles", "System"},
};

constexpr const char* kRoot = "sdmc:/wii-nx";

struct Entry {
    std::string name;     // the folder, which is also the NRO's name
    std::string path;     // the NRO itself
    const char* shelf;
};

bool Exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

// One shelf's titles: a folder is a title when it holds an NRO named after it.
void Collect(const Shelf& shelf, std::vector<Entry>& out) {
    const std::string directory = std::string(kRoot) + "/" + shelf.directory;
    DIR* handle = opendir(directory.c_str());
    if (handle == nullptr) {
        return;
    }
    while (const dirent* item = readdir(handle)) {
        if (item->d_name[0] == '.') {
            continue;
        }
        const std::string name = item->d_name;
        const std::string nro = directory + "/" + name + "/" + name + ".nro";
        if (Exists(nro)) {
            out.push_back({name, nro, shelf.label});
        }
    }
    closedir(handle);
}

std::vector<Entry> Installed() {
    std::vector<Entry> entries;
    for (const auto& shelf : kShelves) {
        Collect(shelf, entries);
    }
    // Grouped by shelf in the order above, then by name, so the list does not
    // reorder itself when the filesystem feels like it.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.shelf != b.shelf) {
            return a.shelf < b.shelf;
        }
        return a.name < b.name;
    });
    return entries;
}

void Draw(const std::vector<Entry>& entries, std::size_t chosen) {
    consoleClear();
    std::printf("\x1b[1;1H wii-nx\n\n");
    if (entries.empty()) {
        std::printf("   Nothing installed.\n\n"
                    "   A title lives in %s/<games|wads|titles>/<name>/<name>.nro\n",
                    kRoot);
        return;
    }
    const char* shelf = nullptr;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (shelf == nullptr || entries[index].shelf != shelf) {
            shelf = entries[index].shelf;
            std::printf("\n %s\n", shelf);
        }
        std::printf("  %s %s\n", index == chosen ? "\x1b[32m>" : " ", entries[index].name.c_str());
        if (index == chosen) {
            std::printf("\x1b[0m");
        }
    }
    std::printf("\n\n A start   B exit\n");
}

}  // namespace

int main(int argc, char** argv) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    const auto entries = Installed();
    std::size_t chosen = 0;
    Draw(entries, chosen);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad);
        if (pressed & HidNpadButton_B) {
            break;
        }
        if (!entries.empty()) {
            const std::size_t last = entries.size() - 1;
            if ((pressed & HidNpadButton_Down) && chosen < last) {
                ++chosen;
                Draw(entries, chosen);
            } else if ((pressed & HidNpadButton_Up) && chosen > 0) {
                --chosen;
                Draw(entries, chosen);
            } else if (pressed & HidNpadButton_A) {
                // The loader reads this when this program returns, so say what
                // is next and then leave: nothing of the launcher stays behind.
                if (envHasNextLoad()) {
                    envSetNextLoad(entries[chosen].path.c_str(), entries[chosen].path.c_str());
                    break;
                }
                std::printf("\n This loader cannot start another title.\n");
            }
        }
        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return 0;
}
