// U8 archives: one built here, read back, plus the shapes that must be refused.
#include "wiinx/format/archive/u8.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (int index = 0; index < 4; index++) {
        out[at + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value >> (24 - index * 8));
    }
}

// An archive holding: a file at the root, a folder, and two files in it.
std::vector<std::uint8_t> BuildArchive() {
    struct Node {
        bool directory;
        std::string name;
        std::string contents;
        std::uint32_t ends;  // directories only
    };
    const std::vector<Node> nodes{
        {true, "", "", 5},                        // the root, ending before node 5
        {false, "boot.bin", "the first file", 0},
        {true, "blyt", "", 5},                    // a folder with two files
        {false, "menu.brlyt", "a layout", 0},
        {false, "menu.brlan", "an animation", 0},
    };

    std::vector<std::uint8_t> names;
    std::vector<std::uint32_t> name_offsets;
    for (const Node& node : nodes) {
        name_offsets.push_back(static_cast<std::uint32_t>(names.size()));
        names.insert(names.end(), node.name.begin(), node.name.end());
        names.push_back(0);
    }

    const std::uint32_t nodes_at = 0x20;
    const std::uint32_t tables = static_cast<std::uint32_t>(nodes.size() * 12 + names.size());
    const std::uint32_t data_at = (nodes_at + tables + 31) & ~31u;

    std::vector<std::uint8_t> archive(data_at, 0);
    Put32(archive, 0x00, 0x55AA382D);
    Put32(archive, 0x04, nodes_at);
    Put32(archive, 0x08, tables);
    Put32(archive, 0x0C, data_at);

    std::uint32_t at = data_at;
    for (std::size_t index = 0; index < nodes.size(); index++) {
        const Node& node = nodes[index];
        const std::size_t entry = nodes_at + index * 12;
        Put32(archive, entry, (node.directory ? 0x01000000u : 0u) | name_offsets[index]);
        if (node.directory) {
            Put32(archive, entry + 4, 0);          // its parent
            Put32(archive, entry + 8, node.ends);  // the node it ends before
        } else {
            Put32(archive, entry + 4, at);
            Put32(archive, entry + 8, static_cast<std::uint32_t>(node.contents.size()));
            at += static_cast<std::uint32_t>((node.contents.size() + 31) & ~std::size_t{31});
        }
    }
    std::memcpy(archive.data() + nodes_at + nodes.size() * 12, names.data(), names.size());

    archive.resize(at, 0);
    for (std::size_t index = 0; index < nodes.size(); index++) {
        if (nodes[index].directory) {
            continue;
        }
        const std::size_t entry = nodes_at + index * 12;
        const std::uint32_t offset = (static_cast<std::uint32_t>(archive[entry + 4]) << 24) |
                                     (static_cast<std::uint32_t>(archive[entry + 5]) << 16) |
                                     (static_cast<std::uint32_t>(archive[entry + 6]) << 8) |
                                     archive[entry + 7];
        std::memcpy(archive.data() + offset, nodes[index].contents.data(),
                    nodes[index].contents.size());
    }
    return archive;
}
}  // namespace

int main() {
    using namespace wiinx::archive;
    std::printf("wiinx::archive U8 check\n\n");

    const auto bytes = BuildArchive();
    const auto archive = U8::Parse(bytes.data(), bytes.size());
    Check("an archive parses", archive.has_value());
    if (archive) {
        Check("every node but the root is listed", archive->Entries().size() == 4);
        const U8Entry* boot = archive->Find("boot.bin");
        Check("a file at the root is found", boot != nullptr && !boot->directory);
        if (boot) {
            Check("  its bytes",
                  std::string(reinterpret_cast<const char*>(archive->Bytes(*boot)), boot->size) ==
                      "the first file");
        }
        const U8Entry* folder = archive->Find("blyt");
        Check("a folder is a folder", folder != nullptr && folder->directory);

        const U8Entry* layout = archive->Find("blyt/menu.brlyt");
        Check("a file inside a folder keeps its path", layout != nullptr);
        if (layout) {
            Check("  its bytes",
                  std::string(reinterpret_cast<const char*>(archive->Bytes(*layout)),
                              layout->size) == "a layout");
        }
        const U8Entry* animation = archive->Find("blyt/menu.brlan");
        Check("and so does the one beside it", animation != nullptr);
        Check("a path that is not there is not found", archive->Find("blyt/nothing") == nullptr);
    }

    // Shapes to refuse rather than trust.
    const std::vector<std::uint8_t> junk(0x100, 0x5A);
    Check("something that is not an archive is refused",
          !U8::Parse(junk.data(), junk.size()).has_value());

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + 0x30);
    Check("an archive cut short is refused",
          !U8::Parse(truncated.data(), truncated.size()).has_value());

    std::vector<std::uint8_t> lying = bytes;
    Put32(lying, 0x08, 0x7FFFFFFF);  // tables longer than the file
    Check("a table that runs past the file is refused",
          !U8::Parse(lying.data(), lying.size()).has_value());

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
