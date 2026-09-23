// Reading a U8 archive.
//
//   0x00  magic, 0x55AA382D
//   0x04  where the nodes begin, from the start of the archive
//   0x08  how long the nodes and the names are, together
//   0x0C  where the data begins
//   then  16 bytes of nothing, and the nodes
//
// A node is twelve bytes: a type byte and three bytes of name offset, then two
// numbers whose meaning depends on the type. For a file they are where its
// bytes are and how many; for a folder they are the node its parent is and the
// node it ends before. The first node is the root, and its second number is how
// many nodes there are.
#include "wiinx/format/archive/u8.hpp"

#include <cstring>

namespace wiinx::archive {
namespace {

constexpr std::size_t kNodeSize = 12;

std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

std::optional<U8> U8::Parse(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 0x20 || Read32(data) != kU8Magic) {
        return std::nullopt;
    }

    const std::uint32_t nodes_at = Read32(data + 0x04);
    const std::uint32_t tables_size = Read32(data + 0x08);
    if (nodes_at + kNodeSize > size || nodes_at + tables_size > size) {
        return std::nullopt;
    }

    const std::uint8_t* nodes = data + nodes_at;
    const std::uint32_t count = Read32(nodes + 8);
    if (count == 0 || static_cast<std::size_t>(count) * kNodeSize > tables_size) {
        return std::nullopt;
    }

    // The names sit behind the nodes, inside the same run of bytes.
    const std::uint8_t* names = nodes + static_cast<std::size_t>(count) * kNodeSize;
    const std::size_t names_size = tables_size - static_cast<std::size_t>(count) * kNodeSize;

    const auto name_at = [&](std::uint32_t offset) {
        if (offset >= names_size) {
            return std::string{};
        }
        const auto* end = static_cast<const std::uint8_t*>(
            std::memchr(names + offset, 0, names_size - offset));
        return std::string(reinterpret_cast<const char*>(names + offset),
                           end != nullptr ? static_cast<std::size_t>(end - (names + offset))
                                          : names_size - offset);
    };

    U8 archive;
    archive.mData = data;
    archive.mSize = size;
    archive.mEntries.reserve(count - 1);

    // A folder says which node it ends before, so the path is kept on a stack.
    struct Open {
        std::string path;
        std::uint32_t ends;
    };
    std::vector<Open> open;

    for (std::uint32_t index = 1; index < count; index++) {
        const std::uint8_t* node = nodes + static_cast<std::size_t>(index) * kNodeSize;
        const bool directory = node[0] != 0;
        const std::uint32_t name_offset = Read32(node) & 0x00FFFFFF;
        const std::uint32_t second = Read32(node + 4);
        const std::uint32_t third = Read32(node + 8);

        while (!open.empty() && index >= open.back().ends) {
            open.pop_back();
        }
        std::string path;
        for (const Open& folder : open) {
            path += folder.path;
            path += '/';
        }
        const std::string name = name_at(name_offset);
        path += name;

        if (directory) {
            archive.mEntries.push_back(U8Entry{path, true, 0, 0});
            open.push_back(Open{name, third});
        } else {
            if (static_cast<std::size_t>(second) + third > size) {
                return std::nullopt;  // a file that runs past the archive
            }
            archive.mEntries.push_back(U8Entry{path, false, second, third});
        }
    }
    return archive;
}

const U8Entry* U8::Find(const std::string& path) const {
    for (const U8Entry& entry : mEntries) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace wiinx::archive
