#pragma once

// U8 ("ARC"): a directory of files in one blob.
//
// What the SDK packs a game's files in, and what a channel's contents are:
// a header, a table of nodes - one per file and folder - and then the data.
// Names live in a string table behind the nodes; a folder says which node it
// ends before, so the tree is walked with a stack rather than by recursion.
//
// Reading is done in place: a file's bytes are a span of the archive, never a
// copy, so serving a game its files costs nothing but the lookup.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinx::archive {

inline constexpr std::uint32_t kU8Magic = 0x55AA382D;

struct U8Entry {
    std::string path;          // "arc/blyt/menu.brlyt"
    bool directory = false;
    std::uint32_t offset = 0;  // where the file's bytes start in the archive
    std::uint32_t size = 0;
};

// An archive, read out of a buffer the caller keeps.
class U8 {
  public:
    // Returns nothing when the bytes are not a U8, or when its tables do not
    // fit inside what was given.
    static std::optional<U8> Parse(const std::uint8_t* data, std::size_t size);

    const std::vector<U8Entry>& Entries() const { return mEntries; }
    const U8Entry* Find(const std::string& path) const;

    // The file's bytes, inside the buffer this was parsed from.
    const std::uint8_t* Bytes(const U8Entry& entry) const { return mData + entry.offset; }

  private:
    const std::uint8_t* mData = nullptr;
    std::size_t mSize = 0;
    std::vector<U8Entry> mEntries;
};

}  // namespace wiinx::archive
