// Reading a GCZ file.
//
// Everything is little-endian here, unlike the discs themselves:
//
//   0x00  magic, 0xB10BC001
//   0x04  what kind of disc is inside: 0 GameCube, 1 Wii
//   0x08  how long the file's compressed data is
//   0x10  how long the disc is
//   0x18  the size of a block
//   0x1C  how many blocks there are
//   0x20  one offset per block, with the top bit set when that block was stored
//         as it is rather than deflated
//   then  one checksum per block, which nothing here verifies
//   then  the blocks
#include "wiinx/format/disc/gcz.hpp"

#include <algorithm>
#include <cstring>

namespace wiinx::disc {
namespace {

constexpr std::uint32_t kMagic = 0xB10BC001;
constexpr std::uint64_t kStoredPlainly = 0x8000000000000000ull;

std::uint32_t Read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Read64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(Read32(p)) |
           (static_cast<std::uint64_t>(Read32(p + 4)) << 32);
}

}  // namespace

std::optional<Gcz> Gcz::Open(Source file, Decompressor decompressor) {
    std::uint8_t header[0x20];
    if (!file.Read(0, header, sizeof(header)) || Read32(header) != kMagic) {
        return std::nullopt;
    }

    Gcz image;
    image.mFile = file;
    image.mDecompressor = decompressor;
    image.mConsole = Read32(header + 0x04) == 0 ? Console::GameCube : Console::Wii;
    image.mDiscSize = Read64(header + 0x10);
    image.mBlockSize = Read32(header + 0x18);

    const std::uint32_t blocks = Read32(header + 0x1C);
    if (image.mBlockSize == 0 || blocks == 0 || blocks > 8u * 1024u * 1024u) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(blocks) * 8);
    if (!file.Read(0x20, table.data(), table.size())) {
        return std::nullopt;
    }
    image.mBlocks.reserve(blocks);
    for (std::uint32_t index = 0; index < blocks; index++) {
        image.mBlocks.push_back(Read64(table.data() + index * 8));
    }

    // The checksums sit between the table and the data, one word each.
    image.mDataAt = 0x20 + static_cast<std::uint64_t>(blocks) * 8 +
                    static_cast<std::uint64_t>(blocks) * 4;
    return image;
}

bool Gcz::ReadBlock(std::uint64_t index, std::vector<std::uint8_t>& out) const {
    if (index >= mBlocks.size()) {
        return false;
    }
    if (mBlockIndex == index && mBlockBytes.size() == mBlockSize) {
        out = mBlockBytes;
        return true;
    }

    const std::uint64_t entry = mBlocks[static_cast<std::size_t>(index)];
    const bool plain = (entry & kStoredPlainly) != 0;
    const std::uint64_t at = mDataAt + (entry & ~kStoredPlainly);

    // A block's length is where the next one starts, since they follow one
    // another; the last runs to the end of what the header described.
    std::uint64_t next = 0;
    if (index + 1 < mBlocks.size()) {
        next = mDataAt + (mBlocks[static_cast<std::size_t>(index) + 1] & ~kStoredPlainly);
    } else {
        next = at + mBlockSize;  // no more table to consult; ask for a block's worth
    }
    if (next <= at) {
        return false;
    }
    const std::size_t stored = static_cast<std::size_t>(std::min<std::uint64_t>(
        next - at, static_cast<std::uint64_t>(mBlockSize) + 0x1000));

    std::vector<std::uint8_t> raw(stored);
    if (!mFile.Read(at, raw.data(), raw.size())) {
        return false;
    }

    if (plain) {
        raw.resize(mBlockSize, 0);
        out = std::move(raw);
    } else {
        if (mDecompressor.inflate == nullptr) {
            return false;
        }
        out.assign(mBlockSize, 0);
        const std::size_t written = mDecompressor.inflate(mDecompressor.context, Compression::Zlib,
                                                          raw.data(), raw.size(), out.data(),
                                                          out.size());
        if (written == 0) {
            return false;
        }
        out.resize(mBlockSize, 0);
    }

    mBlockBytes = out;
    mBlockIndex = index;
    return true;
}

bool Gcz::ReadThrough(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
    std::vector<std::uint8_t> block;
    while (size != 0) {
        const std::uint64_t index = offset / mBlockSize;
        const std::size_t within = static_cast<std::size_t>(offset % mBlockSize);
        const std::size_t take = std::min(size, static_cast<std::size_t>(mBlockSize) - within);

        if (!ReadBlock(index, block) || within >= block.size()) {
            return false;
        }
        const std::size_t available = std::min(take, block.size() - within);
        std::memcpy(out, block.data() + within, available);
        if (available < take) {
            std::memset(out + available, 0, take - available);
        }
        out += take;
        offset += take;
        size -= take;
    }
    return true;
}

namespace {

bool ReadGcz(void* context, std::uint64_t offset, std::uint8_t* out, std::size_t size) {
    return static_cast<const Gcz*>(context)->ReadThrough(offset, out, size);
}

}  // namespace

Source Gcz::AsSource() const { return Source{const_cast<Gcz*>(this), &ReadGcz}; }

}  // namespace wiinx::disc
