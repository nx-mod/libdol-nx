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
#include <utility>
#include <cstring>
#include <cstdint>
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

// System first: the Wii Menu and the channels are what a console shows you
// before anything else, and they are the shortest list.
constexpr Shelf kShelves[] = {
    {"titles", "System"},
    {"games", "Games"},
    {"wads", "WiiWare"},
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

// The name an NRO carries for itself.
//
// Every build writes one - "Mega Man 9 (USA)" rather than "megaman9-nx" - into
// the NACP the loader and the home menu read, so it is already there and does
// not need a file of its own beside it. The layout: "NRO0" at 0x10 with the
// program's size at 0x18, then "ASET" where the program ends, and in that the
// NACP's offset and size. The first language entry begins with the name, 0x200
// bytes, NUL-padded.
std::string NroName(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    auto close = [&] { std::fclose(file); };

    char magic[4]{};
    std::uint32_t nroSize = 0;
    if (std::fseek(file, 0x10, SEEK_SET) != 0 || std::fread(magic, 1, 4, file) != 4 ||
        std::memcmp(magic, "NRO0", 4) != 0 ||
        std::fseek(file, 0x18, SEEK_SET) != 0 || std::fread(&nroSize, 4, 1, file) != 1) {
        close();
        return {};
    }

    struct { std::uint64_t offset, size; } nacp{};
    if (std::fseek(file, static_cast<long>(nroSize), SEEK_SET) != 0 ||
        std::fread(magic, 1, 4, file) != 4 || std::memcmp(magic, "ASET", 4) != 0 ||
        // 0x00 magic, 0x04 version, 0x08 icon offset+size, 0x18 nacp offset+size.
        std::fseek(file, static_cast<long>(nroSize) + 0x18, SEEK_SET) != 0 ||
        std::fread(&nacp, sizeof(nacp), 1, file) != 1 || nacp.size == 0) {
        close();
        return {};
    }

    char name[0x201]{};
    if (std::fseek(file, static_cast<long>(nroSize + nacp.offset), SEEK_SET) != 0 ||
        std::fread(name, 1, 0x200, file) != 0x200) {
        close();
        return {};
    }
    close();
    name[0x200] = '\0';
    return name[0] == '\0' ? std::string{} : std::string(name);
}

// The NRO inside a title's folder: the one named after the folder, or failing
// that the only one there. A build writes the first; a title put on the card by
// hand often has the second, and there is no reason to hide it for that.
std::string FindNro(const std::string& directory, const std::string& name) {
    const std::string named = directory + "/" + name + "/" + name + ".nro";
    if (Exists(named)) {
        return named;
    }
    DIR* handle = opendir((directory + "/" + name).c_str());
    if (handle == nullptr) {
        return {};
    }
    std::string only;
    std::size_t found = 0;
    while (const dirent* item = readdir(handle)) {
        const std::string file = item->d_name;
        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".nro") == 0) {
            ++found;
            only = directory + "/" + name + "/" + file;
        }
    }
    closedir(handle);
    // More than one and there is nothing to choose between them, so say nothing
    // rather than start the wrong one.
    return found == 1 ? only : std::string{};
}

// One shelf's titles: a folder is a title when it holds an NRO.
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
        if (const std::string nro = FindNro(directory, name); !nro.empty()) {
            // Its own name if it has one; the folder is only a fallback for a
            // title built before the build started writing one.
            std::string shown = NroName(nro);
            if (shown.empty()) {
                shown = name;
            }
            out.push_back({shown, nro, shelf.label});
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

// libnx's console is 80x45 at 1280x720, and the runtime puts its loading line
// on row 42 - bottom-centred, the way a game does. This matches it, so the
// launcher and everything it starts look like one thing.
constexpr int kColumns = 80;
constexpr int kRows = 45;
constexpr int kFooterRow = kRows - 2;        // the bottom line
constexpr int kHintRow = kFooterRow - 3;     // the controls, three above it
constexpr const char* kFooter = "GITHUB | NX-MOD | WII-NX";

void PutCentred(int row, const std::string& text, const char* colour = nullptr) {
    int column = (kColumns - static_cast<int>(text.size())) / 2 + 1;
    if (column < 1) {
        column = 1;
    }
    std::printf("\x1b[%d;%dH%s%s%s", row, column,
                colour == nullptr ? "" : colour, text.c_str(),
                colour == nullptr ? "" : "\x1b[0m");
}

void Draw(const std::vector<Entry>& entries, std::size_t chosen) {
    consoleClear();

    // Every shelf heading and every title is one line; the block is centred on
    // the screen so a short list does not sit in the top corner.
    std::vector<std::pair<const char*, std::string>> lines;   // shelf, or null for a title
    const char* shelf = nullptr;
    for (const auto& entry : entries) {
        if (shelf == nullptr || entry.shelf != shelf) {
            // A blank line between shelves, so System and Games do not run
            // together. Nothing above the first heading.
            if (shelf != nullptr) {
                lines.emplace_back(nullptr, std::string{});
            }
            shelf = entry.shelf;
            lines.emplace_back(shelf, std::string{});
        }
        lines.emplace_back(nullptr, entry.name);
    }

    const int body = static_cast<int>(lines.size());
    const int top = std::max(2, (kRows - body) / 2);

    PutCentred(top - 1, "WII-NX", "\x1b[1m");
    if (entries.empty()) {
        PutCentred(kRows / 2, "Nothing installed");
        PutCentred(kRows / 2 + 2, std::string(kRoot) + "/<games|wads|titles>/<name>/<name>.nro");
    } else {
        std::size_t index = 0;
        for (int line = 0; line < body; ++line) {
            const auto& [heading, text] = lines[static_cast<std::size_t>(line)];
            if (heading != nullptr) {
                PutCentred(top + line + 1, heading, "\x1b[2m");
                continue;
            }
            if (text.empty()) {
                continue;                      // the spacer between shelves
            }
            const bool selected = index == chosen;
            PutCentred(top + line + 1, selected ? "> " + text + " <" : text,
                       selected ? "\x1b[32;1m" : nullptr);
            ++index;
        }
        PutCentred(kHintRow, "A start    B exit", "\x1b[2m");
    }

    PutCentred(kFooterRow, kFooter, "\x1b[2m");
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
