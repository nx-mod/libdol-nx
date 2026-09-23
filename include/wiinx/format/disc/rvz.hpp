#pragma once

// RVZ and WIA: the shapes most dumps are kept in.
//
// Dolphin's own formats, and what a dump made in the last few years almost
// always is. A disc is cut into chunks, each chunk compressed, and the
// pseudo-random padding Nintendo writes across the unused parts of a disc is
// thrown away and regenerated on the way out. A Wii disc's partitions are
// stored decrypted and with their hash blocks removed, which is what a reader
// wants anyway.
//
// Decompression is the caller's, the way encryption is: a `Decompressor` is
// handed in, so this library depends on no compressor and a program links only
// the ones it wants.
//
// Format reference: Dolphin's docs/WiaAndRvz.md.
//
// One thing is deliberately left out: the padding itself. A dump throws away
// the pseudo-random filler between a disc's files and records only the seed it
// can be regrown from. Those bytes are in the gaps no file occupies, so this
// reads a dump's real contents correctly while writing zeros where the filler
// would go. A tool that verifies a disc against its hashes needs the generator;
// reading files does not (see TODO.md).

#include "wiinx/format/disc/image.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace wiinx::disc {

// How a chunk was compressed. The numbers are the format's.
enum class Compression : std::uint32_t {
    None = 0,
    Purge = 1,
    Bzip2 = 2,
    Lzma = 3,
    Lzma2 = 4,
    Zstd = 5,
};

struct Decompressor {
    void* context = nullptr;
    // Expands one chunk. Returns the bytes written, or 0 when it cannot - an
    // unsupported method included, which the reader reports rather than guesses
    // around.
    std::size_t (*inflate)(void* context, Compression method, const std::uint8_t* in,
                           std::size_t in_size, std::uint8_t* out, std::size_t out_size) = nullptr;
    // What `compr_data` said, for the methods that need it (LZMA's properties).
    const std::uint8_t* parameters = nullptr;
    std::size_t parameters_size = 0;
};

// An RVZ or WIA file, presented as the disc inside it.
//
//     auto rvz = Rvz::Open(file, decompressor);
//     auto image = Image::Open(rvz->AsSource());
//
// A Wii disc read this way needs no cipher: the partitions are already
// decrypted, and the source says so.
class Rvz {
  public:
    static std::optional<Rvz> Open(Source file, Decompressor decompressor);

    Console GetConsole() const { return mConsole; }
    std::uint64_t DiscSize() const { return mDiscSize; }
    Compression Method() const { return mCompression; }

    // Reads the disc as it would be, except that a Wii partition's data comes
    // back decrypted and without its hash blocks - which is what `Image` reads
    // when told the source is already plain.
    Source AsSource() const;

    // Whether anything here needs a compressor the caller did not supply.
    bool Supported() const { return mSupported; }

    // Whether a Wii partition's data inside is already decrypted, which it is
    // in every RVZ and WIA. `Image` is told this so it does not look for a
    // cipher it does not need.
    bool Decrypted() const { return true; }

    // For the source handed out by AsSource().
    bool ReadThrough(std::uint64_t offset, std::uint8_t* out, std::size_t size) const;

  private:
    struct Group {
        std::uint64_t offset = 0;      // in the file
        std::uint32_t size = 0;        // compressed bytes, 0 for a run of zeros
        bool compressed = false;
        std::uint32_t packed_size = 0; // before the padding was regenerated
    };
    struct Region {
        std::uint64_t offset = 0;      // where this is presented
        std::uint64_t size = 0;
        std::uint32_t first_group = 0;
        std::uint32_t groups = 0;
        std::uint32_t chunk = 0;       // bytes of this region in one group
        std::uint32_t lists = 0;       // hash-exception lists in front of a chunk
    };

    bool ReadTables(const std::vector<std::uint8_t>& disc);
    bool ReadGroup(std::uint32_t index, const Region& region,
                   std::vector<std::uint8_t>& out) const;
    bool Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const;

    Source mFile;
    Decompressor mDecompressor;
    bool mIsRvz = true;
    Console mConsole = Console::Wii;
    Compression mCompression = Compression::None;
    std::uint32_t mChunkSize = 0;
    std::uint64_t mDiscSize = 0;
    bool mSupported = false;
    std::vector<Group> mGroups;
    std::vector<Region> mRegions;
    std::uint8_t mDiscHeader[128] = {};

    // The last group expanded, since reads walk a disc in order.
    mutable std::vector<std::uint8_t> mGroupBytes;
    mutable std::uint32_t mGroupIndex = UINT32_MAX;
};

}  // namespace wiinx::disc
