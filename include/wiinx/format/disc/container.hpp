#pragma once

// The shapes a dump arrives in.
//
// Almost nobody keeps a raw image: a GameCube disc is 1.35 GB and a Wii disc is
// 4.4 GB, so the community stores them compressed, scrubbed, or split to fit a
// FAT32 card. A container is a way of writing the same image down, and this
// layer turns one back into the reads `Image` expects - so a dump someone made
// years ago in whatever the tool of the day was still works.
//
//   raw    .iso, .gcm      the image itself
//   ciso   .ciso, .cso     the image with its empty blocks left out
//   wbfs   .wbfs           the USB loaders' own layout, one game per file
//   gcz    .gcz            Dolphin's older compressed format: zlib blocks
//   wia    .wia            its predecessor to RVZ
//   rvz    .rvz            Dolphin's current one: zstd blocks, padding regrown
//   nkit   .nkit.iso/.gcz  a preservation format that rebuilds the original
//
// Split files (`.wbf1`, `.part1.iso`) are the same container written across
// several files because FAT32 stops at 4 GB; they are joined before this sees
// them.

#include "wiinx/format/disc/image.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wiinx::disc {

enum class ContainerKind {
    Raw,
    Ciso,
    Wbfs,
    Gcz,
    Wia,
    Rvz,
    Nkit,
    Unknown,
};

// What a file says it is, from its first bytes. A file with no magic that could
// be a disc is Raw.
ContainerKind Identify(const Source& file);

// Its name, for a message to whoever is holding the file.
std::string_view Name(ContainerKind kind);

// Whether this library can read that kind yet.
bool Readable(ContainerKind kind);

// A container, presented as the image inside it.
//
//     auto container = Container::Open(file);
//     auto image = Image::Open(container->AsSource(), cipher);
//
// The source points at the container, so the container must outlive it and must
// stay where it is: take the source after the container has found its home.
class Container {
  public:
    static std::optional<Container> Open(Source file);

    ContainerKind Kind() const { return mKind; }
    Source AsSource() const;

    // How large the image inside is, when the container says.
    std::uint64_t ImageSize() const { return mImageSize; }

  private:
    Container() = default;
    bool OpenCiso();
    bool OpenWbfs();

    ContainerKind mKind = ContainerKind::Raw;
    Source mFile;
    std::uint64_t mImageSize = 0;

    // Both readable containers are a map from a block of the image to where
    // that block sits in the file, with 0 meaning "a block of zeros".
    std::uint32_t mBlockSize = 0;
    std::vector<std::uint64_t> mBlocks;
};

}  // namespace wiinx::disc
