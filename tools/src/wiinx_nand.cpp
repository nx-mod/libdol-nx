// What a NAND folder holds, and what it is missing.
//
//   wiinx-nand check   <folder>   what is there, and what a console would expect
//   wiinx-nand init    <folder>   create what is missing, and nothing else
//   wiinx-nand sysconf <folder>   every setting in it, by name
//
// A Wii keeps everything in one tree owned by IOS. A folder assembled by
// wiinx-fetch-nand has the titles and their tickets and nothing else, because
// the rest is written by the console as it runs: the settings, the Mii
// database, the per-title save directories. The runtime seeds the first two on
// its first run, which means a NAND cannot be looked at, prepared or checked
// before it goes to the card.
//
// This does it here instead, from the same code the runtime uses, so what it
// writes is byte for byte what the console would have written.
//
// Nothing here reads a console dump or needs a key: it works on the folder.

#include "wiinx/format/nand/isfs.hpp"
#include "wiinx/format/nand/mii.hpp"
#include "wiinx/format/nand/store.hpp"
#include "wiinx/format/nand/sysconf.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wiinx::nand;

namespace {

// The directories IOS keeps, whether or not anything is in them yet.
constexpr const char* kDirectories[] = {
    "sys", "ticket", "title", "shared1", "shared2", "import", "meta", "tmp",
    "shared2/sys", "shared2/menu", "shared2/menu/FaceLib",
};

std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// The NAND tree, mapped onto a folder. Store takes plain function pointers and
// a context, so the root travels in the context rather than in a capture.
struct Folder {
    fs::path root;

    fs::path Resolve(const std::string& path) const {
        return root / (path.empty() || path[0] != '/' ? path : path.substr(1));
    }
};

bool FolderRead(void* context, const std::string& path, std::vector<std::uint8_t>& out) {
    auto bytes = ReadFile(static_cast<Folder*>(context)->Resolve(path));
    if (bytes.empty()) return false;
    out = std::move(bytes);
    return true;
}

bool FolderWrite(void* context, const std::string& path, const std::uint8_t* data,
                 std::size_t size) {
    return WriteFile(static_cast<Folder*>(context)->Resolve(path),
                     std::vector<std::uint8_t>(data, data + size));
}

bool FolderRemove(void* context, const std::string& path) {
    std::error_code ignored;
    return fs::remove(static_cast<Folder*>(context)->Resolve(path), ignored);
}

bool FolderList(void* context, const std::string& path, std::vector<std::string>& out) {
    const auto directory = static_cast<Folder*>(context)->Resolve(path);
    if (!fs::is_directory(directory)) return false;
    for (const auto& entry : fs::directory_iterator(directory))
        out.push_back(entry.path().filename().string());
    return true;
}

Store FolderStore(Folder& folder) {
    Store store;
    store.context = &folder;
    store.read = &FolderRead;
    store.write = &FolderWrite;
    store.remove = &FolderRemove;
    store.list = &FolderList;
    return store;
}

int Report(const fs::path& root, bool create) {
    if (!fs::is_directory(root)) {
        std::fprintf(stderr, "no such NAND folder: %s\n", root.c_str());
        return 1;
    }
    std::printf(">> %s\n", root.c_str());

    int missing = 0;
    for (const char* name : kDirectories) {
        const auto path = root / name;
        if (fs::is_directory(path)) continue;
        ++missing;
        if (create) {
            fs::create_directories(path);
            std::printf("   created  %s/\n", name);
        } else {
            std::printf("   missing  %s/\n", name);
        }
    }

    // The two files a console writes for itself, from the library's own
    // defaults - the same bytes the runtime seeds on its first run.
    const struct { const char* path; std::vector<std::uint8_t> (*make)(); } seeded[] = {
        {"shared2/sys/SYSCONF", [] { return Sysconf::Defaults().Build(); }},
        {"shared2/menu/FaceLib/RFL_DB.dat", [] { return MiiDatabase::Empty().Build(); }},
    };
    for (const auto& file : seeded) {
        const auto path = root / file.path;
        if (fs::exists(path)) {
            std::printf("   present  %-34s %ju bytes\n", file.path,
                        static_cast<std::uintmax_t>(fs::file_size(path)));
            continue;
        }
        ++missing;
        if (create) {
            if (!WriteFile(path, file.make())) {
                std::fprintf(stderr, "could not write %s\n", path.c_str());
                return 1;
            }
            std::printf("   created  %s\n", file.path);
        } else {
            std::printf("   missing  %s\n", file.path);
        }
    }

    Folder folder{root};
    const auto titles = InstalledTitles(FolderStore(folder));
    std::printf("   titles   %zu installed\n", titles.size());

    // What is already there is worth reading back, not just counting.
    if (const auto bytes = ReadFile(root / "shared2/sys/SYSCONF"); !bytes.empty()) {
        if (const auto sysconf = Sysconf::Parse(bytes)) {
            std::printf("   sysconf  %zu settings", sysconf->Items().size());
            if (const auto language = sysconf->Number(sysconf_keys::kLanguage))
                std::printf(", language %ju", static_cast<std::uintmax_t>(*language));
            std::printf("\n");
        } else {
            std::printf("   sysconf  UNREADABLE\n");
        }
    }
    if (const auto bytes = ReadFile(root / "shared2/menu/FaceLib/RFL_DB.dat"); !bytes.empty()) {
        bool checksum = false;
        if (const auto database = MiiDatabase::Parse(bytes, &checksum)) {
            std::printf("   miis     %zu%s\n", database->Count(),
                        checksum ? "" : "  (checksum stale)");
        } else {
            std::printf("   miis     UNREADABLE\n");
        }
    }

    if (!create && missing) {
        std::printf("\n%d missing. `wiinx-nand init %s` writes them.\n", missing, root.c_str());
    }
    return 0;
}

int ShowSysconf(const fs::path& root) {
    const auto bytes = ReadFile(root / "shared2/sys/SYSCONF");
    if (bytes.empty()) {
        std::fprintf(stderr, "no SYSCONF in %s\n", root.c_str());
        return 1;
    }
    const auto sysconf = Sysconf::Parse(bytes);
    if (!sysconf) {
        std::fprintf(stderr, "not a SYSCONF: %s\n", root.c_str());
        return 1;
    }
    std::printf(">> %s  (%zu settings)\n", root.c_str(), sysconf->Items().size());
    for (const auto& item : sysconf->Items()) {
        std::printf("   %-10s ", item.name.c_str());
        switch (item.type) {
            case SysconfType::SmallArray:
            case SysconfType::BigArray:
                std::printf("%zu bytes\n", item.data.size());
                break;
            default:
                std::printf("%ju\n", static_cast<std::uintmax_t>(item.Number()));
                break;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("wiinx-nand check   <folder>   what is there, and what is missing\n"
                    "wiinx-nand init    <folder>   create what is missing\n"
                    "wiinx-nand sysconf <folder>   every setting in it, by name\n");
        return 2;
    }
    const std::string command = argv[1];
    if (command == "check") return Report(argv[2], false);
    if (command == "init") return Report(argv[2], true);
    if (command == "sysconf") return ShowSysconf(argv[2]);
    std::fprintf(stderr, "unknown command: %s\n", command.c_str());
    return 2;
}
