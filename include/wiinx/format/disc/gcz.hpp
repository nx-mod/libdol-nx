#pragma once

// GCZ: Dolphin's older compressed image, and what a collection made before
// about 2020 is in.
//
// Simpler than RVZ in every way: the disc is cut into blocks of one size, each
// block is deflated unless deflating made it larger, and a table says where
// each one is. Nothing is scrubbed, nothing is regenerated, and a Wii disc
// stays encrypted inside - so a reader hands the result to `Image` exactly as
// it would a raw file.
//
// As with RVZ, the decompressor is the caller's: a build that has zlib links
// it, and one that does not reads only the blocks that were stored plainly.

#include "wiinx/format/disc/image.hpp"
#include "wiinx/format/disc/rvz.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace wiinx::disc {

class Gcz {
  public:
    static std::optional<Gcz> Open(Source file, Decompressor decompressor);

    Console GetConsole() const { return mConsole; }
    std::uint64_t DiscSize() const { return mDiscSize; }
    std::uint32_t BlockSize() const { return mBlockSize; }

    // The image inside, as reads. A Wii disc read this way is still encrypted,
    // so `Image` needs a cipher for it as it would for a raw file.
    Source AsSource() const;

    // For the source handed out by AsSource().
    bool ReadThrough(std::uint64_t offset, std::uint8_t* out, std::size_t size) const;

  private:
    bool ReadBlock(std::uint64_t index, std::vector<std::uint8_t>& out) const;

    Source mFile;
    Decompressor mDecompressor;
    Console mConsole = Console::Wii;
    std::uint64_t mDiscSize = 0;
    std::uint32_t mBlockSize = 0;
    std::vector<std::uint64_t> mBlocks;  // where each block is, with a flag in the top bit
    std::uint64_t mDataAt = 0;

    mutable std::vector<std::uint8_t> mBlockBytes;
    mutable std::uint64_t mBlockIndex = UINT64_MAX;
};

}  // namespace wiinx::disc
